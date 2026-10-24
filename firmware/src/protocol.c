/**
 * LeekWallet protocol endpoint — device side.
 *
 * Speaks the wire protocol from docs/PROTOCOL.md over the ESP32-S3's native
 * USB-Serial-JTAG, which is the same port the console logs to and the same
 * cable used for flashing. No extra hardware, no TinyUSB reconfiguration.
 *
 * Sharing the port with log output means the host has to find frames in a
 * stream that also carries text, so every frame is preceded by a two-byte sync
 * marker. Log lines are UTF-8 and essentially never contain it, and a receiver
 * that loses sync simply scans forward to the next marker rather than
 * misparsing. A dedicated CDC interface (T25b) removes the need, but this works
 * over the cable already attached.
 *
 *   ┌──────┬──────┬────────┬─────────────┐
 *   │ 'L'  │ 'K'  │ len:u16│ type + CBOR │
 *   └──────┴──────┴────────┴─────────────┘
 *
 * The Ethereum commands LeekWallet carried (getAddress, signTransaction,
 * signMessage, signTypedData) are gone (roadmap 1.1). What remains is the
 * chain-agnostic core: the handshake, device and wallet state, unlock and
 * lock, wallet selection and the host-supplied passphrase. Zcash commands
 * arrive in later phases, behind the same session gate.
 */

#include "protocol.h"
#include "version.h"
#include "board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"

#include "cbor.h"
#include "memzero.h"
#include "leek-wallet.h"
#include "pin.h"
#include "session.h"
#include "ui.h"
#if LEEK_HAS_ZCASH
#include "zcash-account.h"
#include "zcash-meta.h"
#include "zcash-sign.h"
#else
/* No Zcash signer on this board (the Pixie), but getStatus still reports
 * the Settings > Network choice, whose values are zsign's: 0 is Mainnet
 * (ui.h, ui_zcash_network). */
#define ZCASH_NET_MAIN 0
#endif

static const char *TAG = "protocol";

#define SYNC0 'L'
#define SYNC1 'K'

#define MAX_FRAME PROTOCOL_MAX_FRAME  /* above anything the device answers today */
#define RX_CHUNK  64

/* Frame types, matching app/packages/core/src/framing.ts */
#define FRAME_REQUEST   0x01
#define FRAME_RESPONSE  0x02
#define FRAME_ENC_REQUEST  0x11
#define FRAME_ENC_RESPONSE 0x12
#define FRAME_ENC_ERROR    0x7E
#define FRAME_ERROR     0x7F

/* Error codes, matching transport.ts */
#define ERR_MALFORMED    0x0001
/* The host speaks a protocol this firmware does not. Its own code rather than
 * ERR_MALFORMED because the two call for opposite responses: a malformed frame
 * is a bug to retry past, a version mismatch is a build to replace, and the
 * user should be told which. transport.ts has called this UnsupportedVersion
 * since before anything sent it. */
#define ERR_VERSION      0x0002
#define ERR_NOT_UNLOCKED 0x0100
#define ERR_NO_WALLET    0x0300
#define ERR_SESSION      0x0400
/* The transport could not take the request in. Distinct from a session error:
 * nothing was parsed, nothing was decrypted, and retrying is the right move.
 * Exists so that no transport may answer a complete request with silence. */
#define ERR_BUSY         0x0401
#define ERR_USER_REJECTED 0x0200
#define ERR_USER_TIMEOUT  0x0201
/* The device checked a PCZT and will not sign it. The low byte is the
 * signer's reason (-ZS_E_* / -ZSIGN_E_*), so a host can tell "the hash did
 * not match" from "an output was tampered with" without parsing text. */
#define ERR_ZSIGN_BASE    0x0500

static uint8_t rx_buf[MAX_FRAME];
static size_t  rx_len = 0;

/* Where replies go. NULL means the USB endpoint below; BLE installs its own
 * writer when it becomes the selected transport (see transport.c). Only one is
 * ever installed, because only one transport is ever live — PROTOCOL.md 3b. */
static ProtocolWriter tx_writer = NULL;

/* Whether the USB port is an endpoint at all. Cleared while BLE is selected. */
/* Off until a transport is chosen. Defaulting this to true meant the USB
 * endpoint answered during boot before transport_init() had run, which is both
 * a transport-exclusivity hole and the window a remote crash was reached
 * through. transport_apply() always sets it explicitly, so nothing depends on
 * the initialiser being permissive. */
static bool rx_enabled = false;

/**
 * Whether the request currently being handled arrived encrypted.
 *
 * A reply belongs to the frame that caused it, not to the state the device
 * happens to be in. Dispatching on state answered a PLAINTEXT request with an
 * ENCRYPTED reply whenever a session existed, and that is worse than untidy:
 *
 *   - The host that sent it had no session (that is why it was in plaintext),
 *     so it had no keys, so it CBOR-decoded raw ciphertext and reported
 *     "unsupported CBOR major type 7" — random bytes spelled out as a data
 *     format instead of a clean "no session". Seen on hardware.
 *   - It desynchronises the very counters the encrypt-errors rule protects.
 *     session_encrypt() advances the device's tx counter; the host advances
 *     its rx counter only when it opens a reply (session.ts decrypt()). A
 *     reply the host cannot open leaves the device→host stream one ahead
 *     forever.
 *
 * Replying in kind costs nothing in either direction. A plaintext request
 * never advanced the device's rx counter — only session_decrypt() does — and
 * a plaintext reply never advances the host's tx counter, because the host
 * defers that until it opens a reply (the asymmetry documented in session.ts,
 * which exists so a request rejected while the user has not pressed ALLOW can
 * be retried). Both sides therefore stand still, which is exactly right for an
 * exchange that touched neither key.
 *
 * The rule this flag enforces: an ENCRYPTED reply is only ever produced for a
 * request that was successfully DECRYPTED. Everything else — including errors
 * about frames that never decrypted — goes back the way it came.
 */
static bool reply_encrypted = false;

/* Reads the flag transport_apply() owns. Exists so the host suite can assert
 * the boot default is off, which is the half of the fix a behavioural test
 * cannot reach: by the time a fixture has run, the flag has been set. */
bool protocol_rx_enabled(void)
{
    return rx_enabled;
}

void protocol_set_rx_enabled(bool enabled)
{
    rx_enabled = enabled;
    rx_len = 0;
}

void protocol_set_writer(ProtocolWriter writer)
{
    tx_writer = writer;
}

/**
 * The sync marker is a property of the USB port, not of the protocol.
 *
 * This port also carries console logs, so a frame has to be findable in a
 * stream of text. BLE does not share its channel with anything, and GATT
 * already delimits every write, so the marker is emitted and required here and
 * nowhere else. See PROTOCOL.md section 2.
 */
/*
 * Logs share this port, and a log line written between a frame's bytes
 * corrupts the frame: seen on the first board, where a watchdog report
 * landed right after "LK" and the companion waited for a reply it could no
 * longer parse. So the marker and the frame go out in one write, and task
 * logs take the same lock (log_vprintf below) so they wait for the frame.
 * Logs printed from an interrupt cannot wait; nothing should be printing
 * from one in normal operation.
 */
#ifndef LEEK_HOST_TEST
#include "freertos/semphr.h"
static SemaphoreHandle_t usb_tx_lock;
static vprintf_like_t    log_vprintf_next;

static int log_vprintf(const char *fmt, va_list ap)
{
    if (usb_tx_lock && xPortCanYield()) {
        xSemaphoreTakeRecursive(usb_tx_lock, portMAX_DELAY);
        int n = log_vprintf_next(fmt, ap);
        xSemaphoreGiveRecursive(usb_tx_lock);
        return n;
    }
    return log_vprintf_next(fmt, ap);
}

static void usb_tx_lock_init(void)
{
    if (!usb_tx_lock) {
        usb_tx_lock = xSemaphoreCreateRecursiveMutex();
        log_vprintf_next = esp_log_set_vprintf(log_vprintf);
    }
}
#endif

static void usb_write_frame(const uint8_t *frame, size_t len)
{
    static uint8_t out[2 + MAX_FRAME + 2];
#ifndef LEEK_HOST_TEST
    usb_tx_lock_init();
    xSemaphoreTakeRecursive(usb_tx_lock, portMAX_DELAY);
#endif
    if (len > sizeof(out) - 2) {
        len = sizeof(out) - 2;
    }
    out[0] = SYNC0;
    out[1] = SYNC1;
    memcpy(out + 2, frame, len);
    /* In pieces the driver's ring buffer can take: a single larger write is
     * not queued at all (see PROTOCOL_USB_TX_BUF). */
    size_t sent = 0, total = len + 2;
    while (sent < total) {
        size_t n = total - sent;
        if (n > PROTOCOL_USB_TX_CHUNK) n = PROTOCOL_USB_TX_CHUNK;
        int w = usb_serial_jtag_write_bytes(out + sent, n, portMAX_DELAY);
        if (w <= 0) {
            break;      /* driver gone; nothing more can be sent */
        }
        sent += (size_t)w;
    }
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
#ifndef LEEK_HOST_TEST
    xSemaphoreGiveRecursive(usb_tx_lock);
#endif
}

static void send_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    /* One assembled frame, so every transport ships the identical bytes and
     * the marker stays the USB writer's business. Static rather than on the
     * stack: this runs on a 4 KB task. */
    static uint8_t tx_buf[MAX_FRAME];

    size_t body = len + 1;      /* type byte counts toward the length */
    if (body + 2 > sizeof(tx_buf)) {
        ESP_LOGE(TAG, "Refusing to send an oversized frame");
        return;
    }

    tx_buf[0] = (uint8_t)(body >> 8);
    tx_buf[1] = (uint8_t)body;
    tx_buf[2] = type;
    if (len) {
        memcpy(tx_buf + 3, payload, len);
    }

    if (tx_writer) {
        tx_writer(tx_buf, body + 2);
    } else {
        usb_write_frame(tx_buf, body + 2);
    }
}

/**
 * Send an error.
 *
 * An error answering an ENCRYPTED request is encrypted like any other reply.
 * That is not about secrecy - it is about counters. The device advances its
 * receive counter the moment a frame decrypts, even if the reply is an error,
 * while the host only advances its send counter when it opens a reply. A
 * plaintext error therefore leaves the two one apart, and every later frame
 * fails to decrypt. Hardware testing walked straight into it: an unimplemented
 * method returned a plaintext error and the next request died with "decrypt
 * failed".
 *
 * An error answering a PLAINTEXT request stays in plaintext, for the mirror
 * reason: nothing decrypted, so no counter moved, and a reply the host cannot
 * open is the desync rather than the cure. See reply_encrypted above.
 *
 * Session-level errors stay in plaintext, because at that point there is no
 * working channel to send them over.
 */
static void send_error_ex(uint16_t code, const char *message, bool allow_encrypt)
{
    /* Room for the longest message (the wrong-network one, 101 characters)
     * with the CBOR around it and the session tag. */
    uint8_t out[176];
    CborWriter w;
    cbor_writer_init(&w, out, sizeof(out));

    cbor_write_map(&w, 2);
    cbor_write_text(&w, "code");
    cbor_write_uint(&w, code);
    cbor_write_text(&w, "message");
    cbor_write_text(&w, message);

    if (!cbor_writer_ok(&w)) {
        return;
    }

    if (allow_encrypt && reply_encrypted && session_state() == SESSION_ACTIVE) {
        int enc = session_encrypt(out, w.length, sizeof(out));
        if (enc > 0) {
            /* A distinct type. Sending an encrypted error as an encrypted
             * *response* makes it indistinguishable from success once the
             * payload is opened, and the client reports an empty result rather
             * than the failure that actually happened. */
            send_frame(FRAME_ENC_ERROR, out, (size_t)enc);
            return;
        }
    }

    send_frame(FRAME_ERROR, out, w.length);
}

static void send_error(uint16_t code, const char *message)
{
    send_error_ex(code, message, true);
}

/** For failures that mean the channel itself is unusable. */
static void send_session_error(uint16_t code, const char *message)
{
    send_error_ex(code, message, false);
}

/**
 * A complete request the transport could not hand to the dispatcher.
 *
 * BLE is the only transport with a gate between "frame received" and "frame
 * dispatched" — a queue, because a signing request blocks on a human and the
 * NimBLE host task cannot be the thread that waits. USB has no gate: whatever
 * consume() finds is dispatched inline. That difference is exactly the
 * divergence this exists to close.
 *
 * Plaintext, always: nothing was decrypted, so no counter moved on either
 * side, and a retry of the identical frame is correct. Silence, by contrast,
 * is unrecoverable — the host cannot tell a dropped request from a slow one,
 * and the protocol has no request IDs to resynchronise with (PROTOCOL.md 3b).
 */
void protocol_send_transport_busy(void)
{
    /* allow_encrypt = false: nothing here was decrypted, so this is a
     * plaintext reply whatever the session is doing. */
    send_error_ex(ERR_BUSY, "device busy; resend", false);
}

/**
 * Whether the request being handled arrived over the confirmed channel.
 *
 * Not "does a session exist" - "did THIS request come through it". The two are
 * different the moment a plaintext frame arrives while a session is up, and
 * the difference is the whole authentication story: a plaintext frame proves
 * nothing about who sent it, so answering one from the session's authority
 * would let anything plugged into the port ride a passkey the user confirmed
 * for somebody else. Enumerating a wallet, switching seeds or applying a
 * passphrase are all reachable that way.
 *
 * The old spelling, `session_state() != SESSION_ACTIVE`, performed the
 * operation for such a frame and merely encrypted the answer - so the reply
 * was unreadable, but the device had already done as it was told.
 */
static bool request_is_authenticated(void)
{
    return reply_encrypted && session_state() == SESSION_ACTIVE;
}

/* Handle the plaintext handshake. Runs before any session exists, so it must
 * reveal nothing: a device public key, a commitment and a version, and no
 * user-specific state at all.
 *
 * Both legs report their own failures, because the failures are worth telling
 * apart. A single "handshake failed" was what let M-2 and the missing version
 * check hide behind each other: an old host and a corrupt frame produced the
 * same sentence, so nobody could act on either. */
static bool handle_hello(const uint8_t *payload, size_t len,
                         uint8_t *out, size_t out_size, size_t *out_len)
{
    /* M-2. A plaintext `hello` is unauthenticated — anything with write access
     * to the port or the characteristic can send one — and session_begin()
     * resets the session and puts a Connect? screen up. While the user is
     * answering a question the DEVICE asked them, that is an interruption an
     * attacker gets to schedule: a signing approval replaced mid-read by a
     * pairing prompt, with the sign request silently dropped.
     *
     * So a handshake waits while a confirmation is on screen. Deliberately not
     * "refuse whenever a session is ACTIVE": there is no disconnect event on
     * USB, so an app that crashed and restarted would be locked out until the
     * device was power-cycled, and reconnecting is the common case while this
     * is the rare one. */
    if (ui_user_is_answering()) {
        send_session_error(ERR_BUSY, "the device is waiting for the user; retry");
        return false;
    }

    /* Version first, so a mismatch is reported as a mismatch rather than
     * surfacing later as a decrypt failure against keys derived from labels
     * the other end never used. An old host sends no `version` at all, which
     * is a mismatch and is named as one. */
    CborItem ver;
    uint64_t offered = 0;
    if (cbor_map_find(payload, len, "version", &ver) && ver.type == CBOR_UINT) {
        offered = ver.value;
    }
    if (offered != PROTOCOL_VERSION) {
        char why[80];
        snprintf(why, sizeof(why),
                 "this device speaks protocol v%u; the app offered v%u — update the older one",
                 (unsigned)PROTOCOL_VERSION, (unsigned)offered);
        send_session_error(ERR_VERSION, why);
        return false;
    }

    CborItem item;
    if (!cbor_map_find(payload, len, "hostPubkey", &item) ||
        item.type != CBOR_BYTES || item.value != SESSION_PUBKEY_SIZE) {
        send_session_error(ERR_MALFORMED, "hello needs a 32-byte hostPubkey");
        return false;
    }

    uint8_t device_pub[SESSION_PUBKEY_SIZE];
    uint8_t device_commit[SESSION_COMMIT_SIZE];
    if (!session_begin(item.data, device_pub, device_commit)) {
        send_session_error(ERR_SESSION, "handshake failed");
        return false;
    }

    /* Nothing goes on screen yet. The passkey does not exist until the host
     * reveals its nonce, and showing a prompt for a value that has not been
     * computed is how a user learns to press ALLOW without reading. */
    CborWriter w;
    cbor_writer_init(&w, out, out_size);
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "result");
    cbor_write_map(&w, 3);
    cbor_write_text(&w, "version");
    cbor_write_uint(&w, PROTOCOL_VERSION);
    cbor_write_text(&w, "devicePubkey");
    cbor_write_bytes(&w, device_pub, sizeof(device_pub));
    cbor_write_text(&w, "deviceCommit");
    cbor_write_bytes(&w, device_commit, sizeof(device_commit));

    if (!cbor_writer_ok(&w)) {
        session_reset();
        send_session_error(ERR_MALFORMED, "handshake reply did not fit");
        return false;
    }
    *out_len = w.length;
    return true;
}

/* Second leg: the host reveals its nonce and the device reveals the one it
 * committed to. Only now can either side derive, and only now does the passkey
 * exist to be displayed. */
static bool handle_hello_reveal(const uint8_t *payload, size_t len,
                                uint8_t *out, size_t out_size, size_t *out_len)
{
    /* M-2 again, on the leg that actually paints. Gating only `hello` gated
     * the wrong half: `hello` deliberately shows nothing — the passkey does
     * not exist yet — and it is this leg that calls
     * ui_request_session_confirm(). A peer could therefore open a handshake at
     * a quiet moment, sit in AWAITING_REVEAL for as long as it liked (nothing
     * times it out, and USB has no disconnect), and spend the reveal at a
     * moment of its own choosing to wipe a seed off the glass or a wipe
     * confirmation out from under the user.
     *
     * Deferred rather than refused, and BEFORE session_reveal(), so the
     * outstanding commitment survives and the same reveal succeeds once the
     * user has answered. Refusing would consume the commitment and force a
     * fresh `hello`, which is the lockout this gate exists to avoid. */
    if (ui_user_is_answering()) {
        send_session_error(ERR_BUSY, "the device is waiting for the user; retry");
        return false;
    }

    CborItem item;
    if (!cbor_map_find(payload, len, "hostNonce", &item) ||
        item.type != CBOR_BYTES || item.value != SESSION_NONCE_SIZE) {
        send_session_error(ERR_MALFORMED, "helloReveal needs a 16-byte hostNonce");
        return false;
    }

    uint8_t device_nonce[SESSION_NONCE_SIZE];
    if (!session_reveal(item.data, device_nonce)) {
        /* Either no commitment is outstanding, or the peer key was
         * small-order. Both mean there is no channel and no half-state left
         * behind — session_reveal() tore it down. */
        send_session_error(ERR_SESSION, "no handshake is waiting for a nonce");
        return false;
    }

    /* The user now has to compare a six-digit code on the OLED with the one
     * the app computes. Until they confirm, nothing encrypted is accepted. */
    ui_request_session_confirm();

    CborWriter w;
    cbor_writer_init(&w, out, out_size);
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "result");
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "deviceNonce");
    cbor_write_bytes(&w, device_nonce, sizeof(device_nonce));

    if (!cbor_writer_ok(&w)) {
        session_reset();
        send_session_error(ERR_MALFORMED, "handshake reply did not fit");
        return false;
    }
    *out_len = w.length;
    return true;
}

/* A passphrase this device did not see typed (T42).
 *
 * Set once the user has approved a host-supplied passphrase, and the reason
 * the session-reset hook exists. A passphrase entered ON the device belongs to
 * the person holding it and outlives any host; one typed INTO a host belongs
 * to that host's session, and when the session dies - disconnect, transport
 * switch, a new peer, a frame that failed to authenticate - the wallet it
 * selected must die with it. Otherwise the next thing to connect inherits a
 * hidden wallet it never asked for and the user never re-approved, and every
 * address and signature it gets comes from a seed nothing on screen chose.
 */
static bool host_passphrase_applied = false;

/* Set by the `lock` handler, honoured once its reply is on the wire.
 *
 * lock_device() drops the session, and the UI task is what runs it -- so
 * without this the channel could close in the ~100 us between ui_request_lock()
 * and the reply being encrypted, and the host would get no answer to a call it
 * made itself. Rare, non-deterministic, and exactly the kind of thing that
 * costs a day to find later. */
static bool drop_session_after_reply = false;

void protocol_note_device_passphrase(void)
{
    host_passphrase_applied = false;
}

static void host_passphrase_forget(void)
{
    if (!host_passphrase_applied) {
        return;
    }
    host_passphrase_applied = false;
    ESP_LOGW(TAG, "Session ended; dropping the host-supplied passphrase");
    wallet_clear_passphrase();
}

/**
 * Block until the user answers the prompt on screen, or long enough that
 * nobody is going to.
 *
 * Shared by every confirmation - today only the passphrase - because a
 * second copy of this loop is a second place for a request to be answered
 * without anyone having looked at the device. Returns the outcome; the caller
 * owns whatever it has to undo on anything but SIGN_APPROVED.
 */
static SignOutcome wait_for_user(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(120000);
    for (;;) {
        SignOutcome outcome = ui_sign_outcome();
        if (outcome != SIGN_PENDING) {
            ui_sign_clear();
            return outcome;
        }
        if (xTaskGetTickCount() > deadline) {
            ui_sign_clear();
            /* Clearing the request is not the same as taking the question off
             * the screen, and for a long time only the first happened. See
             * ui_sign_expire(). */
            ui_sign_expire();
            return SIGN_PENDING;        /* nobody answered */
        }
        /* A user standing at the device reading a question is not an idle
         * session, but no frames cross the wire while they do it. Without
         * this the idle timeout would tear the channel down mid-approval --
         * H-1's shape, reintroduced as a feature. */
        session_note_activity();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#if LEEK_HAS_ZCASH
/*
 * Wait for the open wallet to be usable, after the PIN gate has opened.
 *
 * The PIN gate opens the moment the PIN is accepted, and getStatus reports
 * unlocked from then on, but the UI task only then decrypts the vault
 * (another PBKDF2, about 1.5 s on the S3) and selects the stored wallet. A
 * companion that unlocked the device and asked for its account straight
 * away met a closed vault and was told "derivation failed" - seen on the
 * first board. So a Zcash request waits for the wallet, briefly, and says
 * which of the two it is when it gives up.
 */
/*
 * Ready means a seed in RAM, not `unlocked` with a wallet selected: the
 * vault publishes `unlocked` before it decrypts the active seed
 * (wallet_unlock), and drops the seed before loading the next one
 * (wallet_select_wallet, which the home screen calls after an unlock when
 * no wallet is active). A request that landed in that window passed this
 * wait and was then told by the derivation "no wallet on the device: create
 * or import one" - seen over Bluetooth right after connecting, where a
 * retry a moment later worked.
 */
static WalletError wait_for_wallet(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    for (;;) {
        WalletStatus st = wallet_get_status();
        const bool temp = wallet_has_temporary_mnemonic();
        if (st.unlocked && st.has_mnemonic && (temp || st.active_wallet_index > 0)) {
            return WALLET_OK;
        }
        if (!pin_is_unlocked()) {
            return WALLET_ERROR_LOCKED;
        }
        if (st.unlocked && st.wallet_count == 0 && !temp) {
            return WALLET_ERROR_NO_MNEMONIC;
        }
        if (xTaskGetTickCount() > deadline) {
            return st.unlocked ? WALLET_ERROR_NO_MNEMONIC : WALLET_ERROR_LOCKED;
        }
        session_note_activity();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* A wallet error from a Zcash request, said in words a user can act on. */
static void send_wallet_error(WalletError err)
{
    switch (err) {
        case WALLET_ERROR_NO_MNEMONIC:
            /* A board that holds a wallet is not told to create one: its
             * seed was not in RAM yet (still opening, or being switched).
             * Same code, so a companion's retry covers both. */
            if (wallet_get_status().wallet_count > 0 || wallet_has_temporary_mnemonic()) {
                send_error(ERR_NO_WALLET, "the wallet is still opening on the device; try again");
            } else {
                send_error(ERR_NO_WALLET, "no wallet on the device: create or import one");
            }
            break;
        case WALLET_ERROR_LOCKED:
            send_error(ERR_NOT_UNLOCKED, "the wallet is not open yet; unlock the device");
            break;
        default:
            send_error(ERR_NO_WALLET, "derivation failed");
            break;
    }
}

/* Payload bytes per zSignChunk: a 4096-byte frame less the frame header,
 * the session tag and the CBOR around the data. */
#define ZSIGN_MAX_CHUNK 3072

static void send_zsign_refusal(int rc)
{
    send_error((uint16_t)(ERR_ZSIGN_BASE | ((unsigned)-rc & 0xFF)), zcash_sign_strerror(rc));
}

static const char *network_title(uint8_t network)
{
    return network == ZCASH_NET_MAIN ? "Mainnet" : "Testnet";
}

/* The board answers only for the network its owner chose under Settings >
 * Network. A board set to Testnet once handed a mainnet app the mainnet
 * account without a word, and an owner who believes the board is on the
 * other network can send real funds believing them play money, or wait
 * for funds on an address they are not watching. So a request for the other
 * network is refused before anything is derived, with a reason that names
 * both, and the board says so on its screen. True if `network` is the
 * board's; otherwise the refusal has been sent.
 *
 * ui_zcash_network() is one byte the UI task writes and this task reads:
 * the read cannot tear, and a switch made while a request is in flight
 * applies from the next request. */
static bool network_is_the_boards(uint8_t network)
{
    const uint8_t board = ui_zcash_network();
    if (network == board) {
        return true;
    }
    char why[112];
    snprintf(why, sizeof why,
             "the board is set to %s; switch it to %s in Settings > Network, or switch the app to %s",
             network_title(board), network_title(network), network_title(board));
    ui_notify_wrong_network(network);
    send_error((uint16_t)(ERR_ZSIGN_BASE | (unsigned)-ZSIGN_E_BOARD_NETWORK), why);
    return false;
}

/* zSignBegin / zSignChunk / zSignFinish. Writes the result into `w` and
 * returns true, or sends an error and returns false. */
static bool handle_zsign(const char *method, const uint8_t *payload, size_t len, CborWriter *w)
{
    CborItem item;
    int rc;

    if (strcmp(method, "zSignBegin") == 0) {
        /* Before any upload: a PCZT for a wallet that is not open yet would
         * only be refused at the end, after every chunk had crossed. */
        WalletError werr = wait_for_wallet();
        if (werr != WALLET_OK) {
            send_wallet_error(werr);
            return false;
        }
        uint32_t total = 0, account = 0;
        if (!cbor_map_find(payload, len, "len", &item) || item.type != CBOR_UINT) {
            send_error(ERR_MALFORMED, "len required");
            return false;
        }
        total = item.value;
        if (cbor_map_find(payload, len, "account", &item)) {
            if (item.type != CBOR_UINT) {
                send_error(ERR_MALFORMED, "account out of range");
                return false;
            }
            account = item.value;
        }
        CborItem hash;
        if (!cbor_map_find(payload, len, "sha256", &hash) || hash.type != CBOR_BYTES ||
            hash.value != 32) {
            send_error(ERR_MALFORMED, "sha256 must be 32 bytes");
            return false;
        }
        char net_name[8];
        if (!cbor_map_find(payload, len, "network", &item) ||
            !cbor_text_copy(&item, net_name, sizeof(net_name)) ||
            (strcmp(net_name, "main") != 0 && strcmp(net_name, "test") != 0)) {
            send_error(ERR_MALFORMED, "network must be \"main\" or \"test\"");
            return false;
        }
        const uint8_t network = strcmp(net_name, "main") == 0 ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
        if (!network_is_the_boards(network)) {
            return false;
        }
        rc = zcash_sign_begin(total, hash.data, account, network);
        if (rc != ZS_OK) {
            send_zsign_refusal(rc);
            return false;
        }
        cbor_write_map(w, 1);
        cbor_write_text(w, "result");
        cbor_write_map(w, 2);
        cbor_write_text(w, "maxChunk");
        cbor_write_uint(w, ZSIGN_MAX_CHUNK);
        cbor_write_text(w, "backend");
        cbor_write_text(w, zcash_sign_backend_name(zcash_sign_backend()));
        return true;
    }

    if (strcmp(method, "zSignChunk") == 0) {
        CborItem data;
        if (!cbor_map_find(payload, len, "offset", &item) || item.type != CBOR_UINT ||
            !cbor_map_find(payload, len, "data", &data) || data.type != CBOR_BYTES ||
            data.value == 0 || data.value > ZSIGN_MAX_CHUNK) {
            send_error(ERR_MALFORMED, "offset and 1..3072 bytes of data required");
            return false;
        }
        rc = zcash_sign_chunk(item.value, data.data, data.value);
        if (rc != ZS_OK) {
            send_zsign_refusal(rc);
            return false;
        }
        cbor_write_map(w, 1);
        cbor_write_text(w, "result");
        cbor_write_map(w, 1);
        cbor_write_text(w, "received");
        cbor_write_uint(w, zcash_sign_received());
        return true;
    }

    /* zSignFinish: check, review, sign. The setting is checked again: it
     * may have been switched on the board while the PCZT was uploading. */
    if (zcash_sign_received() > 0 && !network_is_the_boards(zcash_sign_network())) {
        zcash_sign_reset();
        return false;
    }
    const zs_summary *summary = NULL;
    rc = zcash_sign_check(&summary);
    if (rc != ZS_OK) {
        send_zsign_refusal(rc);
        return false;
    }

    ui_request_zsign_review(summary, zcash_sign_network());
    SignOutcome outcome = wait_for_user();
    if (outcome != SIGN_APPROVED) {
        zcash_sign_reset();
        send_error(outcome == SIGN_PENDING ? ERR_USER_TIMEOUT : ERR_USER_REJECTED,
                   outcome == SIGN_PENDING ? "no answer on the device" : "rejected on device");
        return false;
    }

    /* The sighash goes back too: the host checks it against its own before
     * it applies anything, which catches a device and a host that disagree
     * about what was signed. Copied now, because signing consumes the
     * summary. */
    uint8_t sighash[32];
    memcpy(sighash, summary->sighash, sizeof sighash);
    static zs_sig sigs[ZS_MAX_SIGS];
    uint32_t n = 0;
    rc = zcash_sign_sign(sigs, ZS_MAX_SIGS, &n);
    if (rc != ZS_OK) {
        send_zsign_refusal(rc);
        return false;
    }
    cbor_write_map(w, 1);
    cbor_write_text(w, "result");
    cbor_write_map(w, 2);
    cbor_write_text(w, "sighash");
    cbor_write_bytes(w, sighash, sizeof sighash);
    cbor_write_text(w, "signatures");
    cbor_write_array(w, n);
    for (uint32_t i = 0; i < n; i++) {
        bool t = sigs[i].pool == ZS_POOL_TRANSPARENT;
        cbor_write_map(w, t ? 4 : 3);
        cbor_write_text(w, "pool");
        cbor_write_uint(w, sigs[i].pool);
        cbor_write_text(w, "index");
        cbor_write_uint(w, sigs[i].index);
        cbor_write_text(w, "sig");
        if (t) {
            /* DER || sighash type, and the key (PROTOCOL.md, roadmap 10.1). */
            uint8_t der[ZCASH_TSIG_MAX];
            size_t dl = zcash_tsig_encode(&sigs[i], der);
            cbor_write_bytes(w, der, dl);
            cbor_write_text(w, "pubkey");
            cbor_write_bytes(w, sigs[i].pubkey, sizeof sigs[i].pubkey);
        } else {
            cbor_write_bytes(w, sigs[i].sig, sizeof sigs[i].sig);
        }
    }
    memzero(sigs, sizeof sigs);
    return true;
}
#endif

/* Handle one decoded request. */
static void dispatch(const uint8_t *payload, size_t len)
{
    /* Cleared per request: every branch below can bail out through
     * send_error(), and a flag left armed would drop the *next* request's
     * session instead of this one's. */
    drop_session_after_reply = false;

    CborItem item;
    if (!cbor_map_find(payload, len, "method", &item)) {
        send_error(ERR_MALFORMED, "no method");
        return;
    }

    char method[32];
    if (!cbor_text_copy(&item, method, sizeof(method))) {
        send_error(ERR_MALFORMED, "bad method");
        return;
    }
    /* One line per request: the method name only, never a field. What the
     * first board's bring-up needed to tell a lost request from a slow one. */
    ESP_LOGI(TAG, "request %s", method);

    /* Static, not stack: a Zcash account reply (UFVK + UA + t-address) is
     * ~700 bytes, a zSignFinish reply up to ~2.8 KB of signatures, and this
     * runs on a 10 KB task. dispatch() is only ever entered from the one
     * active transport (PROTOCOL.md 3b), never two at once. The frame header
     * and the session tag fit in what is left of PROTOCOL_MAX_FRAME. */
    static uint8_t out[PROTOCOL_MAX_FRAME - 64];
    CborWriter w;
    cbor_writer_init(&w, out, sizeof(out));

    if (strcmp(method, "ping") == 0) {
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "pong");
        cbor_write_uint(&w, 1);

    } else if (strcmp(method, "getFeatures") == 0) {
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 3);
        /* Which chain this firmware speaks. Was implied - LeekWallet only
           ever spoke Ethereum and answered with no chain at all - and a host
           built for that firmware must be able to tell it is not talking to
           one. No signing command exists yet (roadmap phase 1). */
        cbor_write_text(&w, "chain");
        cbor_write_text(&w, "zcash");
        cbor_write_text(&w, "firmware");
        cbor_write_text(&w, LEEK_FW_VERSION);   /* version.h, the one place it is set */
        cbor_write_text(&w, "model");
        /* From board.h, not a literal. A host that cannot tell which board it
           is talking to cannot warn about a capability the board lacks -- the
           Pixie has no camera, so airgapped signing will never be offered
           there, and the model string is how the app knows without guessing. */
        cbor_write_text(&w, BOARD_MODEL);

    } else if (strcmp(method, "getStatus") == 0) {
        WalletStatus status = wallet_get_status();
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 7);
        /* The account the device's own screens are on. Reported so the host can
         * follow it: the app invalidates everything it derived whenever this
         * changes, exactly as it does for the passphrase, which stops the
         * device browsing one account while the app lists another. */
        cbor_write_text(&w, "account");
        cbor_write_uint(&w, ui_hd_account());
        cbor_write_text(&w, "activeWallet");
        cbor_write_uint(&w, status.active_wallet_index);
        /* The network chosen under Settings > Network, "main" or "test". The
         * board refuses z* requests for the other one (0x0528), and a
         * companion that reads this first can say so, or follow the board,
         * before it asks. Not a secret: the board shows it on its screen. */
        cbor_write_text(&w, "network");
        cbor_write_text(&w, ui_zcash_network() == ZCASH_NET_MAIN ? "main" : "test");
        /* Temporary mode, said out loud.
         *
         * `activeWallet` is 0 throughout it -- deliberately, because no stored
         * seed is selected -- and a host with only that number cannot tell
         * "signing from a seed this device will never keep" from "something is
         * wrong". The app rendered `wallet 0/5`, which reads as a fault.
         *
         * It is not a secret: it says a seed is in RAM, never what it is, and
         * the same fact is on the device's own screen the whole time. */
        cbor_write_text(&w, "temporary");
        cbor_write_uint(&w, wallet_has_temporary_mnemonic() ? 1 : 0);
        cbor_write_text(&w, "passphrase");
        cbor_write_uint(&w, wallet_has_passphrase() ? 1 : 0);
        cbor_write_text(&w, "unlocked");
        cbor_write_uint(&w, pin_is_unlocked() ? 1 : 0);
        cbor_write_text(&w, "walletCount");
        cbor_write_uint(&w, status.wallet_count);

    } else if (strcmp(method, "unlock") == 0) {
        /* Never carries a PIN. It asks the device to prompt, the user types on
         * the device, and the host learns the outcome by polling getStatus.
         * A PIN crossing the wire would defeat the point of having one. */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }

        if (pin_is_unlocked()) {
            cbor_write_map(&w, 1);
            cbor_write_text(&w, "result");
            cbor_write_map(&w, 1);
            cbor_write_text(&w, "unlocked");
            cbor_write_uint(&w, 1);
        } else {
            ui_request_unlock();
            cbor_write_map(&w, 1);
            cbor_write_text(&w, "result");
            cbor_write_map(&w, 2);
            cbor_write_text(&w, "prompted");
            cbor_write_uint(&w, 1);
            cbor_write_text(&w, "unlocked");
            cbor_write_uint(&w, 0);
        }

    } else if (strcmp(method, "lock") == 0) {
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        ui_request_lock();
        /* The lock takes the channel with it (see lock_device()), but not
         * before this answer lands. */
        drop_session_after_reply = true;
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "unlocked");
        cbor_write_uint(&w, 0);

    } else if (strcmp(method, "selectWallet") == 0) {
        /* Which stored seed is active. No confirmation: it reveals nothing and
         * moves nothing, and every operation that does either names its own
         * address on screen afterwards. */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        if (!pin_is_unlocked()) {
            send_error(ERR_NOT_UNLOCKED, "device is locked");
            return;
        }

        if (!cbor_map_find(payload, len, "index", &item) || item.type != CBOR_UINT) {
            send_error(ERR_MALFORMED, "index required");
            return;
        }
        /* 1-based, and bounded before the cast: wallet_select_wallet takes a
         * uint8_t, so 257 would arrive as 1 and quietly select a wallet the
         * host did not ask for. */
        if (item.value < 1 || item.value > 0xFF ||
            wallet_select_wallet((uint8_t)item.value) != WALLET_OK) {
            send_error(ERR_NO_WALLET, "no such wallet");
            return;
        }

        /* Switching seeds drops the passphrase (PROTOCOL.md 5). A passphrase
         * belongs to the seed it was entered against; carrying it across would
         * silently land the user in a third wallet nobody named. */
        host_passphrase_applied = false;
        wallet_clear_passphrase();

        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "activeWallet");
        cbor_write_uint(&w, item.value);

    } else if (strcmp(method, "setPassphrase") == 0) {
        /* The app as a secure keyboard (PROTOCOL.md 5).
         *
         * This is the weaker of the two entry paths and it is not close: a
         * compromised host reads the passphrase as it is typed, before any
         * encryption applies. What makes it survivable is that the device
         * derives the resulting wallet and shows its fingerprint, and the user
         * recognises it - or does not, and the passphrase is dropped. That is
         * why the passphrase is never accepted without the confirmation below,
         * and why a rejection clears it rather than leaving it applied. */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        if (!pin_is_unlocked()) {
            send_error(ERR_NOT_UNLOCKED, "device is locked");
            return;
        }

        if (!cbor_map_find(payload, len, "passphrase", &item) ||
            item.type != CBOR_TEXT) {
            send_error(ERR_MALFORMED, "passphrase required");
            return;
        }

        char passphrase[64];
        size_t pass_len = item.value;
        /* An empty passphrase is the base wallet, not a passphrase. Making it
         * mean "clear" here would give one method two outcomes, one of which
         * changes wallets on an empty field. */
        if (pass_len == 0 || pass_len >= sizeof(passphrase)) {
            send_error(ERR_MALFORMED, "passphrase length out of range");
            return;
        }
        for (size_t i = 0; i < pass_len; i++) {
            /* Printable ASCII, matching what the device's own keyboard can
             * produce. A passphrase enterable from the app but not from the
             * device is a wallet the user cannot reach without the app. */
            if (item.data[i] < 0x20 || item.data[i] > 0x7E) {
                memzero(passphrase, sizeof(passphrase));
                send_error(ERR_MALFORMED, "passphrase must be printable ASCII");
                return;
            }
        }
        memcpy(passphrase, item.data, pass_len);
        passphrase[pass_len] = '\0';

        /* The frame was decrypted in place, so the plaintext passphrase is
         * still sitting in the receive buffer - a static .bss array that
         * nothing clears and that a later frame only partially overwrites.
         * Zero the bytes here, where their length is known; item.data points
         * into that buffer, which is writable for exactly this reason. */
        memzero((uint8_t *)item.data, pass_len);

        WalletError perr = wallet_set_passphrase(passphrase, pass_len);
        /* Gone from this frame the moment the wallet has it. It lives in RAM
         * for the session inside the wallet layer and nowhere else, and it is
         * never written to flash or logged. */
        memzero(passphrase, sizeof(passphrase));
        if (perr != WALLET_OK) {
            host_passphrase_applied = false;
            wallet_clear_passphrase();
            send_error(ERR_NO_WALLET, "could not apply the passphrase");
            return;
        }

        /* The seed fingerprint the user compares. Derived here, on the task
         * that applied the passphrase, which is the same rule as T47: the
         * screen must name what the device actually did.
         *
         * The fingerprint rather than an address: it identifies the SEED, which
         * is the question this screen asks, and it is the same for every chain
         * and account, and there is no Zcash address to show yet (roadmap
         * phase 1). */
        uint32_t pass_xfp = 0;
        if (wallet_get_master_fingerprint(&pass_xfp) != WALLET_OK) {
            host_passphrase_applied = false;
            wallet_clear_passphrase();
            send_error(ERR_NO_WALLET, "derivation failed");
            return;
        }

        ui_request_passphrase_confirm(pass_xfp);

        SignOutcome pass_outcome = wait_for_user();
        if (pass_outcome != SIGN_APPROVED) {
            /* Recoverable by construction: the device goes back to the wallet
             * it was in, and the host can try again. Leaving an unconfirmed
             * passphrase applied is how a user ends up signing from a wallet
             * they never agreed to. */
            host_passphrase_applied = false;
            wallet_clear_passphrase();
            send_error(pass_outcome == SIGN_PENDING ? ERR_USER_TIMEOUT
                                                    : ERR_USER_REJECTED,
                       pass_outcome == SIGN_PENDING ? "no answer on the device"
                                                    : "rejected on device");
            return;
        }

        /* Approved. From here the passphrase lives exactly as long as the
         * session that supplied it (T42) - see host_passphrase_forget(). */
        host_passphrase_applied = true;
        session_set_on_reset(host_passphrase_forget);

        /* The fingerprint, never the passphrase or anything derived from it
         * beyond what a watch-only descriptor already publishes. */
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 2);
        cbor_write_text(&w, "fingerprint");
        cbor_write_uint(&w, pass_xfp);
        cbor_write_text(&w, "passphrase");
        cbor_write_uint(&w, 1);

#if LEEK_HAS_ZCASH
    } else if (strcmp(method, "zGetAccount") == 0) {
        /* The account a companion imports: viewing key, address, and the
         * seed fingerprint that lets it tell this seed's accounts apart (and a
         * passphrase wallet from the base one). Viewing keys reveal history,
         * not spend authority, so this needs a session and an unlocked device
         * but no on-screen confirmation - the same footing as the xpub a
         * watch-only Bitcoin wallet asks for. Showing an address for the user
         * to compare is zShowAddress's job. */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        if (!pin_is_unlocked()) {
            send_error(ERR_NOT_UNLOCKED, "device is locked");
            return;
        }

        uint32_t account = 0;
        if (cbor_map_find(payload, len, "account", &item)) {
            if (item.type != CBOR_UINT || item.value >= 0x80000000u) {
                send_error(ERR_MALFORMED, "account out of range");
                return;
            }
            account = item.value;
        }
        char net_name[8];
        if (!cbor_map_find(payload, len, "network", &item) ||
            !cbor_text_copy(&item, net_name, sizeof(net_name)) ||
            (strcmp(net_name, "main") != 0 && strcmp(net_name, "test") != 0)) {
            /* Required, never defaulted: an account on the wrong network is a
             * set of addresses nobody on the intended one can pay. */
            send_error(ERR_MALFORMED, "network must be \"main\" or \"test\"");
            return;
        }
        uint8_t network = strcmp(net_name, "main") == 0 ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
        if (!network_is_the_boards(network)) {
            return;
        }

        static ZcashAccount acct;
        WalletError werr = wait_for_wallet();
        if (werr == WALLET_OK) {
            werr = zcash_account_derive(account, network, &acct);
        }
        if (werr != WALLET_OK) {
            send_wallet_error(werr);
            return;
        }
        /* The birthday, when the device has one for this account (a
         * companion set it, or the SD file carried it). Absent rather than
         * zero: zero would read as "scan from genesis". */
        ZcashMeta meta;
        const bool have_birthday = zmeta_get(account, network, &meta) && meta.birthday != 0;

        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, have_birthday ? 7 : 6);
        if (have_birthday) {
            cbor_write_text(&w, "birthday");
            cbor_write_uint(&w, meta.birthday);
        }
        cbor_write_text(&w, "account");
        cbor_write_uint(&w, acct.account);
        cbor_write_text(&w, "network");
        cbor_write_text(&w, net_name);
        cbor_write_text(&w, "seedFingerprint");
        cbor_write_bytes(&w, acct.keys.seed_fingerprint, sizeof acct.keys.seed_fingerprint);
        cbor_write_text(&w, "ufvk");
        cbor_write_text(&w, acct.keys.ufvk);
        cbor_write_text(&w, "ua");
        cbor_write_text(&w, acct.keys.ua);
        cbor_write_text(&w, "tAddress");
        cbor_write_text(&w, acct.t_address);

    } else if (strcmp(method, "zGetFeatures") == 0) {
        /* What this device's Zcash signer does, so a companion can decide
         * before building a transaction, rather than after the device
         * refuses it. Describes the firmware, not the wallet: readable
         * before a session, like getFeatures. */
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 8);
        cbor_write_text(&w, "backend");
        cbor_write_text(&w, zcash_sign_backend_name(zcash_sign_backend()));
        cbor_write_text(&w, "branchIds");
        cbor_write_array(&w, ZCASH_SIGN_BRANCH_COUNT);
        for (size_t i = 0; i < ZCASH_SIGN_BRANCH_COUNT; i++) {
            cbor_write_uint(&w, ZCASH_SIGN_BRANCHES[i]);
        }
        cbor_write_text(&w, "txVersions");
        cbor_write_array(&w, 1);
        cbor_write_uint(&w, 6);
        /* Spends the signer will sign. Outputs to any pool are reviewed. */
        cbor_write_text(&w, "spendPools");
        cbor_write_array(&w, 3);
        cbor_write_text(&w, "orchard");
        cbor_write_text(&w, "ironwood");
        cbor_write_text(&w, "transparent");   /* P2PKH of the account, roadmap 10.1 */
        cbor_write_text(&w, "maxPczt");
        cbor_write_uint(&w, ZCASH_PCZT_MAX);
        cbor_write_text(&w, "maxChunk");
        cbor_write_uint(&w, ZSIGN_MAX_CHUNK);
        cbor_write_text(&w, "storage");
        cbor_write_array(&w, LEEK_HAS_SDCARD ? 3 : 2);
        cbor_write_text(&w, "vault");
        cbor_write_text(&w, "stateless");
        if (LEEK_HAS_SDCARD) cbor_write_text(&w, "sd");
        cbor_write_text(&w, "showAddress");
        cbor_write_uint(&w, 1);

    } else if (strcmp(method, "zShowAddress") == 0) {
        /* The app shows an address; the device derives its own and pages it
         * for the user, who answers on the device whether they match. The
         * reply carries the device's address only after a yes, so a host
         * cannot use this to skip the user. */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        if (!pin_is_unlocked()) {
            send_error(ERR_NOT_UNLOCKED, "device is locked");
            return;
        }
        CborItem item;
        uint32_t account = 0;
        if (cbor_map_find(payload, len, "account", &item)) {
            if (item.type != CBOR_UINT || item.value >= 0x80000000u) {
                send_error(ERR_MALFORMED, "account out of range");
                return;
            }
            account = (uint32_t)item.value;
        }
        char net_name[8];
        if (!cbor_map_find(payload, len, "network", &item) ||
            !cbor_text_copy(&item, net_name, sizeof(net_name)) ||
            (strcmp(net_name, "main") != 0 && strcmp(net_name, "test") != 0)) {
            send_error(ERR_MALFORMED, "network must be \"main\" or \"test\"");
            return;
        }
        const uint8_t network = strcmp(net_name, "main") == 0 ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
        if (!network_is_the_boards(network)) {
            return;
        }
        /* Optional "ua": the address the app displays, which may be any of
         * the account's diversified addresses (wallets rotate them). The
         * device shows it only once its own keys prove it is the account's;
         * without it, the device shows its default address. */
        static char asked[ZS_UA_MAX];
        bool has_ua = cbor_map_find(payload, len, "ua", &item);
        if (has_ua && !cbor_text_copy(&item, asked, sizeof asked)) {
            send_error(ERR_MALFORMED, "ua: not an address");
            return;
        }
        static ZcashAccount shown;
        WalletError werr = wait_for_wallet();
        if (werr == WALLET_OK) {
            werr = zcash_account_derive(account, network, &shown);
        }
        if (werr != WALLET_OK) {
            send_wallet_error(werr);
            return;
        }
        if (has_ua) {
            uint8_t index[11];
            char t_address[ZCASH_TADDR_MAX];
            switch (zcash_ua_check(&shown, asked, index, t_address)) {
            case ZCASH_UA_OURS:
                break;
            case ZCASH_UA_NOT_OURS:
                memzero(&shown, sizeof shown);
                send_error((uint16_t)(ERR_ZSIGN_BASE | (unsigned)-ZS_E_USER_ADDRESS),
                           "this address is not from this device's wallet");
                return;
            case ZCASH_UA_UNSUPPORTED:
                memzero(&shown, sizeof shown);
                send_error((uint16_t)(ERR_ZSIGN_BASE | (unsigned)-ZS_E_UNSUPPORTED_BUNDLE),
                           "the address has a receiver the device cannot check");
                return;
            default:
                memzero(&shown, sizeof shown);
                send_error(ERR_MALFORMED, "ua: not a unified address on this network");
                return;
            }
            snprintf(shown.keys.ua, sizeof shown.keys.ua, "%s", asked);
            snprintf(shown.t_address, sizeof shown.t_address, "%s", t_address);
        }
        ui_request_show_address(shown.keys.ua, shown.t_address, account, network);
        SignOutcome outcome = wait_for_user();
        if (outcome != SIGN_APPROVED) {
            send_error(outcome == SIGN_PENDING ? ERR_USER_TIMEOUT : ERR_USER_REJECTED,
                       outcome == SIGN_PENDING ? "no answer on the device"
                                               : "the address does not match");
            return;
        }
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 2);
        cbor_write_text(&w, "ua");
        cbor_write_text(&w, shown.keys.ua);
        cbor_write_text(&w, "tAddress");
        cbor_write_text(&w, shown.t_address);

    } else if (strcmp(method, "zSetAccountMeta") == 0) {
        /* A companion records the account's birthday (and label) on the
         * device, so the next companion can import without asking. Not
         * secret and not needed to sign, so a session and an unlocked
         * device are enough. Stored in flash only for a stored wallet with
         * no passphrase; otherwise held until lock (zcash-meta.h). */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        if (!pin_is_unlocked()) {
            send_error(ERR_NOT_UNLOCKED, "device is locked");
            return;
        }
        CborItem item;
        uint32_t account = 0;
        if (cbor_map_find(payload, len, "account", &item)) {
            if (item.type != CBOR_UINT || item.value >= 0x80000000u) {
                send_error(ERR_MALFORMED, "account out of range");
                return;
            }
            account = (uint32_t)item.value;
        }
        /* The network the birthday belongs to. Optional for the companions
         * that predate it, all of which were testnet-only, so absent still
         * means testnet rather than the board's setting: a birthday from
         * a testnet companion filed under mainnet would be a mainnet
         * birthday nobody chose. The guard below then refuses it on a
         * board set to Mainnet, as it refuses that companion's zGetAccount. */
        uint8_t network = ZCASH_NET_TEST;
        if (cbor_map_find(payload, len, "network", &item)) {
            char net_name[8];
            if (!cbor_text_copy(&item, net_name, sizeof net_name) ||
                (strcmp(net_name, "main") != 0 && strcmp(net_name, "test") != 0)) {
                send_error(ERR_MALFORMED, "network must be \"main\" or \"test\"");
                return;
            }
            network = strcmp(net_name, "main") == 0 ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
        }
        if (!network_is_the_boards(network)) {
            return;
        }
        WalletError werr = wait_for_wallet();
        if (werr != WALLET_OK) {
            send_wallet_error(werr);
            return;
        }
        ZcashMeta meta;
        zmeta_get(account, network, &meta);   /* fields not sent are kept */
        if (cbor_map_find(payload, len, "birthday", &item)) {
            if (item.type != CBOR_UINT || item.value == 0) {
                send_error(ERR_MALFORMED, "birthday must be a block height");
                return;
            }
            meta.birthday = (uint32_t)item.value;
        }
        if (cbor_map_find(payload, len, "label", &item) &&
            !cbor_text_copy(&item, meta.label, sizeof meta.label)) {
            send_error(ERR_MALFORMED, "label: at most 32 characters");
            return;
        }
        ZcashMetaResult r = zmeta_set(account, network, &meta);
        if (r == ZMETA_E_ARG) {
            send_error(ERR_MALFORMED, "label must be printable ASCII");
            return;
        }
        if (r != ZMETA_OK_STORED && r != ZMETA_OK_SESSION) {
            send_error(ERR_NO_WALLET, "no seed to attach it to");
            return;
        }
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "result");
        cbor_write_map(&w, 1);
        cbor_write_text(&w, "stored");
        cbor_write_uint(&w, r == ZMETA_OK_STORED ? 1 : 0);

    } else if (strcmp(method, "zSignBegin") == 0 || strcmp(method, "zSignChunk") == 0 ||
               strcmp(method, "zSignFinish") == 0) {
        /* A PCZT, uploaded in order, checked by the signer backends, shown
         * page by page and signed only on the user's ACCEPT (roadmap 3).
         * Session and unlock for all three: an upload is cheap to start but
         * holds up to ZCASH_PCZT_MAX of PSRAM, and only a paired host may. */
        if (!request_is_authenticated()) {
            send_session_error(ERR_SESSION, "session required");
            return;
        }
        if (!pin_is_unlocked()) {
            zcash_sign_reset();
            send_error(ERR_NOT_UNLOCKED, "device is locked");
            return;
        }
        if (!handle_zsign(method, payload, len, &w)) {
            return;     /* it answered with an error */
        }
#endif

    } else {
        send_error(ERR_MALFORMED, "unknown or not yet implemented");
        return;
    }

    if (!cbor_writer_ok(&w)) {
        send_error(ERR_MALFORMED, "response too large");
        return;
    }

    if (reply_encrypted && session_state() == SESSION_ACTIVE) {
        int enc = session_encrypt(out, w.length, sizeof(out));
        if (enc < 0) {
            send_error(ERR_SESSION, "encrypt failed");
            return;
        }
        send_frame(FRAME_ENC_RESPONSE, out, (size_t)enc);
    } else {
        send_frame(FRAME_RESPONSE, out, w.length);
    }

    if (drop_session_after_reply) {
        drop_session_after_reply = false;
        session_reset();
    }
}

/**
 * Handle one complete frame: len:u16 ‖ type ‖ body, exactly as encodeFrame()
 * produces it and with no transport decoration of any kind.
 *
 * This is the single entry point every transport funnels into — USB after it
 * has found its sync marker, BLE after reassembly. Nothing below here knows or
 * can ask which one it was, which is what keeps the two honest: a divergence
 * would have to be written deliberately rather than drifting in.
 *
 * `frame` must be writable, because an encrypted body is decrypted in place.
 */
/**
 * Warn while there is still stack left to warn with.
 *
 * A FreeRTOS stack overflow reboots the device, and a reboot mid-session is
 * indistinguishable from a dozen other faults: the host sees its session
 * vanish and reports whatever it was doing at the time. That is exactly how
 * this was found -- a 744-byte EIP-712 render struct declared as a local in
 * dispatch() enlarged the frame for *every* method, and the symptom that
 * reached a user was "unlock failed: derivation failed", pointing at wallet
 * code that was working perfectly.
 *
 * So the margin is checked where every request passes, and a thin one is said
 * out loud before it becomes a reboot. Logged once per low reading rather than
 * per frame, because a warning printed hundreds of times is a warning nobody
 * reads.
 */
static void warn_on_thin_stack(void)
{
    /* Words, not bytes, on ESP-IDF's FreeRTOS. 2 KB is the line: comfortably
     * above the deepest path measured, and low enough that hitting it means
     * something changed. Deliberately an absolute figure rather than a fraction
     * of the task size -- when the stack grew to make room for larger calldata,
     * a proportional line would have moved with it and reported the same
     * comfort from a thinner margin. */
    const UBaseType_t low_water_words = 2048 / sizeof(StackType_t);
    static UBaseType_t worst = (UBaseType_t)-1;

    UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
    if (left < worst) {
        worst = left;
        if (left < low_water_words) {
            ESP_LOGW(TAG, "stack headroom on %s is down to %u words",
                     pcTaskGetName(NULL), (unsigned)left);
        }
    }
}

void protocol_handle_frame(uint8_t *frame, size_t len)
{
    /* Cleared for every frame, so a reply can never inherit the encryption of
     * the previous one. Set only between a successful decrypt and the end of
     * that frame's dispatch. */
    reply_encrypted = false;
    warn_on_thin_stack();

    if (len < 4) {
        send_error(ERR_MALFORMED, "short frame");
        return;
    }

    size_t body = ((size_t)frame[0] << 8) | frame[1];
    if (body < 1 || body + 2 != len) {
        /* The declared length has to agree with what arrived. A transport that
         * hands over a frame it did not verify is a transport we cannot trust
         * to have found a frame boundary at all. */
        send_error(ERR_MALFORMED, "frame length mismatch");
        return;
    }

    uint8_t  type    = frame[2];
    uint8_t *payload = frame + 3;
    size_t   payload_len = body - 1;

    if (type == FRAME_REQUEST) {
        /* Plaintext is only ever the handshake. 160 rather than 128: the v2
         * helloAck carries a 32-byte public key AND a 32-byte commitment
         * beside their key names, which lands around 112 bytes and left too
         * little margin for a field to be added without silently overflowing
         * into a refusal. 32 bytes of a task stack that is 8 KB. */
        uint8_t out[160];
        size_t  out_len = 0;
        CborItem probe;
        char method[32] = {0};

        bool named = cbor_map_find(payload, payload_len, "method", &probe) &&
                     cbor_text_copy(&probe, method, sizeof(method));

        if (named && strcmp(method, "hello") == 0) {
            /* Both handshake legs report their own errors, so there is nothing
             * to say here on failure — saying it twice would put two frames on
             * the wire for one request. */
            if (handle_hello(payload, payload_len, out, sizeof(out), &out_len)) {
                send_frame(FRAME_RESPONSE, out, out_len);
            }
        } else if (named && strcmp(method, "helloReveal") == 0) {
            if (handle_hello_reveal(payload, payload_len, out, sizeof(out), &out_len)) {
                send_frame(FRAME_RESPONSE, out, out_len);
            }
        } else {
            dispatch(payload, payload_len);
        }
    } else if (type == FRAME_ENC_REQUEST) {
        /* Decrypt in place, dispatch, re-encrypt the reply. */
        int plain = session_decrypt(payload, payload_len);
        if (plain < 0) {
            /* Nothing decrypted, so the receive counter did not move and there
             * is nothing to keep in step with. Plaintext, and the session is
             * already gone. */
            send_session_error(ERR_SESSION, "decrypt failed");
        } else {
            /* From here to the end of dispatch the reply owes the host one
             * encrypted frame: the receive counter HAS moved. */
            reply_encrypted = true;
            dispatch(payload, (size_t)plain);
            reply_encrypted = false;
        }
    } else {
        send_error(ERR_MALFORMED, "unexpected frame type");
    }
}

/* Pull complete frames out of the receive buffer, resynchronising on garbage. */
static void consume(void)
{
    for (;;) {
        /* Discard anything before a sync marker: console output shares this
         * port, so leading noise is expected rather than exceptional. */
        size_t start = 0;
        while (start + 1 < rx_len &&
               !(rx_buf[start] == SYNC0 && rx_buf[start + 1] == SYNC1)) {
            start++;
        }
        if (start > 0) {
            memmove(rx_buf, rx_buf + start, rx_len - start);
            rx_len -= start;
        }

        if (rx_len < 4) return;

        size_t body = ((size_t)rx_buf[2] << 8) | rx_buf[3];
        if (body < 1 || body > MAX_FRAME - 4) {
            /* Not a length we would ever send. Drop the marker and rescan
             * rather than trusting it. */
            memmove(rx_buf, rx_buf + 2, rx_len - 2);
            rx_len -= 2;
            continue;
        }

        if (rx_len < 4 + body) return;      /* wait for the rest */

        /* Past the marker, the frame is the transport-blind one. */
        protocol_handle_frame(rx_buf + 2, body + 2);

        memmove(rx_buf, rx_buf + 4 + body, rx_len - (4 + body));
        rx_len -= 4 + body;
    }
}

/* One read-and-parse pass. Factored out of the task so the host suite can run
 * the real receive path (sim/test_protocol.c) without a scheduler to preempt
 * an endless loop; the task is what that loop was, and nothing else changed. */
static bool rx_pump(void)
{
    uint8_t chunk[RX_CHUNK];

    int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
    if (n <= 0) {
        return false;
    }
    if (!rx_enabled) {
        /* BLE is the selected transport, so this port is not an endpoint right
         * now. The bytes are still drained — leaving them to back up would
         * mean a stale request executing the moment USB is reselected — but
         * nothing is parsed and nothing is answered. */
        rx_len = 0;
        return true;
    }
    if (rx_len + (size_t)n > sizeof(rx_buf)) {
        /* Never grow past the fixed buffer on the host's say-so. */
        ESP_LOGW(TAG, "Receive buffer overrun; resynchronising");
        rx_len = 0;
        return true;
    }
    memcpy(rx_buf + rx_len, chunk, (size_t)n);
    rx_len += (size_t)n;
    consume();
    return true;
}

static void protocol_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Protocol endpoint listening on USB-Serial-JTAG");

    for (;;) {
        rx_pump();
    }
}

void protocol_reset_rx(void)
{
    rx_len = 0;
}

#ifdef LEEK_HOST_TEST
/** Drain whatever the test has queued on the port, then return. */
/* Send an arbitrary frame down the active writer, for the frame-size test. */
void protocol__send_frame_for_test(uint8_t type, const uint8_t *payload, size_t len)
{
    send_frame(type, payload, len);
}

void protocol__pump_for_test(void)
{
    while (rx_pump()) { }
}

/** Forget any partial frame, as a reconnect would. */
void protocol__reset_for_test(void)
{
    rx_len = 0;
}
#endif

void protocol_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = PROTOCOL_USB_RX_BUF;
    cfg.tx_buffer_size = PROTOCOL_USB_TX_BUF;

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %d", err);
        return;
    }

    /* 10 KB, not 4. Both request tasks run the same dispatch() and then call
     * into PBKDF2 and BIP32 derivation; at 4 KB the BLE task overflowed and
     * rebooted the device during unlock. The signing paths that pushed this
     * to 10 KB are gone, but the stack is deliberately not shrunk ahead of
     * the Zcash signing path that will replace them - warn_on_thin_stack()
     * is the gauge, and it is cheaper to shrink with measurements later.
     *
     * Raised together with bleproto in ble.c: they must not drift, since either
     * one can serve any request. */
    xTaskCreate(protocol_task, "protocol", 10240, NULL, 4, NULL);
}
