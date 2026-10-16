/**
 * Wire-protocol conformance tests (ROADMAP T26).
 *
 * The real src/protocol.c, running on the host against an in-memory port. Up
 * to now the only machine that had ever executed the device's parser was a
 * board, which made "the mock must never be more permissive than the device"
 * an aspiration: there was nothing to compare the mock against except a
 * document. Twice the mock accepted requests the firmware rejects and
 * certified broken code. This is the other half of that comparison.
 *
 * So the tests here are deliberately written at the byte level — sync marker,
 * length, frame type, CBOR body — rather than through a helper that speaks the
 * protocol on the endpoint's own terms. A test that shares the encoder with
 * the code under test agrees with it by construction, which is exactly the
 * agreement that stopped being informative.
 *
 * Two properties are worth naming because they are the ones that bite:
 *
 *   - Errors returned once a session is active must be ENCRYPTED (0x7E). The
 *     device advances its receive counter the moment a frame decrypts, error
 *     or not, so a plaintext error leaves the two sides one apart and every
 *     later frame fails to decrypt. This is a permanent desynchronisation from
 *     a transient fault.
 *   - A request is answered in the form it arrived in: a plaintext request
 *     never gets an encrypted reply, whatever the session state.
 *
 * The signing commands this suite once covered are gone with the code that
 * served them (roadmap 1.1); what remains is the chain-agnostic core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cbor.h"
#include "esp_stubs.h"
#include "fake_nvs.h"
#include "blind-signing.h"
#include "fake_usb.h"
#include "fake_wallet.h"
#include "leek-wallet.h"
#include "pin.h"
#include "ble.h"
#include "ble-chunk.h"
#include "ble-name.h"
#include "nvs.h"
#include "protocol.h"
#include "transport.h"
#include "session.h"
#include "ui.h"

#include "chacha20poly1305/rfc7539.h"

void pin__reset_static_state_for_test(void);
void protocol__pump_for_test(void);
void protocol__reset_for_test(void);

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        printf("  FAIL: "); printf(__VA_ARGS__);     \
        printf("\n"); failures++;                    \
    }                                                \
} while (0)

/* Frame types and error codes as the wire carries them, spelled out here
 * rather than included from protocol.c. A test that imports the constants it
 * is checking cannot notice one of them changing. */
#define T_REQUEST      0x01
#define T_RESPONSE     0x02
#define T_ENC_REQUEST  0x11
#define T_ENC_RESPONSE 0x12
#define T_ENC_ERROR    0x7E
#define T_ERROR        0x7F

#define E_MALFORMED    0x0001
#define E_VERSION      0x0002
#define E_NOT_UNLOCKED 0x0100
#define E_REJECTED     0x0200
#define E_TIMEOUT      0x0201
#define E_NO_WALLET    0x0300
#define E_SESSION      0x0400
#define E_BUSY         0x0401

/* ------------------------------------------------------------- fake UI */

/* The screens are another agent's file and another tier's problem. What the
 * protocol tests need to know is only whether the device asked the user
 * anything, and what answer came back. */

static int  unlock_prompts;
static int  lock_requests;
static int  session_confirm_prompts;
static SignOutcome scripted_outcome;

static int      passphrase_prompts;    /* ui_request_passphrase_confirm calls */
static uint32_t shown_passphrase_xfp;

/* What the device's own screens are browsing. A variable rather than a constant
 * so a test can move it the way a button press does, and check getStatus
 * reports it -- the field exists precisely so the host can follow a change the
 * user made on the device without telling the app. */
static uint32_t stub_hd_account;

uint32_t ui_hd_account(void)          { return stub_hd_account; }

void ui_request_session_confirm(void) { session_confirm_prompts++; }

/* M-2. On hardware this is "is a screen up that the user has to answer"; here
 * it is a switch the tests flip, because what protocol.c owes is a defined
 * response to the answer rather than a particular way of reaching it. */
static bool stub_user_is_answering = false;
bool ui_user_is_answering(void)       { return stub_user_is_answering; }
void ui_request_unlock(void)          { unlock_prompts++; }
void ui_request_lock(void)            { lock_requests++; }

void ui_request_passphrase_confirm(uint32_t fingerprint)
{
    passphrase_prompts++;
    shown_passphrase_xfp = fingerprint;
}

SignOutcome ui_sign_outcome(void)
{
    return scripted_outcome;
}

void ui_sign_clear(void) { }

/* The Zcash review: counted, and the summary kept so a test can check that
 * what the user would have read is what was signed. */
static int zsign_reviews;
static zs_summary shown_zsign;
void ui_request_zsign_review(const zs_summary *s, uint8_t network)
{
    (void)network;
    zsign_reviews++;
    shown_zsign = *s;
}

/* zShowAddress: what the device was asked to show, for the test to compare
 * with the vector. */
static int  address_checks;
static char shown_ua[256];
void ui_request_show_address(const char *ua, const char *t_address, uint32_t account,
                             uint8_t network)
{
    (void)t_address; (void)account; (void)network;
    address_checks++;
    snprintf(shown_ua, sizeof shown_ua, "%s", ua);
}

/* Counted, because the timeout path answering the host is only half of what it
 * owes: the confirmation screen has to come down too, and for a long time it
 * did not. */
static int sign_expiries;
void ui_sign_expire(void) { sign_expiries++; }

/* ------------------------------------------------------------ host side */

/* The host half of the session: same derivation, opposite direction. Written
 * out rather than borrowed from session.c because the point is to check the
 * device against an independent implementation of the same rules. */

void curve25519_scalarmult_donna(uint8_t *mypublic, const uint8_t *n,
                                 const uint8_t *basepoint);

static uint8_t host_priv[32], host_pub[32];
static uint8_t device_pub[32];
static uint8_t host_nonce[SESSION_NONCE_SIZE], device_nonce[SESSION_NONCE_SIZE];
static uint8_t k_h2d[32], k_d2h[32];
static uint32_t host_tx, host_rx;
static bool session_up;

static void fill(uint8_t *buf, size_t len, uint8_t seed)
{
    uint32_t s = 0x9e3779b9u ^ seed;
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        buf[i] = (uint8_t)(s >> 24);
    }
}

static void host_keypair(uint8_t seed)
{
    static const uint8_t base[32] = { 9 };
    uint8_t clamped[32];
    fill(host_priv, 32, seed);
    memcpy(clamped, host_priv, 32);
    clamped[0] &= 248; clamped[31] &= 127; clamped[31] |= 64;
    curve25519_scalarmult_donna(host_pub, clamped, base);
}

static void make_nonce(uint32_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 12);
    nonce[8]  = (uint8_t)(counter >> 24);
    nonce[9]  = (uint8_t)(counter >> 16);
    nonce[10] = (uint8_t)(counter >> 8);
    nonce[11] = (uint8_t)counter;
}

static size_t host_seal(uint8_t *buf, size_t len)
{
    uint8_t nonce[12];
    make_nonce(host_tx++, nonce);

    chacha20poly1305_ctx ctx;
    rfc7539_init(&ctx, k_h2d, nonce);
    chacha20poly1305_encrypt(&ctx, buf, buf, len);
    rfc7539_finish(&ctx, 0, len, buf + len);
    return len + 16;
}

/* Returns the plaintext length, or -1 if the tag does not verify — which is
 * what a counter that has drifted looks like from here. */
static int host_open(uint8_t *buf, size_t len)
{
    if (len < 16) return -1;
    size_t body = len - 16;

    uint8_t nonce[12];
    make_nonce(host_rx, nonce);

    chacha20poly1305_ctx ctx;
    rfc7539_init(&ctx, k_d2h, nonce);
    chacha20poly1305_decrypt(&ctx, buf, buf, body);

    uint8_t tag[16];
    rfc7539_finish(&ctx, 0, body, tag);
    if (memcmp(tag, buf + body, 16) != 0) return -1;

    host_rx++;
    return (int)body;
}

/* ------------------------------------------------------------- the wire */

static uint8_t stash[8192];
static size_t  stash_len;

static void raw_send(const uint8_t *bytes, size_t len)
{
    fake_usb_host_write(bytes, len);
    protocol__pump_for_test();
    stash_len += fake_usb_device_read(stash + stash_len, sizeof(stash) - stash_len);
}

/* Collect whatever the device has written, without sending anything. Needed
 * for the few paths that emit a frame without a request arriving first. */
static void collect_output(void)
{
    stash_len += fake_usb_device_read(stash + stash_len, sizeof(stash) - stash_len);
}

/** Frame a payload the way the device expects and hand it over. */
static void send_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t frame[PROTOCOL_MAX_FRAME + 8];
    size_t body = len + 1;

    frame[0] = 'L';
    frame[1] = 'K';
    frame[2] = (uint8_t)(body >> 8);
    frame[3] = (uint8_t)body;
    frame[4] = type;
    if (len) {
        memcpy(frame + 5, payload, len);
    }
    raw_send(frame, len + 5);
}

static void send_plain(const uint8_t *payload, size_t len)
{
    send_frame(T_REQUEST, payload, len);
}

static void send_encrypted(const uint8_t *payload, size_t len)
{
    /* Sized to the device's own limit rather than to today's longest request:
     * signTypedData carries its type definitions and is several times the size
     * of anything else here, and a harness buffer that quietly overflowed
     * would look exactly like a firmware bug. */
    uint8_t buf[PROTOCOL_MAX_FRAME];
    memcpy(buf, payload, len);
    send_frame(T_ENC_REQUEST, buf, host_seal(buf, len));
}

typedef struct {
    bool    present;
    uint8_t type;
    uint8_t payload[PROTOCOL_MAX_FRAME];
    size_t  len;
} Frame;

/** Pull the next complete frame the device wrote, if there is one. */
static Frame next_frame(void)
{
    Frame f;
    memset(&f, 0, sizeof(f));

    size_t i = 0;
    while (i + 1 < stash_len && !(stash[i] == 'L' && stash[i + 1] == 'K')) i++;
    if (i + 4 >= stash_len) return f;

    size_t body = ((size_t)stash[i + 2] << 8) | stash[i + 3];
    if (i + 4 + body > stash_len) return f;
    if (body < 1 || body - 1 > sizeof(f.payload)) return f;

    f.present = true;
    f.type = stash[i + 4];
    f.len = body - 1;
    memcpy(f.payload, stash + i + 5, f.len);

    size_t consumed = i + 4 + body;
    memmove(stash, stash + consumed, stash_len - consumed);
    stash_len -= consumed;
    return f;
}

/** The next frame, decrypted if it is one of the encrypted types. */
static Frame next_reply(void)
{
    Frame f = next_frame();
    if (f.present && (f.type == T_ENC_RESPONSE || f.type == T_ENC_ERROR)) {
        int n = host_open(f.payload, f.len);
        if (n < 0) {
            f.len = 0;
            f.payload[0] = 0;   /* caller's assertions will report the miss */
        } else {
            f.len = (size_t)n;
        }
    }
    return f;
}

static void drop_pending(void) { stash_len = 0; }

/* ---------------------------------------------------------- CBOR helpers */

/** { "method": <name> } — the smallest legal request. */
static size_t req(uint8_t *buf, size_t cap, const char *method)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, method);
    return cbor_writer_ok(&w) ? w.length : 0;
}

/**
 * The body of the top-level { "result": ... } wrapper.
 *
 * cbor_map_find only walks one level, so nested lookups need the offset of the
 * value, which the reader tracks and the finder does not.
 */
static bool result_body(const Frame *f, const uint8_t **out, size_t *out_len)
{
    CborReader r;
    CborItem it;
    cbor_reader_init(&r, f->payload, f->len);

    if (!cbor_read(&r, &it) || it.type != CBOR_MAP || it.value != 1) return false;
    if (!cbor_read(&r, &it) || it.type != CBOR_TEXT) return false;
    if (it.value != 6 || memcmp(it.data, "result", 6) != 0) return false;

    *out = f->payload + r.pos;
    *out_len = f->len - r.pos;
    return true;
}

static bool error_code(const Frame *f, uint32_t *code)
{
    CborItem it;
    if (!cbor_map_find(f->payload, f->len, "code", &it)) return false;
    if (it.type != CBOR_UINT) return false;
    *code = it.value;
    return true;
}

/** An error of exactly this code, in a frame of exactly this type. */
static void expect_error(uint8_t type, uint32_t want, const char *what)
{
    Frame f = next_reply();
    if (!f.present) {
        printf("  FAIL: %s: no reply at all\n", what);
        failures++;
        return;
    }
    CHECK(f.type == type, "%s: frame type 0x%02X, wanted 0x%02X", what, f.type, type);

    uint32_t code = 0;
    if (!error_code(&f, &code)) {
        printf("  FAIL: %s: reply carries no error code\n", what);
        failures++;
        return;
    }
    CHECK(code == want, "%s: error 0x%04X, wanted 0x%04X", what, code, (unsigned)want);
}

static void expect_silence(const char *what)
{
    Frame f = next_frame();
    CHECK(!f.present, "%s: device answered when it had nothing to answer", what);
}

/* ------------------------------------------------------------- fixtures */

static const char *TEST_MNEMONIC =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";

/* Fresh device, nothing negotiated, no PIN entered. */
static void fresh_device(void)
{
    fake_usb_reset();
    fake_nvs_reset();
    /* The wipe of simulated flash took the blind-signing flag with it; the
     * cached copy has to go too, or a test that enabled it would leak the
     * weaker mode into the next one. This is the same call the wipe screen
     * makes on hardware. */
    blind_signing_forget();
    pin__reset_static_state_for_test();
    pin_init();
    fake_wallet_reset();
    session_reset();
    protocol__reset_for_test();
    /* On hardware transport_init() always runs before the endpoint serves, and
     * it is what turns receive on. The suite has no transport layer, so it
     * stands in for that call -- without it every test would be exercising a
     * device that has not yet chosen a transport and answers nothing. */
    protocol_set_rx_enabled(true);

    stub_hd_account = 0;
    stash_len = 0;
    host_tx = host_rx = 0;
    session_up = false;
    unlock_prompts = lock_requests = session_confirm_prompts = 0;
    passphrase_prompts = 0;
    shown_passphrase_xfp = 0;
    stub_user_is_answering = false;
    scripted_outcome = SIGN_APPROVED;

    protocol_start();
}

static void device_has_a_wallet(void)
{
    wallet_unlock("123456", 6);
    fake_wallet_preload(TEST_MNEMONIC);
}

static void device_unlocked(void)
{
    pin_set("123456");
    pin_verify("123456");
    device_has_a_wallet();
}

/* Write `{method: <name>}` plus whatever `extra` adds, and send it plaintext. */
static void send_hello_frame(const char *method,
                             const char *field, const uint8_t *value, size_t value_len)
{
    uint8_t payload[128];
    CborWriter w;
    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, field ? 3 : 2);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, method);
    cbor_write_text(&w, "version");
    cbor_write_uint(&w, PROTOCOL_VERSION);
    if (field) {
        cbor_write_text(&w, field);
        cbor_write_bytes(&w, value, value_len);
    }
    send_plain(payload, w.length);
}

/**
 * Run the v2 handshake and stop at PENDING: keys agreed, user has not confirmed.
 *
 * Written out the long way rather than calling into session.c's instance,
 * because the point of this suite is to check the device against an
 * independent implementation of the same rules — including the commitment
 * check, which is the host's job and the reason the digits mean anything.
 */
static void handshake(uint8_t seed)
{
    host_keypair(seed);
    fill(host_nonce, sizeof(host_nonce), (uint8_t)(seed ^ 0x5a));

    send_hello_frame("hello", "hostPubkey", host_pub, sizeof(host_pub));

    Frame f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE, "hello was not answered in plaintext");
    if (!f.present || f.type != T_RESPONSE) return;

    const uint8_t *body;
    size_t body_len;
    CborItem it, commit_it, ver;
    if (!result_body(&f, &body, &body_len) ||
        !cbor_map_find(body, body_len, "devicePubkey", &it) ||
        it.type != CBOR_BYTES || it.value != 32 ||
        !cbor_map_find(body, body_len, "deviceCommit", &commit_it) ||
        commit_it.type != CBOR_BYTES || commit_it.value != SESSION_COMMIT_SIZE) {
        printf("  FAIL: helloAck carried no device public key and commitment\n");
        failures++;
        return;
    }
    CHECK(cbor_map_find(body, body_len, "version", &ver) && ver.type == CBOR_UINT &&
          ver.value == PROTOCOL_VERSION, "helloAck did not name the protocol version");

    memcpy(device_pub, it.data, 32);
    uint8_t committed[SESSION_COMMIT_SIZE];
    memcpy(committed, commit_it.data, sizeof(committed));

    /* Only now does the host reveal its nonce. Sending it before the device
     * had committed would give the device's side of the comparison away. */
    send_hello_frame("helloReveal", "hostNonce", host_nonce, sizeof(host_nonce));

    f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE, "helloReveal was not answered in plaintext");
    if (!f.present || f.type != T_RESPONSE) return;

    if (!result_body(&f, &body, &body_len) ||
        !cbor_map_find(body, body_len, "deviceNonce", &it) ||
        it.type != CBOR_BYTES || it.value != SESSION_NONCE_SIZE) {
        printf("  FAIL: the device revealed no nonce\n");
        failures++;
        return;
    }
    memcpy(device_nonce, it.data, SESSION_NONCE_SIZE);

    /* The check the app must never skip: without it the device's nonce is a
     * value the peer could have chosen after seeing ours, which is the whole
     * of C-1. */
    uint8_t expected[SESSION_COMMIT_SIZE];
    session_commitment(device_pub, host_pub, device_nonce, expected);
    CHECK(memcmp(expected, committed, sizeof(expected)) == 0,
          "the device revealed a nonce it had not committed to");

    SessionTranscript t;
    memcpy(t.host_public, host_pub, 32);
    memcpy(t.device_public, device_pub, 32);
    memcpy(t.host_nonce, host_nonce, SESSION_NONCE_SIZE);
    memcpy(t.device_nonce, device_nonce, SESSION_NONCE_SIZE);

    char passkey[7];
    CHECK(session_derive(host_priv, device_pub, &t, k_h2d, k_d2h, passkey),
          "host could not derive the session");
}

/** Complete the handshake: the user compares the passkey and accepts. */
static void confirmed_session(uint8_t seed)
{
    handshake(seed);
    session_confirm();
    session_up = true;
}

/* ================================================================= tests */

/* Boot order. protocol_start() used to run before transport_init(), so for a
 * few milliseconds the USB endpoint answered anything -- on a BLE device too --
 * and transport_apply() reset the receive state of a task already parsing a
 * frame. A host that opens the port without resetting the board lands in that
 * window; the device panicked in the USB-Serial-JTAG ISR on the third request.
 * The endpoint must answer nothing until a transport has been chosen. */
static void test_nothing_is_answered_before_a_transport_is_chosen(void)
{
    printf("== the endpoint is silent until a transport is selected\n");

    /* Checked before any fixture runs: fresh_device() stands in for
     * transport_init() and turns receive on, so after it the boot default is no
     * longer observable. This assertion is the only place it is. */
    CHECK(!protocol_rx_enabled(),
          "receive is on before transport_init() has chosen a transport");

    fresh_device();
    protocol_set_rx_enabled(false);     /* the pre-transport_init() state */

    uint8_t payload[64];
    for (const char *const *m = (const char *[]){"ping", "getFeatures", "getStatus", NULL};
         *m; m++) {
        send_plain(payload, req(payload, sizeof(payload), *m));
        Frame f = next_frame();
        CHECK(!f.present, "%s was answered before a transport was chosen", *m);
    }

    /* And once transport_apply() has run, the same frames are served. */
    protocol_set_rx_enabled(true);
    send_plain(payload, req(payload, sizeof(payload), "ping"));
    Frame f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE,
          "ping went unanswered after a transport was selected");
}

static void test_plaintext_ping_and_features(void)
{
    printf("== ping and getFeatures answer in plaintext, byte for byte\n");
    fresh_device();

    CHECK(fake_usb_driver_installed(), "protocol_start did not install the port");

    uint8_t payload[64];
    send_plain(payload, req(payload, sizeof(payload), "ping"));

    Frame f = next_frame();
    CHECK(f.present, "ping went unanswered");
    if (f.present) {
        CHECK(f.type == T_RESPONSE, "ping replied with type 0x%02X", f.type);

        /* {"result": {"pong": 1}}, canonical: a1 66 "result" a1 64 "pong" 01 */
        static const uint8_t want[] = {
            0xa1, 0x66, 'r','e','s','u','l','t',
            0xa1, 0x64, 'p','o','n','g', 0x01,
        };
        CHECK(f.len == sizeof(want) && memcmp(f.payload, want, f.len) == 0,
              "ping payload is not the canonical encoding (%zu bytes)", f.len);
    }

    send_plain(payload, req(payload, sizeof(payload), "getFeatures"));
    f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE, "getFeatures went unanswered");

    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (f.present && result_body(&f, &body, &body_len)) {
        CHECK(cbor_map_find(body, body_len, "blindSigning", &it) &&
              it.type == CBOR_UINT && it.value == 0,
              "getFeatures does not report blind signing off by default");
        CHECK(cbor_map_find(body, body_len, "model", &it) && it.type == CBOR_TEXT,
              "getFeatures names no model");
        CHECK(cbor_map_find(body, body_len, "chain", &it) && it.type == CBOR_TEXT &&
              it.value == 5 && memcmp(it.data, "zcash", 5) == 0,
              "getFeatures does not report the zcash chain");
        /* Nothing user-specific may appear here: it is answered before any
         * session exists, to anything plugged into the port. */
        CHECK(!cbor_map_find(body, body_len, "walletCount", &it) &&
              !cbor_map_find(body, body_len, "activeWallet", &it),
              "getFeatures leaks wallet state to an unauthenticated peer");
    } else {
        CHECK(!f.present, "getFeatures reply is not a result map");
    }
}

static void test_status_is_public_but_thin(void)
{
    printf("== getStatus answers pre-session and says only what it must\n");
    fresh_device();
    device_unlocked();

    uint8_t payload[64];
    send_plain(payload, req(payload, sizeof(payload), "getStatus"));

    Frame f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE, "getStatus went unanswered in plaintext");

    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (f.present && result_body(&f, &body, &body_len)) {
        CHECK(cbor_map_find(body, body_len, "unlocked", &it) &&
              it.type == CBOR_UINT && it.value == 1, "getStatus lost the unlock flag");
        CHECK(cbor_map_find(body, body_len, "passphrase", &it) &&
              it.type == CBOR_UINT && it.value == 0, "getStatus lost the passphrase flag");
        /* Whether a passphrase is applied, never which one. */
        CHECK(!cbor_map_find(body, body_len, "passphraseValue", &it),
              "getStatus reveals the passphrase itself");
    }
}

/* ---------------------------------------------------------- the handshake */

/* Send a `hello` whose version field is whatever the caller says, including
 * absent, which is what a v1 host sends. */
static void send_hello_versioned(bool with_version, uint32_t version)
{
    uint8_t payload[128];
    CborWriter w;
    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, with_version ? 3 : 2);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "hello");
    if (with_version) {
        cbor_write_text(&w, "version");
        cbor_write_uint(&w, version);
    }
    cbor_write_text(&w, "hostPubkey");
    cbor_write_bytes(&w, host_pub, sizeof(host_pub));
    send_plain(payload, w.length);
}

/**
 * PROTOCOL.md §7 promised a version check that was never built, and `hello`
 * carried a version the client never read. Both ends now check, and the
 * failure has to be its own error code: a v1 host that got as far as sealing a
 * frame would fail with "decrypt failed", which is true and useless.
 */
static void test_version_mismatch_is_named(void)
{
    printf("== a host speaking another protocol version is told so\n");

    fresh_device();
    host_keypair(90);

    send_hello_versioned(false, 0);
    expect_error(T_ERROR, E_VERSION, "a v1 host, which sends no version at all");
    CHECK(session_state() == SESSION_IDLE, "a rejected hello left session state behind");
    CHECK(session_confirm_prompts == 0, "a rejected hello put a prompt on screen");

    send_hello_versioned(true, 1);
    expect_error(T_ERROR, E_VERSION, "a host offering v1");

    send_hello_versioned(true, 99);
    expect_error(T_ERROR, E_VERSION, "a host offering a version from the future");

    /* And the current one is accepted, so the check is a check rather than a
     * wall. */
    handshake(91);
    CHECK(session_state() == SESSION_PENDING,
          "the current version did not complete a handshake");
}

/* The ordering C-1 turns on: nothing derives, and nothing goes on screen,
 * until the host has revealed a nonce against a live commitment. */
static void test_reveal_is_required_before_anything_is_shown(void)
{
    printf("== the passkey screen waits for the reveal, and a stray reveal is refused\n");

    fresh_device();
    host_keypair(92);

    /* A reveal with no commitment outstanding. */
    send_hello_frame("helloReveal", "hostNonce", host_pub, SESSION_NONCE_SIZE);
    expect_error(T_ERROR, E_SESSION, "helloReveal with no handshake behind it");

    send_hello_versioned(true, PROTOCOL_VERSION);
    Frame f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE, "hello was not answered");
    CHECK(session_state() == SESSION_AWAITING_REVEAL,
          "hello did not leave the device awaiting a nonce (state %d)", session_state());
    /* The whole point: the device has committed but has NOT asked the user to
     * compare anything, because there is nothing to compare yet. */
    CHECK(session_confirm_prompts == 0,
          "the device showed a passkey before the host revealed its nonce");

    uint8_t nonce[SESSION_NONCE_SIZE];
    fill(nonce, sizeof(nonce), 93);
    send_hello_frame("helloReveal", "hostNonce", nonce, sizeof(nonce));
    f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE, "helloReveal was not answered");
    CHECK(session_state() == SESSION_PENDING, "the reveal did not reach PENDING");
    CHECK(session_confirm_prompts == 1, "the reveal did not put a passkey on screen");

    /* One commitment, one reveal. A second would be a second derivation
     * against a nonce the peer has already seen. */
    send_hello_frame("helloReveal", "hostNonce", nonce, sizeof(nonce));
    expect_error(T_ERROR, E_SESSION, "a commitment reused for a second reveal");

    /* A short nonce is malformed rather than padded. */
    fresh_device();
    host_keypair(94);
    send_hello_versioned(true, PROTOCOL_VERSION);
    (void)next_frame();
    send_hello_frame("helloReveal", "hostNonce", nonce, 8);
    expect_error(T_ERROR, E_MALFORMED, "an 8-byte hostNonce");
}

/* M-2. A plaintext `hello` is unauthenticated, and honouring one mid-approval
 * lets anybody with write access to the port schedule the moment a signing
 * confirmation disappears from under the user. */
static void test_hello_waits_for_a_user_who_is_answering(void)
{
    printf("== a handshake does not interrupt a confirmation on screen (M-2)\n");

    fresh_device();
    device_unlocked();
    confirmed_session(95);

    stub_user_is_answering = true;

    host_keypair(96);
    send_hello_versioned(true, PROTOCOL_VERSION);
    expect_error(T_ERROR, E_BUSY, "a hello while the user is answering");
    CHECK(session_state() == SESSION_ACTIVE,
          "an unauthenticated hello tore down a live session while the user was "
          "reading a confirmation (state %d)", session_state());
    CHECK(session_confirm_prompts == 1,
          "a refused hello still repainted the screen");

    /* And once the user has answered, a handshake works again — this is a
     * deferral, not a lockout. Refusing whenever a session merely EXISTS would
     * strand an app that crashed and restarted, because USB has no disconnect
     * for the device to notice. */
    stub_user_is_answering = false;
    handshake(97);
    CHECK(session_state() == SESSION_PENDING,
          "a handshake was still refused after the user answered");
}

/* The other leg. M-2 gated `hello`, but `hello` is not the leg that puts a
 * screen up — `helloReveal` is, and a handshake can sit in AWAITING_REVEAL for
 * as long as the peer likes. So the interruption M-2 closed is still
 * reachable in two moves: open a handshake while nothing is on screen, wait
 * for the user to be reading something, then reveal. */
static void test_a_reveal_cannot_interrupt_a_confirmation_either(void)
{
    printf("== the second handshake leg also waits for the user (M-2b)\n");

    fresh_device();
    device_unlocked();

    /* Move one: a handshake opened at a quiet moment. Nothing on screen yet,
     * by design — the passkey does not exist until the reveal. */
    host_keypair(120);
    send_hello_versioned(true, PROTOCOL_VERSION);
    (void)next_frame();
    CHECK(session_state() == SESSION_AWAITING_REVEAL,
          "the device did not commit (state %d)", session_state());
    CHECK(session_confirm_prompts == 0, "hello alone put a prompt on screen");

    /* Move two: the user is now answering a question the DEVICE asked. */
    stub_user_is_answering = true;

    uint8_t nonce[SESSION_NONCE_SIZE];
    fill(nonce, sizeof(nonce), 121);
    send_hello_frame("helloReveal", "hostNonce", nonce, sizeof(nonce));
    expect_error(T_ERROR, E_BUSY, "a helloReveal while the user is answering");
    CHECK(session_confirm_prompts == 0,
          "a reveal replaced the screen the user was reading");

    /* A deferral, not a lockout: the commitment is still outstanding, so the
     * same reveal works once the user has answered. */
    CHECK(session_state() == SESSION_AWAITING_REVEAL,
          "the deferred reveal threw the commitment away (state %d)",
          session_state());
    stub_user_is_answering = false;
    send_hello_frame("helloReveal", "hostNonce", nonce, sizeof(nonce));
    Frame f = next_frame();
    CHECK(f.present && f.type == T_RESPONSE,
          "the reveal was still refused after the user answered");
    CHECK(session_confirm_prompts == 1, "the reveal put no passkey on screen");
}

static void test_keys_need_a_session_and_a_passkey(void)
{
    printf("== key operations wait for a session and for the user to confirm\n");
    fresh_device();
    device_unlocked();

    uint8_t payload[64];

    /* No handshake at all. selectWallet is the probe: it moves which seed
     * every later derivation comes from, so it is the gated command with the
     * least excuse for answering a stranger. */
    send_plain(payload, req(payload, sizeof(payload), "selectWallet"));
    expect_error(T_ERROR, E_SESSION, "selectWallet before any handshake");

    /* Keys agreed, but the six digits on the OLED have not been compared. The
     * whole MITM defence is that step, so PENDING must be as good as nothing:
     * a relay that got this far would otherwise be talking to a wallet. */
    handshake(11);
    CHECK(session_confirm_prompts == 1, "the handshake did not put a passkey on screen");
    CHECK(session_state() == SESSION_PENDING, "handshake left the session in state %d",
          session_state());

    send_encrypted(payload, req(payload, sizeof(payload), "selectWallet"));
    expect_error(T_ERROR, E_SESSION, "selectWallet before the passkey was confirmed");

    /* And the plaintext door stays shut too — a host that skips the confirm
     * step and drops back to plaintext must not be answered either. */
    send_plain(payload, req(payload, sizeof(payload), "selectWallet"));
    expect_error(T_ERROR, E_SESSION, "plaintext selectWallet during a pending session");
}

static void test_locked_device_refuses_keys(void)
{
    printf("== a locked device refuses key operations with 0x0100\n");
    fresh_device();
    device_has_a_wallet();          /* a wallet exists; the PIN has not been entered */
    confirmed_session(12);

    CHECK(!pin_is_unlocked(), "the fixture left the device unlocked");

    uint8_t payload[64];
    send_encrypted(payload, req(payload, sizeof(payload), "selectWallet"));
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "selectWallet while locked");

    send_encrypted(payload, req(payload, sizeof(payload), "setPassphrase"));
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "setPassphrase while locked");
    CHECK(passphrase_prompts == 0, "a locked device put a passphrase on screen");

    /* unlock() prompts on the device and never carries a PIN. */
    send_encrypted(payload, req(payload, sizeof(payload), "unlock"));
    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE, "unlock was not answered");
    CHECK(unlock_prompts == 1, "unlock did not ask the device to prompt");

    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (f.present && result_body(&f, &body, &body_len)) {
        CHECK(cbor_map_find(body, body_len, "unlocked", &it) &&
              it.type == CBOR_UINT && it.value == 0,
              "unlock claimed success before the user typed anything");
    }
}

/* `lock` answers before the channel closes.
 *
 * lock_device() drops the session, and on the device the UI task is what runs
 * it -- so the reply to the host's own lock call had to be on the wire first.
 * A frame that comes back T_ENC_RESPONSE proves it was encrypted while the
 * session was still up; a session that is IDLE straight afterwards proves the
 * drop happened at all. Getting only one of those is the bug.
 */
static void test_lock_answers_before_it_closes_the_channel(void)
{
    printf("== the host's own lock is answered, then the channel closes\n");
    fresh_device();
    device_unlocked();
    confirmed_session(31);
    CHECK(session_state() == SESSION_ACTIVE, "the fixture left no session");

    uint8_t payload[64];
    send_encrypted(payload, req(payload, sizeof(payload), "lock"));

    Frame f = next_reply();
    CHECK(f.present, "lock was not answered at all");
    CHECK(f.present && f.type == T_ENC_RESPONSE,
          "the lock reply was not encrypted: the channel closed before it was sent");

    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (f.present && result_body(&f, &body, &body_len)) {
        CHECK(cbor_map_find(body, body_len, "unlocked", &it) &&
              it.type == CBOR_UINT && it.value == 0,
              "lock did not report the device as locked");
    }

    CHECK(session_state() == SESSION_IDLE,
          "the channel survived a lock the host asked for");

    /* And the flag does not linger: a later request must not drop its own
     * session as a side effect of an earlier lock.
     *
     * The host's frame counters are reset by hand because only fresh_device()
     * does it, and a fresh device would throw away the state under test. A
     * second session on stale counters fails its tag and tears itself down,
     * which would look exactly like the bug being checked for. */
    host_tx = host_rx = 0;
    confirmed_session(32);
    send_encrypted(payload, req(payload, sizeof(payload), "getStatus"));
    (void)next_reply();
    CHECK(session_state() == SESSION_ACTIVE,
          "a request after a lock dropped its own session");
}

/* The signing and address commands LeekWallet served are gone (roadmap 1.1). A host
 * built for that firmware must get a clean "unknown method" for each of them,
 * inside a confirmed session and encrypted like any other error - not
 * silence, not a plaintext reply that desynchronises the channel, and not a
 * half-implemented answer. */
static void test_removed_methods_are_unknown(void)
{
    printf("== the removed LeekWallet methods are refused as unknown\n");
    fresh_device();
    device_unlocked();
    confirmed_session(13);

    static const char *const removed[] = {
        "getAddress", "signTransaction", "signMessage", "signTypedData", "signHash",
    };
    uint8_t payload[64];
    for (size_t i = 0; i < sizeof(removed) / sizeof(removed[0]); i++) {
        send_encrypted(payload, req(payload, sizeof(payload), removed[i]));
        expect_error(T_ENC_ERROR, E_MALFORMED, removed[i]);
    }

    CHECK(passphrase_prompts == 0 && session_confirm_prompts == 1,
          "a removed method put something on the screen");

    /* And the session is still in step afterwards. */
    send_encrypted(payload, req(payload, sizeof(payload), "ping"));
    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE,
          "the session did not survive the refusals");
}

static size_t set_passphrase_request(uint8_t *buf, size_t cap, const char *pass);

static void test_rejection_and_timeout(void)
{
    printf("== refusing and ignoring the prompt both end the request\n");
    fresh_device();
    device_unlocked();
    confirmed_session(16);

    uint8_t payload[256];
    size_t len = set_passphrase_request(payload, sizeof(payload), "hunter2");

    scripted_outcome = SIGN_REJECTED;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_REJECTED, "user rejected the passphrase");

    /* Nobody touches the device. It must give up rather than hold the channel
     * open forever, and take the question off the screen as it does. */
    sign_expiries = 0;
    scripted_outcome = SIGN_PENDING;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_TIMEOUT, "nobody answered the prompt");
    CHECK(sign_expiries == 1, "the unanswered question was left on the screen");
}

/* The asymmetry that cost a hardware session: see the file header. */
static void test_errors_stay_encrypted_once_a_session_exists(void)
{
    printf("== an error inside a session is encrypted, or the two sides desync\n");
    fresh_device();
    device_unlocked();
    confirmed_session(17);

    uint8_t payload[64];

    /* One good exchange first, so both counters are off zero and a drift
     * shows up as a decrypt failure rather than as an accident. */
    send_encrypted(payload, req(payload, sizeof(payload), "getStatus"));
    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE, "getStatus inside the session failed");

    /* Now something the device does not implement. The tempting reply is a
     * plaintext error: it is simple, and it is the bug. The device advanced
     * its receive counter to decrypt this request, so a reply outside the
     * encrypted stream leaves the host's send counter one behind forever. */
    send_encrypted(payload, req(payload, sizeof(payload), "definitelyNotAMethod"));

    Frame err = next_frame();
    CHECK(err.present, "the unknown method went unanswered");
    CHECK(err.type == T_ENC_ERROR,
          "an in-session error came back as frame type 0x%02X, not 0x7E — "
          "every later frame will fail to decrypt", err.type);

    if (err.type == T_ENC_ERROR) {
        int n = host_open(err.payload, err.len);
        CHECK(n > 0, "the encrypted error did not open with the expected nonce");
        if (n > 0) {
            err.len = (size_t)n;
            uint32_t code = 0;
            CHECK(error_code(&err, &code) && code == E_MALFORMED,
                  "unknown method reported 0x%04X", code);
        }
    }

    /* The point of all of it: the channel still works afterwards. */
    send_encrypted(payload, req(payload, sizeof(payload), "ping"));
    f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE && f.len > 0,
          "the session did not survive an error — this is the desync");
}

static void test_malformed_input(void)
{
    printf("== truncated, lying, oversized and unparseable input\n");
    fresh_device();
    device_unlocked();

    uint8_t payload[64];
    size_t len = req(payload, sizeof(payload), "ping");

    /* A frame split across two reads. The endpoint shares its port with the
     * console, so this is the normal case, not the exotic one. */
    uint8_t frame[128];
    frame[0] = 'L'; frame[1] = 'K';
    frame[2] = 0; frame[3] = (uint8_t)(len + 1);
    frame[4] = T_REQUEST;
    memcpy(frame + 5, payload, len);

    raw_send(frame, 6);
    expect_silence("half a frame");
    raw_send(frame + 6, len + 5 - 6);
    CHECK(next_frame().present, "the second half of a split frame was dropped");

    /* A length field that lies about a frame far bigger than the device will
     * ever hold. It must be rejected on sight, without buffering toward it. */
    uint8_t liar[8] = { 'L', 'K', 0xFF, 0xFF, T_REQUEST, 0, 0, 0 };
    raw_send(liar, sizeof(liar));
    expect_silence("a length field claiming 65535 bytes");

    /* And a zero length, which is not a frame either. */
    uint8_t empty[6] = { 'L', 'K', 0x00, 0x00, 'x', 'y' };
    raw_send(empty, sizeof(empty));
    expect_silence("a zero-length frame");

    /* After all of that the device is still able to find a real frame — the
     * resynchronisation is the whole reason for the sync marker. */
    send_plain(payload, len);
    CHECK(next_frame().present, "the device never resynchronised");

    /* Console noise ahead of a frame, which is what actually arrives on this
     * port in production. */
    drop_pending();
    raw_send((const uint8_t *)"I (1234) wifi: some log line\n", 29);
    send_plain(payload, len);
    CHECK(next_frame().present, "log output ahead of a frame lost the frame");

    /* More bytes than the receive buffer, all at once. The device must drop
     * them rather than grow, and must recover afterwards. */
    drop_pending();
    static uint8_t flood[2048];
    memset(flood, 0x41, sizeof(flood));
    raw_send(flood, sizeof(flood));
    send_plain(payload, len);
    CHECK(next_frame().present, "the device did not recover from a flood");

    /* CBOR outside the supported subset. A float, an indefinite-length map,
     * and a 64-bit argument: each is a grammar the device deliberately does
     * not implement, and each must be a refusal rather than a guess. */
    static const uint8_t a_float[] = { 0xfb, 0x40, 0x09, 0x21, 0xfb, 0x54, 0x44, 0x2d, 0x18 };
    send_plain(a_float, sizeof(a_float));
    expect_error(T_ERROR, E_MALFORMED, "an IEEE double");

    static const uint8_t indefinite[] = { 0xbf, 0x66, 'm','e','t','h','o','d',
                                          0x64, 'p','i','n','g', 0xff };
    send_plain(indefinite, sizeof(indefinite));
    expect_error(T_ERROR, E_MALFORMED, "an indefinite-length map");

    static const uint8_t huge_arg[] = { 0xa1, 0x66, 'm','e','t','h','o','d',
                                        0x1b, 0,0,0,0,0,0,0,1 };
    send_plain(huge_arg, sizeof(huge_arg));
    expect_error(T_ERROR, E_MALFORMED, "a 64-bit argument");

    /* A frame carrying nothing but its type byte. */
    send_frame(T_REQUEST, NULL, 0);
    expect_error(T_ERROR, E_MALFORMED, "an empty payload");

    /* A method name longer than the device's buffer must not be truncated
     * into a shorter one that happens to be real. */
    CborWriter w;
    uint8_t long_method[128];
    cbor_writer_init(&w, long_method, sizeof(long_method));
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "pingpingpingpingpingpingpingpingpingpingping");
    send_plain(long_method, w.length);
    expect_error(T_ERROR, E_MALFORMED, "an overlong method name");

    /* An unknown frame type is not a frame this device speaks. */
    send_frame(0x42, payload, len);
    expect_error(T_ERROR, E_MALFORMED, "an unknown frame type");

    /* An encrypted frame with no session behind it. */
    static const uint8_t junk[32] = { 0 };
    send_frame(T_ENC_REQUEST, junk, sizeof(junk));
    expect_error(T_ERROR, E_SESSION, "an encrypted frame with no session");
}

static void test_tampered_frame_tears_down_the_session(void)
{
    printf("== a forged tag ends the session rather than dropping one frame\n");
    fresh_device();
    device_unlocked();
    confirmed_session(18);

    uint8_t payload[64];
    size_t len = req(payload, sizeof(payload), "ping");

    uint8_t buf[128];
    memcpy(buf, payload, len);
    size_t sealed = host_seal(buf, len);
    buf[0] ^= 0x01;
    send_frame(T_ENC_REQUEST, buf, sealed);

    expect_error(T_ERROR, E_SESSION, "a tampered frame");
    CHECK(session_state() == SESSION_IDLE,
          "a failed tag left the session standing (state %d)", session_state());
}

/* ------------------------------------------------------------ zSign* */

#include "zcash-sign.h"
#include "zsign_c.h"
#include "sha2.h"
#include "../../vectors/pczt_vectors.h"

#define E_ZSIGN(rc) (0x0500u | ((unsigned)-(rc) & 0xFF))

static uint8_t *load_vector(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (b && fread(b, 1, *len, f) != *len) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static size_t zsign_begin_req(uint8_t *buf, size_t cap, uint32_t len, const uint8_t *sha,
                              size_t sha_len, const char *network)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 4 + (network != NULL));
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "zSignBegin");
    cbor_write_text(&w, "len");
    cbor_write_uint(&w, len);
    cbor_write_text(&w, "sha256");
    cbor_write_bytes(&w, sha, sha_len);
    cbor_write_text(&w, "account");
    cbor_write_uint(&w, 0);
    if (network) {
        cbor_write_text(&w, "network");
        cbor_write_text(&w, network);
    }
    return w.length;
}

static size_t zsign_simple_req(uint8_t *buf, size_t cap, const char *method)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, method);
    return w.length;
}

static void zsign_chunk(uint32_t offset, const uint8_t *data, size_t n)
{
    static uint8_t buf[PROTOCOL_MAX_FRAME];
    CborWriter w;
    cbor_writer_init(&w, buf, sizeof buf);
    cbor_write_map(&w, 3);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "zSignChunk");
    cbor_write_text(&w, "offset");
    cbor_write_uint(&w, offset);
    cbor_write_text(&w, "data");
    cbor_write_bytes(&w, data, n);
    send_encrypted(buf, w.length);
}

/* Begin + every chunk, each acknowledged with the running count. */
static bool zsign_upload(const uint8_t *b, size_t len, const uint8_t sha[32])
{
    uint8_t req[128];
    send_encrypted(req, zsign_begin_req(req, sizeof req, (uint32_t)len, sha, 32, "test"));
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (!(f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len))) return false;
    if (!cbor_map_find(body, body_len, "maxChunk", &it) || it.type != CBOR_UINT) return false;
    uint32_t max = it.value;
    for (size_t off = 0; off < len; off += max) {
        size_t n = len - off < max ? len - off : max;
        zsign_chunk((uint32_t)off, b + off, n);
        f = next_reply();
        if (!(f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len) &&
              cbor_map_find(body, body_len, "received", &it) && it.value == off + n)) {
            return false;
        }
    }
    return true;
}

static void test_zsign(void)
{
    printf("== zSign*: chunked upload, device checks, review, signatures back\n");
    const zv_pczt_vector *v = &ZV_PCZT[0];
    size_t len = 0;
    uint8_t *pczt = load_vector(v->file, &len);
    CHECK(pczt != NULL, "cannot read %s", v->file);
    if (!pczt) return;
    uint8_t sha[32];
    sha256_Raw(pczt, len, sha);
    uint8_t req[128];

    fresh_device();
    pin_set("123456");
    pin_verify("123456");
    wallet_unlock("123456", 6);
    fake_wallet_preload(v->mnemonic);

    send_plain(req, zsign_begin_req(req, sizeof req, (uint32_t)len, sha, 32, "test"));
    expect_error(T_ERROR, E_SESSION, "zSignBegin before any handshake");

    confirmed_session(31);
    send_encrypted(req, zsign_begin_req(req, sizeof req, (uint32_t)len, sha, 31, "test"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "a 31-byte hash");
    send_encrypted(req, zsign_begin_req(req, sizeof req, (uint32_t)len, sha, 32, NULL));
    expect_error(T_ENC_ERROR, E_MALFORMED, "no network");
    send_encrypted(req, zsign_begin_req(req, sizeof req, 0, sha, 32, "test"));
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_TOO_LARGE), "an empty upload");
    send_encrypted(req, zsign_begin_req(req, sizeof req, ZCASH_PCZT_MAX + 1, sha, 32, "test"));
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_TOO_LARGE), "an oversized upload");
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_STATE), "zSignFinish with nothing uploaded");

    printf("== zSignChunk out of order drops the upload\n");
    send_encrypted(req, zsign_begin_req(req, sizeof req, (uint32_t)len, sha, 32, "test"));
    (void)next_reply();
    zsign_chunk(100, pczt + 100, 100);
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_OFFSET), "a chunk at a gap");
    zsign_chunk(0, pczt, 100);
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_STATE), "a chunk after the upload was dropped");

    printf("== the user rejects: no signature, upload gone\n");
    CHECK(zsign_upload(pczt, len, sha), "upload failed");
    int reviews = zsign_reviews;
    scripted_outcome = SIGN_REJECTED;
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    expect_error(T_ENC_ERROR, E_REJECTED, "a rejected review");
    CHECK(zsign_reviews == reviews + 1, "no review was shown");
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_STATE), "the rejected upload survived");

    printf("== the user approves: signatures that verify, over the reviewed sighash\n");
    CHECK(zsign_upload(pczt, len, sha), "upload failed");
    scripted_outcome = SIGN_APPROVED;
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    CHECK(f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len),
          "zSignFinish was not answered with a result");
    if (f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len)) {
        CHECK(shown_zsign.fee == v->fee, "the review showed fee %llu",
              (unsigned long long)shown_zsign.fee);
        CHECK(cbor_map_find(body, body_len, "sighash", &it) && it.type == CBOR_BYTES &&
              it.value == 32 && memcmp(it.data, shown_zsign.sighash, 32) == 0,
              "returned sighash is not the reviewed one");
        CHECK(cbor_map_find(body, body_len, "signatures", &it) && it.type == CBOR_ARRAY &&
              it.value == 1, "expected one signature");
        /* Walk the array: [{pool, index, sig}]. */
        CborReader r;
        int32_t err = 0;
        zc_ctx *c = zc_pczt_new(pczt, len, &err);
        const uint8_t *arr = NULL;
        {
            /* Re-find the array position inside body. */
            CborReader br;
            CborItem k;
            cbor_reader_init(&br, body, body_len);
            cbor_read(&br, &k);   /* the map */
            while (cbor_read(&br, &k)) {
                if (k.type == CBOR_TEXT && k.value == 10 && memcmp(k.data, "signatures", 10) == 0) {
                    arr = body + br.pos;
                    break;
                }
                cbor_skip(&br);
            }
        }
        CHECK(arr != NULL, "signatures not found");
        if (arr && c) {
            cbor_reader_init(&r, arr, body_len - (size_t)(arr - body));
            CborItem a;
            cbor_read(&r, &a);
            for (uint32_t i = 0; i < a.value; i++) {
                const uint8_t *entry = arr + r.pos;
                size_t elen = body_len - (size_t)(entry - body);
                CborItem pool, index, sig;
                bool ok = cbor_map_find(entry, elen, "pool", &pool) &&
                          cbor_map_find(entry, elen, "index", &index) &&
                          cbor_map_find(entry, elen, "sig", &sig) && sig.value == 64;
                CHECK(ok, "signature %u malformed", i);
                if (ok) {
                    zs_sig zs = { .pool = (uint8_t)pool.value, .index = index.value };
                    memcpy(zs.sig, sig.data, 64);
                    CHECK(zc_pczt_verify_sig(c, &zs) == ZS_OK, "signature %u does not verify", i);
                }
                cbor_skip(&r);
            }
        }
        if (c) zc_pczt_free(c);
    }

    printf("== refused before review: hash mismatch, tampered PCZT\n");
    uint8_t bad_sha[32];
    memcpy(bad_sha, sha, 32);
    bad_sha[0] ^= 1;
    CHECK(zsign_upload(pczt, len, bad_sha), "upload failed");
    reviews = zsign_reviews;
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_HASH), "a PCZT that does not match its hash");

    const zv_pczt_vector *bad = NULL;
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) {
        if (strcmp(ZV_PCZT[i].name, "r_output_value_tampered") == 0) bad = &ZV_PCZT[i];
    }
    size_t blen = 0;
    uint8_t *bp = bad ? load_vector(bad->file, &blen) : NULL;
    CHECK(bp != NULL, "tampered vector missing");
    if (bp) {
        uint8_t bsha[32];
        sha256_Raw(bp, blen, bsha);
        CHECK(zsign_upload(bp, blen, bsha), "upload failed");
        send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
        expect_error(T_ENC_ERROR, E_ZSIGN(ZS_E_OUTPUT_COMMITMENT), "a tampered output value");
        free(bp);
    }
    CHECK(zsign_reviews == reviews, "a refused PCZT reached the review screen");

    printf("== locking drops an upload\n");
    CHECK(zsign_upload(pczt, len, sha), "upload failed");
    pin_lock();
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "zSignFinish while locked");
    pin_verify("123456");
    send_encrypted(req, zsign_simple_req(req, sizeof req, "zSignFinish"));
    expect_error(T_ENC_ERROR, E_ZSIGN(ZSIGN_E_STATE), "an upload survived a lock");

    zcash_sign_reset();
    free(pczt);
}

/* ------------------------------------------------------------ zGetAccount */

#include "../../vectors/keys_vectors.h"

extern int fake_zcash_derive_calls;

static size_t zget_account_request(uint8_t *buf, size_t cap, int account, const char *network)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 1 + (account >= 0) + (network != NULL));
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "zGetAccount");
    if (account >= 0) {
        cbor_write_text(&w, "account");
        cbor_write_uint(&w, (uint32_t)account);
    }
    if (network) {
        cbor_write_text(&w, "network");
        cbor_write_text(&w, network);
    }
    return cbor_writer_ok(&w) ? w.length : 0;
}

static void test_zget_account(void)
{
    printf("== zGetAccount: session and unlock required, network named, vector answer\n");
    fresh_device();
    device_has_a_wallet();

    uint8_t payload[96];
    size_t len = zget_account_request(payload, sizeof(payload), 0, "test");

    send_plain(payload, len);
    expect_error(T_ERROR, E_SESSION, "zGetAccount before any handshake");

    confirmed_session(23);
    int calls = fake_zcash_derive_calls;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "zGetAccount while locked");
    CHECK(fake_zcash_derive_calls == calls, "derived while the device was locked");

    pin_set("123456");
    pin_verify("123456");

    /* The network is never defaulted. */
    send_encrypted(payload, zget_account_request(payload, sizeof(payload), 0, NULL));
    expect_error(T_ENC_ERROR, E_MALFORMED, "zGetAccount without a network");
    send_encrypted(payload, zget_account_request(payload, sizeof(payload), 0, "regtest"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "zGetAccount on an unknown network");

    CborWriter w;
    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 3);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "zGetAccount");
    cbor_write_text(&w, "account");
    cbor_write_uint(&w, 0x80000000u);
    cbor_write_text(&w, "network");
    cbor_write_text(&w, "test");
    send_encrypted(payload, w.length);
    expect_error(T_ENC_ERROR, E_MALFORMED, "a hardened account index");

    for (int net = 0; net <= 1; net++) {
        for (int acct = 0; acct <= 1; acct++) {
            const zv_key_vector *v = NULL;
            for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
                if (ZV_KEYS[i].network == net && ZV_KEYS[i].account == (uint32_t)acct &&
                    !ZV_KEYS[i].passphrase[0]) { v = &ZV_KEYS[i]; break; }
            }
            send_encrypted(payload, zget_account_request(payload, sizeof(payload), acct,
                                                         net ? "test" : "main"));
            Frame f = next_reply();
            const uint8_t *body;
            size_t body_len;
            CborItem it;
            char text[512];
            CHECK(f.present && f.type == T_ENC_RESPONSE, "zGetAccount was not answered");
            if (!(f.present && result_body(&f, &body, &body_len))) continue;
            CHECK(cbor_map_find(body, body_len, "ufvk", &it) && cbor_text_copy(&it, text, sizeof text) &&
                  strcmp(text, v->ufvk) == 0, "ufvk differs from librustzcash (%s)", v->name);
            CHECK(cbor_map_find(body, body_len, "ua", &it) && cbor_text_copy(&it, text, sizeof text) &&
                  strcmp(text, v->ua) == 0, "ua differs from librustzcash (%s)", v->name);
            CHECK(cbor_map_find(body, body_len, "tAddress", &it) && cbor_text_copy(&it, text, sizeof text) &&
                  strcmp(text, v->transparent_address) == 0, "tAddress differs (%s)", v->name);
            CHECK(cbor_map_find(body, body_len, "seedFingerprint", &it) && it.type == CBOR_BYTES &&
                  it.value == 32 && memcmp(it.data, v->seed_fingerprint, 32) == 0,
                  "seedFingerprint differs (%s)", v->name);
            CHECK(cbor_map_find(body, body_len, "account", &it) && it.type == CBOR_UINT &&
                  it.value == (uint32_t)acct, "account not echoed");
        }
    }
}

/* -------------------------------------------------------- zSetAccountMeta */

#include "zcash-meta.h"

static size_t zset_meta_request(uint8_t *buf, size_t cap, uint32_t account, uint32_t birthday,
                                const char *label)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 2 + (birthday != 0) + (label != NULL));
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "zSetAccountMeta");
    cbor_write_text(&w, "account");
    cbor_write_uint(&w, account);
    if (birthday) {
        cbor_write_text(&w, "birthday");
        cbor_write_uint(&w, birthday);
    }
    if (label) {
        cbor_write_text(&w, "label");
        cbor_write_text(&w, label);
    }
    return cbor_writer_ok(&w) ? w.length : 0;
}

/* The birthday in a zGetAccount answer for `account`, or 0 if absent. */
static uint32_t zget_birthday(uint32_t account)
{
    uint8_t payload[96];
    send_encrypted(payload, zget_account_request(payload, sizeof payload, (int)account, "test"));
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (!(f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len))) return 0xFFFFFFFFu;
    return cbor_map_find(body, body_len, "birthday", &it) && it.type == CBOR_UINT ? it.value : 0;
}

static void test_zset_account_meta(void)
{
    printf("== zSetAccountMeta: a companion records the birthday; zGetAccount returns it\n");
    fresh_device();
    zmeta__reset_for_test();
    device_has_a_wallet();

    uint8_t payload[128];
    size_t len = zset_meta_request(payload, sizeof payload, 0, 3000000, "Main");
    send_plain(payload, len);
    expect_error(T_ERROR, E_SESSION, "zSetAccountMeta before any handshake");
    confirmed_session(24);
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "zSetAccountMeta while locked");
    pin_set("123456");
    pin_verify("123456");

    CHECK(zget_birthday(0) == 0, "a birthday before any was set");
    send_encrypted(payload, len);
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    CHECK(f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len) &&
          cbor_map_find(body, body_len, "stored", &it) && it.type == CBOR_UINT && it.value == 1,
          "a stored wallet's record was not stored");
    CHECK(zget_birthday(0) == 3000000, "zGetAccount does not return the birthday");
    CHECK(zget_birthday(1) == 0, "another account got it");

    /* A label alone keeps the birthday. */
    send_encrypted(payload, zset_meta_request(payload, sizeof payload, 0, 0, "Renamed"));
    next_reply();
    ZcashMeta m;
    CHECK(zmeta_get(0, &m) && m.birthday == 3000000 && strcmp(m.label, "Renamed") == 0,
          "a partial update lost a field");

    send_encrypted(payload, zset_meta_request(payload, sizeof payload, 0x80000000u, 1, NULL));
    expect_error(T_ENC_ERROR, E_MALFORMED, "a hardened account");
    send_encrypted(payload, zset_meta_request(payload, sizeof payload, 0, 0, "tab\there"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "a control character in the label");
    send_encrypted(payload, zset_meta_request(payload, sizeof payload, 0, 0,
                                              "0123456789012345678901234567890123"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "a label over 32 characters");

    /* With a passphrase, the record is for this session only. */
    wallet_set_passphrase("TREZOR", 6);
    send_encrypted(payload, zset_meta_request(payload, sizeof payload, 0, 3100000, NULL));
    f = next_reply();
    CHECK(f.present && result_body(&f, &body, &body_len) &&
          cbor_map_find(body, body_len, "stored", &it) && it.value == 0,
          "a passphrase wallet's record was stored in flash");
    CHECK(zget_birthday(0) == 3100000, "the session record is not returned");
    wallet_clear_passphrase();
    CHECK(zget_birthday(0) == 3000000, "the base wallet saw the hidden wallet's birthday");
}

/* ------------------------------------------- zGetFeatures, zShowAddress */

static size_t method_only(uint8_t *buf, size_t cap, const char *method)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, method);
    return cbor_writer_ok(&w) ? w.length : 0;
}

static size_t zshow_request(uint8_t *buf, size_t cap, uint32_t account, const char *network)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 3);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "zShowAddress");
    cbor_write_text(&w, "account");
    cbor_write_uint(&w, account);
    cbor_write_text(&w, "network");
    cbor_write_text(&w, network);
    return cbor_writer_ok(&w) ? w.length : 0;
}

/* Seen on the first board: the PIN gate opens before the UI has decrypted
 * the vault, and a companion asking at once was told "derivation failed".
 * The request now waits for the wallet, and says what is missing. */
static void test_zcash_waits_for_the_wallet(void)
{
    printf("== Zcash requests wait for the vault, and name what is missing\n");
    fresh_device();
    confirmed_session(26);
    pin_set("123456");
    pin_verify("123456");   /* PIN gate open, vault still closed */

    uint8_t payload[96];
    size_t len = zget_account_request(payload, sizeof payload, 0, "test");
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "vault never opened");

    wallet_unlock("123456", 6);   /* open, but nothing stored */
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NO_WALLET, "no wallet on the device");

    fake_wallet_preload(TEST_MNEMONIC);
    send_encrypted(payload, len);
    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE, "a ready wallet was not answered");
}

static void test_zget_features(void)
{
    printf("== zGetFeatures: before a session, what the signer accepts\n");
    fresh_device();
    uint8_t payload[64];
    send_plain(payload, method_only(payload, sizeof payload, "zGetFeatures"));
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    CHECK(f.present && f.type == T_RESPONSE && result_body(&f, &body, &body_len),
          "zGetFeatures was not answered in plaintext");
    if (!(f.present && result_body(&f, &body, &body_len))) return;
    char text[16];
    CHECK(cbor_map_find(body, body_len, "backend", &it) && cbor_text_copy(&it, text, sizeof text) &&
          strcmp(text, "both") == 0, "backend is not BOTH by default");
    CHECK(cbor_map_find(body, body_len, "branchIds", &it) && it.type == CBOR_ARRAY &&
          it.value == ZCASH_SIGN_BRANCH_COUNT, "branchIds");
    CHECK(cbor_map_find(body, body_len, "maxPczt", &it) && it.value == ZCASH_PCZT_MAX, "maxPczt");
    CHECK(cbor_map_find(body, body_len, "storage", &it) && it.type == CBOR_ARRAY &&
          it.value == (LEEK_HAS_SDCARD ? 3u : 2u), "storage modes");
    CHECK(cbor_map_find(body, body_len, "showAddress", &it) && it.value == 1, "showAddress");
}

static void test_zshow_address(void)
{
    printf("== zShowAddress: shown on the device; the address only after a yes\n");
    fresh_device();
    device_has_a_wallet();
    uint8_t payload[96];
    size_t len = zshow_request(payload, sizeof payload, 0, "test");

    send_plain(payload, len);
    expect_error(T_ERROR, E_SESSION, "zShowAddress before any handshake");
    confirmed_session(25);
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "zShowAddress while locked");
    pin_set("123456");
    pin_verify("123456");

    const zv_key_vector *v = &ZV_KEYS[1];   /* test/abandon-art/pp0/acct0 */
    address_checks = 0;
    scripted_outcome = SIGN_REJECTED;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_REJECTED, "a mismatch on the device");
    CHECK(address_checks == 1 && strcmp(shown_ua, v->ua) == 0, "the device showed \"%s\"", shown_ua);

    sign_expiries = 0;
    scripted_outcome = SIGN_PENDING;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_TIMEOUT, "nobody answered");
    CHECK(sign_expiries == 1, "the question was left on the screen");

    scripted_outcome = SIGN_APPROVED;
    send_encrypted(payload, len);
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    char text[256];
    CHECK(f.present && f.type == T_ENC_RESPONSE && result_body(&f, &body, &body_len) &&
          cbor_map_find(body, body_len, "ua", &it) && cbor_text_copy(&it, text, sizeof text) &&
          strcmp(text, v->ua) == 0, "the confirmed address was not returned");

    send_encrypted(payload, zshow_request(payload, sizeof payload, 0, "regtest"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "an unknown network");
    send_encrypted(payload, zshow_request(payload, sizeof payload, 0x80000000u, "test"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "a hardened account");
    scripted_outcome = SIGN_APPROVED;
}

/* ----------------------------------------------------------- selectWallet */

static size_t select_wallet_request(uint8_t *buf, size_t cap, uint64_t index)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 2);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "selectWallet");
    cbor_write_text(&w, "index");
    cbor_write_uint(&w, index);
    return cbor_writer_ok(&w) ? w.length : 0;
}

/* Which seed the device is deriving from, by its fingerprint. The switch has
 * to reach derivation, not merely a status field. */
static bool active_fingerprint(uint32_t *out)
{
    return wallet_get_master_fingerprint(out) == WALLET_OK;
}

static void test_select_wallet(void)
{
    printf("== selectWallet moves between stored seeds, and refuses the rest\n");
    fresh_device();
    device_has_a_wallet();

    uint8_t payload[96];
    size_t len = select_wallet_request(payload, sizeof(payload), 1);

    send_plain(payload, len);
    expect_error(T_ERROR, E_SESSION, "selectWallet before any handshake");

    confirmed_session(22);
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "selectWallet while locked");

    pin_set("123456");
    pin_verify("123456");
    fake_wallet_preload(
        "legal winner thank year wave sausage worth useful legal winner thank year "
        "wave sausage worth useful legal winner thank year wave sausage worth title");

    uint32_t before = 0, after = 0;
    CHECK(active_fingerprint(&before), "no fingerprint before the switch");

    send_encrypted(payload, select_wallet_request(payload, sizeof(payload), 1));
    Frame f = next_reply();
    const uint8_t *body;
    size_t body_len;
    CborItem it;
    CHECK(f.present && f.type == T_ENC_RESPONSE, "selectWallet was not answered");
    if (f.present && result_body(&f, &body, &body_len)) {
        CHECK(cbor_map_find(body, body_len, "activeWallet", &it) &&
              it.type == CBOR_UINT && it.value == 1,
              "selectWallet did not report the wallet it selected");
    }

    CHECK(active_fingerprint(&after), "no fingerprint after the switch");
    /* The switch has to reach derivation, not merely a status field: a
     * selectWallet that updates a counter and leaves the keys alone is worse
     * than one that fails, because the screen would then name wallet 1 while
     * the keys came from wallet 2. */
    CHECK(before != after,
          "selecting a different wallet left the same seed active (%08X)",
          (unsigned)after);

    /* Out of range, in every direction the host can reach for. 256 and 257 are
     * the ones worth naming: a uint8_t cast would turn them into 0 and 1, and 1
     * is a wallet that exists. */
    static const uint64_t bad[] = { 0, 3, 99, 255, 256, 257, 0xFFFFFFFFu };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        send_encrypted(payload, select_wallet_request(payload, sizeof(payload), bad[i]));
        expect_error(T_ENC_ERROR, E_NO_WALLET, "an out-of-range wallet index");
    }
    /* Whatever it refused, it must not have moved. */
    uint32_t still = 0;
    CHECK(active_fingerprint(&still), "no fingerprint after the refusals");
    CHECK(still == after,
          "a refused selectWallet changed the active wallet anyway");

    /* No index, and an index of the wrong type. */
    CborWriter w;
    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "selectWallet");
    send_encrypted(payload, w.length);
    expect_error(T_ENC_ERROR, E_MALFORMED, "selectWallet with no index");

    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 2);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "selectWallet");
    cbor_write_text(&w, "index");
    cbor_write_text(&w, "2");
    send_encrypted(payload, w.length);
    expect_error(T_ENC_ERROR, E_MALFORMED, "an index sent as text");

    /* A passphrase belongs to the seed it was entered against. Switching seeds
     * must drop it, or the user lands in a third wallet nobody named. */
    wallet_set_passphrase("hunter2", 7);
    CHECK(wallet_has_passphrase(), "the fixture did not apply a passphrase");
    send_encrypted(payload, select_wallet_request(payload, sizeof(payload), 2));
    (void)next_reply();
    CHECK(!wallet_has_passphrase(),
          "switching wallets carried the passphrase across");
}

/* ---------------------------------------------------------- setPassphrase */

static size_t set_passphrase_request(uint8_t *buf, size_t cap, const char *pass)
{
    CborWriter w;
    cbor_writer_init(&w, buf, cap);
    cbor_write_map(&w, 2);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "setPassphrase");
    cbor_write_text(&w, "passphrase");
    cbor_write_text(&w, pass);
    return cbor_writer_ok(&w) ? w.length : 0;
}

/** Do these bytes appear anywhere in the reply? */
static bool contains(const uint8_t *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    if (n > hay_len) return false;
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) return true;
    }
    return false;
}

static void test_set_passphrase(void)
{
    printf("== setPassphrase confirms a fingerprint, and a refusal is recoverable\n");
    fresh_device();
    device_has_a_wallet();

    uint8_t payload[256];
    size_t len = set_passphrase_request(payload, sizeof(payload), "hunter2");

    send_plain(payload, len);
    expect_error(T_ERROR, E_SESSION, "setPassphrase before any handshake");

    confirmed_session(23);
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_NOT_UNLOCKED, "setPassphrase while locked");
    CHECK(passphrase_prompts == 0, "a locked device asked to confirm a passphrase");
    CHECK(!wallet_has_passphrase(), "a locked device applied a passphrase");

    pin_set("123456");
    pin_verify("123456");

    /* The happy path. The fingerprint the device shows is the one it answers
     * with: a reply naming a different wallet than the screen did would make
     * the confirmation meaningless. */
    scripted_outcome = SIGN_APPROVED;
    send_encrypted(payload, len);

    CHECK(passphrase_prompts == 1, "the passphrase was applied without a prompt");
    CHECK(wallet_has_passphrase(), "an approved passphrase was not applied");
    uint32_t applied_xfp = 0;
    CHECK(wallet_get_master_fingerprint(&applied_xfp) == WALLET_OK &&
          applied_xfp == shown_passphrase_xfp,
          "the screen was shown %08X, not the fingerprint of the wallet applied",
          (unsigned)shown_passphrase_xfp);

    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE, "setPassphrase was not answered");

    const uint8_t *body;
    size_t body_len;
    CborItem it;
    if (f.present && result_body(&f, &body, &body_len)) {
        CHECK(cbor_map_find(body, body_len, "fingerprint", &it) &&
              it.type == CBOR_UINT && it.value == shown_passphrase_xfp,
              "the reply names a different wallet than the screen did");
        CHECK(!cbor_map_find(body, body_len, "address", &it),
              "the reply still carries an address");
        CHECK(cbor_map_find(body, body_len, "passphrase", &it) &&
              it.type == CBOR_UINT && it.value == 1,
              "the reply does not report the passphrase as active");
        /* Never the passphrase itself, in any field. */
        CHECK(!contains(f.payload, f.len, "hunter2"),
              "the reply echoes the passphrase back");
    }

    /* A refusal has to be recoverable: the wrong wallet must not stay selected
     * just because the user said no. That is the entire reason the fingerprint
     * is shown before the passphrase is used for anything. */
    wallet_clear_passphrase();
    scripted_outcome = SIGN_REJECTED;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_REJECTED, "the user did not recognise the wallet");
    CHECK(!wallet_has_passphrase(),
          "a rejected passphrase stayed applied - the user is now in a wallet "
          "they refused");

    scripted_outcome = SIGN_PENDING;
    send_encrypted(payload, len);
    expect_error(T_ENC_ERROR, E_TIMEOUT, "nobody answered the fingerprint prompt");
    CHECK(!wallet_has_passphrase(), "an unanswered passphrase stayed applied");

    /* Malformed input is refused rather than guessed at, and nothing is applied
     * on the way to refusing it. */
    scripted_outcome = SIGN_APPROVED;
    passphrase_prompts = 0;

    CborWriter w;
    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 1);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "setPassphrase");
    send_encrypted(payload, w.length);
    expect_error(T_ENC_ERROR, E_MALFORMED, "setPassphrase with no passphrase");

    send_encrypted(payload, set_passphrase_request(payload, sizeof(payload), ""));
    expect_error(T_ENC_ERROR, E_MALFORMED, "an empty passphrase");

    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 2);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "setPassphrase");
    cbor_write_text(&w, "passphrase");
    cbor_write_bytes(&w, (const uint8_t *)"hunter2", 7);
    send_encrypted(payload, w.length);
    expect_error(T_ENC_ERROR, E_MALFORMED, "a passphrase sent as bytes");

    /* Longer than the buffer, and outside what the device's own keyboard can
     * produce: a passphrase enterable only from the app is a wallet the user
     * cannot reach without the app. */
    char too_long[80];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    send_encrypted(payload, set_passphrase_request(payload, sizeof(payload), too_long));
    expect_error(T_ENC_ERROR, E_MALFORMED, "an over-long passphrase");

    send_encrypted(payload,
                   set_passphrase_request(payload, sizeof(payload), "bad\x01pass"));
    expect_error(T_ENC_ERROR, E_MALFORMED, "a passphrase with a control byte");

    CHECK(passphrase_prompts == 0, "a malformed passphrase reached the screen");
    CHECK(!wallet_has_passphrase(), "a malformed passphrase was applied anyway");
}


/* ==========================================================================
 * BLE transport (ROADMAP T25, T57)
 *
 * The point of these is not that chunking works — sim/test_ble_chunk.c covers
 * that layer on its own bytes. It is that the SAME dispatch answers over BLE:
 * same frames, same errors, no sync marker, and no second code path that could
 * quietly disagree with the cable. And that the two transports are never both
 * live, which is the property session.c's single nonce pair depends on.
 * ========================================================================== */

/* What the device notified, as chunks, exactly as a peer would see them. */
static uint8_t ble_chunks[64][BLE_CHUNK_MAX_FRAME + 1];
static size_t  ble_chunk_len[64];
static int     ble_chunk_count;
static uint16_t ble_mtu = 23;

static bool ble_capture(void *ctx, const uint8_t *chunk, size_t len)
{
    (void)ctx;
    if (ble_chunk_count >= 64) return false;
    memcpy(ble_chunks[ble_chunk_count], chunk, len);
    ble_chunk_len[ble_chunk_count] = len;
    ble_chunk_count++;
    return true;
}

/* Stands in for ble.c's notify path: the same ble_chunk_split call, at the same
 * negotiated MTU, into a buffer instead of a radio. */
static void ble_writer(const uint8_t *frame, size_t len)
{
    ble_chunk_split(frame, len, ble_mtu, ble_capture, NULL);
}

/* Send a request over BLE the way a host does: encode the frame with NO sync
 * marker, split it at the MTU, and deliver each chunk as its own GATT write. */
static void ble_send_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t frame[PROTOCOL_MAX_FRAME + 8];
    size_t body = len + 1;
    frame[0] = (uint8_t)(body >> 8);
    frame[1] = (uint8_t)body;
    frame[2] = type;
    if (len) memcpy(frame + 3, payload, len);

    static uint8_t wire[64][BLE_CHUNK_MAX_FRAME + 1];
    static size_t  wire_len[64];

    /* Chunked by the same function the device uses, because that is what the
     * TypeScript client does too — chunkForBle is one algorithm on both ends. */
    ble_chunk_count = 0;
    ble_chunk_split(frame, body + 2, ble_mtu, ble_capture, NULL);
    int n = ble_chunk_count;
    for (int i = 0; i < n; i++) {
        memcpy(wire[i], ble_chunks[i], ble_chunk_len[i]);
        wire_len[i] = ble_chunk_len[i];
    }
    ble_chunk_count = 0;

    static BleReassembler rx;
    ble_chunk_reset(&rx);
    for (int i = 0; i < n; i++) {
        BleChunkResult res = ble_chunk_push(&rx, wire[i], wire_len[i]);
        if (res == BLE_CHUNK_FRAME_READY) {
            protocol_handle_frame(rx.buf, rx.len);
            ble_chunk_reset(&rx);
        } else if (res == BLE_CHUNK_ERROR) {
            printf("  FAIL: the device refused its own chunking\n");
            failures++;
        }
    }
}

/** Reassemble whatever the device notified back into one frame, undecrypted. */
static Frame ble_next_frame(void)
{
    Frame f;
    memset(&f, 0, sizeof(f));

    BleReassembler rx;
    ble_chunk_reset(&rx);
    for (int i = 0; i < ble_chunk_count; i++) {
        if (ble_chunk_push(&rx, ble_chunks[i], ble_chunk_len[i]) == BLE_CHUNK_FRAME_READY) {
            f.present = true;
            f.type = rx.buf[2];
            f.len  = rx.len - 3;
            memcpy(f.payload, rx.buf + 3, f.len);
            break;
        }
    }
    ble_chunk_count = 0;
    return f;
}

/** The next notified frame, decrypted if it is one of the encrypted types. */
static Frame ble_next_reply(void)
{
    Frame f = ble_next_frame();
    if (f.present && (f.type == T_ENC_RESPONSE || f.type == T_ENC_ERROR)) {
        int n = host_open(f.payload, f.len);
        f.len = n < 0 ? 0 : (size_t)n;
    }
    return f;
}

static void ble_selected(void)
{
    fresh_device();
    ble_chunk_count = 0;
    transport_set(TRANSPORT_BLE);
    protocol_set_writer(ble_writer);
}

static void test_ble_carries_the_same_frames(void)
{
    printf("== BLE answers the same frames as the cable, unmarked\n");

    ble_selected();

    uint8_t payload[64];
    ble_mtu = 23;                       /* the floor many stacks actually give */
    ble_send_frame(T_REQUEST, payload, req(payload, sizeof(payload), "ping"));

    CHECK(ble_chunk_count > 0, "no notification at all");

    /* The marker is a USB artifact. If it appeared here the host — which ships
     * BLE_USES_SYNC = false — would read 'L','K' as a length and hang. */
    CHECK(!(ble_chunk_count > 0 && ble_chunk_len[0] >= 3 &&
            ble_chunks[0][1] == 'L' && ble_chunks[0][2] == 'K'),
          "a sync marker leaked onto BLE");

    Frame f = ble_next_reply();
    CHECK(f.present, "ping produced no reassemblable frame");
    CHECK(f.type == T_RESPONSE, "ping answered with frame type 0x%02X", f.type);

    /* Byte-for-byte the cable's answer. Two transports, one dispatch. */
    fresh_device();
    transport_set(TRANSPORT_USB);   /* also detaches the BLE writer */
    send_plain(payload, req(payload, sizeof(payload), "ping"));
    Frame usb = next_frame();
    CHECK(usb.present && usb.type == f.type && usb.len == f.len &&
          memcmp(usb.payload, f.payload, f.len) == 0,
          "BLE and USB gave different answers to the same request");
}

static void test_ble_survives_a_split_write_at_mtu_23(void)
{
    printf("== a request split across many GATT writes is answered\n");

    ble_selected();
    device_has_a_wallet();

    /* getStatus's reply is comfortably over 19 bytes, so the answer has to be
     * chunked as well as the request. */
    ble_mtu = 23;
    uint8_t payload[64];
    ble_send_frame(T_REQUEST, payload, req(payload, sizeof(payload), "getStatus"));

    CHECK(ble_chunk_count >= 1, "no reply chunks");
    for (int i = 0; i < ble_chunk_count; i++) {
        CHECK(ble_chunk_len[i] <= 20,
              "chunk %d is %zu bytes, past the 19+1 an MTU of 23 allows",
              i, ble_chunk_len[i]);
    }
    Frame f = ble_next_reply();
    CHECK(f.present && f.type == T_RESPONSE, "getStatus was not answered over BLE");
}

static void test_ble_rejects_hostile_writes(void)
{
    printf("== hostile GATT writes are dropped without reaching dispatch\n");

    ble_selected();

    BleReassembler rx;
    ble_chunk_reset(&rx);

    /* A frame claiming 64 KB. Nothing may be buffered on that claim. */
    uint8_t huge[8] = { 0x80, 0xff, 0xff, 0x01, 0x00 };
    CHECK(ble_chunk_push(&rx, huge, 5) == BLE_CHUNK_ERROR, "an oversized frame was buffered");
    CHECK(ble_chunk_count == 0, "the device answered a frame it never received");

    /* Out of order: chunk 1 with no chunk 0. */
    uint8_t stray[8] = { 0x01, 0xaa, 0xbb };
    CHECK(ble_chunk_push(&rx, stray, 3) == BLE_CHUNK_ERROR, "an orphan chunk was accepted");
    CHECK(ble_chunk_count == 0, "the device answered an orphan chunk");

    /* And after all that noise the next real request still works — a peer that
     * misbehaves must not wedge the endpoint for the next one. */
    uint8_t payload[64];
    ble_send_frame(T_REQUEST, payload, req(payload, sizeof(payload), "ping"));
    Frame f = ble_next_reply();
    CHECK(f.present && f.type == T_RESPONSE, "the endpoint was wedged by bad chunks");
}

static void test_only_one_transport_is_live(void)
{
    printf("== USB and BLE are never both reachable (T57)\n");

    fresh_device();
    transport_init();

    /* Default is the cable, and BLE is off — not idle, off. */
    CHECK(transport_get() == TRANSPORT_USB, "the device did not default to USB");
    CHECK(!ble_transport_running(), "BLE was up before anyone selected it");

    uint8_t payload[64];
    send_plain(payload, req(payload, sizeof(payload), "ping"));
    CHECK(next_frame().present, "USB did not answer while selected");

    /* Select BLE: the radio comes up and the cable goes quiet. A device that
     * kept answering here would have two peers on one set of nonce counters. */
    transport_set(TRANSPORT_BLE);
    protocol_set_writer(ble_writer);
    CHECK(ble_transport_running(), "selecting BLE did not start the radio");

    drop_pending();
    ble_chunk_count = 0;
    send_plain(payload, req(payload, sizeof(payload), "ping"));
    expect_silence("USB while BLE is selected");
    /* Not just "no bytes on the cable": the cable must not have been PARSED.
     * A device that dispatches a USB request and notifies the answer over BLE
     * is still serving two peers from one set of nonce counters, which is the
     * fault this setting exists to prevent. */
    CHECK(ble_chunk_count == 0,
          "a USB request was dispatched and answered over BLE");

    /* Back to the cable: advertising stops. */
    transport_set(TRANSPORT_USB);
    CHECK(!ble_transport_running(), "BLE kept advertising after USB was selected");
    CHECK(transport_get() == TRANSPORT_USB, "the setting did not follow");

    send_plain(payload, req(payload, sizeof(payload), "ping"));
    CHECK(next_frame().present, "USB stayed deaf after being reselected");
}

static void test_switching_transports_drops_the_session(void)
{
    printf("== switching transports tears the session down (T57)\n");

    fresh_device();
    transport_set(TRANSPORT_USB);
    device_unlocked();
    confirmed_session(31);
    CHECK(session_state() == SESSION_ACTIVE, "no session to tear down");

    transport_set(TRANSPORT_BLE);
    /* A passkey confirmed on the cable does not authorise the radio. */
    CHECK(session_state() == SESSION_IDLE,
          "a confirmed session survived a transport switch (state %d)",
          session_state());

    transport_set(TRANSPORT_USB);
    CHECK(session_state() == SESSION_IDLE, "switching back left a session");
}


/* ==========================================================================
 * Two transports, one dispatch (ROADMAP T26)
 *
 * The BLE tests above prove that ping is answered over the radio. That is not
 * the property that matters. Hardware found the one that does: `getMnemonic`,
 * an unknown method, returned a clean error frame over the cable and produced
 * NOTHING AT ALL over BLE. A request that goes unanswered is worse than one
 * that is refused - on an active session the device's receive counter has
 * moved and the host's send counter has not, so the channel is silently dead
 * from then on, and with no request IDs in this protocol the host cannot even
 * tell which request it lost.
 *
 * So the two transports are compared by construction rather than by hope:
 * every case below runs twice, once down each channel, and the reply must
 * agree in frame type and in decrypted payload. Adding a case covers both
 * transports whether the author thought about BLE or not, which is the only
 * version of this that stays true.
 *
 * Encrypted replies are compared AFTER opening them: the device's session key
 * is ephemeral, so the ciphertext legitimately differs between the two runs
 * while the plaintext must not.
 * ========================================================================== */

typedef enum { VIA_USB, VIA_BLE } Via;

static const char *via_name(Via v) { return v == VIA_BLE ? "BLE" : "USB"; }

/** A fresh device with `v` as the selected transport, and nothing else. */
static void via_fresh(Via v)
{
    fresh_device();
    ble_chunk_count = 0;
    ble_mtu = 23;                  /* the floor, so replies chunk on BLE */
    if (v == VIA_BLE) {
        transport_set(TRANSPORT_BLE);
        protocol_set_writer(ble_writer);
    } else {
        transport_set(TRANSPORT_USB);
    }
}

static void via_send(Via v, uint8_t type, const uint8_t *payload, size_t len)
{
    if (v == VIA_BLE) {
        ble_send_frame(type, payload, len);
    } else {
        send_frame(type, payload, len);
    }
}

static void via_send_plain(Via v, const uint8_t *payload, size_t len)
{
    via_send(v, T_REQUEST, payload, len);
}

static void via_send_encrypted(Via v, const uint8_t *payload, size_t len)
{
    uint8_t buf[512];
    memcpy(buf, payload, len);
    via_send(v, T_ENC_REQUEST, buf, host_seal(buf, len));
}

/** The reply, with encrypted bodies opened so two runs can be compared. */
static Frame via_reply(Via v)
{
    return v == VIA_BLE ? ble_next_reply() : next_reply();
}

/** Handshake to PENDING over `v`. The USB-only twin is handshake(). */
static void via_handshake(Via v, uint8_t seed)
{
    host_keypair(seed);
    fill(host_nonce, sizeof(host_nonce), (uint8_t)(seed ^ 0x5a));

    uint8_t payload[128];
    CborWriter w;
    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 3);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "hello");
    cbor_write_text(&w, "version");
    cbor_write_uint(&w, PROTOCOL_VERSION);
    cbor_write_text(&w, "hostPubkey");
    cbor_write_bytes(&w, host_pub, sizeof(host_pub));
    via_send_plain(v, payload, w.length);

    Frame f = via_reply(v);
    const uint8_t *body;
    size_t body_len;
    CborItem it, commit_it;
    uint8_t committed[SESSION_COMMIT_SIZE];
    if (!f.present || !result_body(&f, &body, &body_len) ||
        !cbor_map_find(body, body_len, "devicePubkey", &it) ||
        it.type != CBOR_BYTES || it.value != 32 ||
        !cbor_map_find(body, body_len, "deviceCommit", &commit_it) ||
        commit_it.type != CBOR_BYTES || commit_it.value != SESSION_COMMIT_SIZE) {
        printf("  FAIL: handshake over %s did not produce a commitment\n", via_name(v));
        failures++;
        return;
    }
    memcpy(device_pub, it.data, 32);
    memcpy(committed, commit_it.data, sizeof(committed));

    cbor_writer_init(&w, payload, sizeof(payload));
    cbor_write_map(&w, 3);
    cbor_write_text(&w, "method");
    cbor_write_text(&w, "helloReveal");
    cbor_write_text(&w, "version");
    cbor_write_uint(&w, PROTOCOL_VERSION);
    cbor_write_text(&w, "hostNonce");
    cbor_write_bytes(&w, host_nonce, sizeof(host_nonce));
    via_send_plain(v, payload, w.length);

    f = via_reply(v);
    char passkey[SESSION_PASSKEY_LEN + 1];
    SessionTranscript t;
    uint8_t expected[SESSION_COMMIT_SIZE];
    if (!f.present || !result_body(&f, &body, &body_len) ||
        !cbor_map_find(body, body_len, "deviceNonce", &it) ||
        it.type != CBOR_BYTES || it.value != SESSION_NONCE_SIZE) {
        printf("  FAIL: handshake over %s revealed no nonce\n", via_name(v));
        failures++;
        return;
    }
    memcpy(device_nonce, it.data, SESSION_NONCE_SIZE);

    session_commitment(device_pub, host_pub, device_nonce, expected);
    if (memcmp(expected, committed, sizeof(expected)) != 0) {
        printf("  FAIL: %s revealed a nonce it had not committed to\n", via_name(v));
        failures++;
        return;
    }

    memcpy(t.host_public, host_pub, 32);
    memcpy(t.device_public, device_pub, 32);
    memcpy(t.host_nonce, host_nonce, SESSION_NONCE_SIZE);
    memcpy(t.device_nonce, device_nonce, SESSION_NONCE_SIZE);

    if (!session_derive(host_priv, device_pub, &t, k_h2d, k_d2h, passkey)) {
        printf("  FAIL: handshake over %s did not produce a session\n", via_name(v));
        failures++;
    }
}

static void via_confirmed_session(Via v, uint8_t seed)
{
    via_handshake(v, seed);
    session_confirm();
    session_up = true;
}

/* One conformance case. Each performs its own setup so the two runs share no
 * state, and returns the device's reply. */
typedef enum {
    CASE_PING,
    CASE_UNKNOWN_METHOD,          /* the getMnemonic that started this */
    CASE_TIER_NEEDS_SESSION,
    CASE_TIER_NEEDS_UNLOCK,
    CASE_UNKNOWN_METHOD_IN_SESSION,
    CASE_PLAINTEXT_IN_SESSION,
    CASE_NO_METHOD_KEY,
    CASE_NOT_CBOR,
    CASE_UNKNOWN_FRAME_TYPE,
    CASE_FORGED_TAG,
    CASE_COUNT
} ConformanceCase;

static const char *case_name(ConformanceCase c)
{
    switch (c) {
        case CASE_PING:                      return "ping";
        case CASE_UNKNOWN_METHOD:            return "getMnemonic (unknown method)";
        case CASE_TIER_NEEDS_SESSION:        return "selectWallet with no session";
        case CASE_TIER_NEEDS_UNLOCK:         return "selectWallet while locked";
        case CASE_UNKNOWN_METHOD_IN_SESSION: return "unknown method in a session";
        case CASE_PLAINTEXT_IN_SESSION:      return "plaintext request in a session";
        case CASE_NO_METHOD_KEY:             return "a request with no method";
        case CASE_NOT_CBOR:                  return "bytes that are not CBOR";
        case CASE_UNKNOWN_FRAME_TYPE:        return "an unknown frame type";
        case CASE_FORGED_TAG:                return "a forged tag";
        default:                             return "?";
    }
}

static Frame run_case(Via v, ConformanceCase c)
{
    uint8_t payload[96];
    size_t  len;

    via_fresh(v);

    switch (c) {
        case CASE_PING:
            via_send_plain(v, payload, req(payload, sizeof(payload), "ping"));
            break;

        case CASE_UNKNOWN_METHOD:
            /* The exact request the BLE probe sends and the cable answered
             * alone. It must be refused, and it must be refused audibly. */
            via_send_plain(v, payload, req(payload, sizeof(payload), "getMnemonic"));
            break;

        case CASE_TIER_NEEDS_SESSION:
            device_unlocked();
            via_send_plain(v, payload, req(payload, sizeof(payload), "selectWallet"));
            break;

        case CASE_TIER_NEEDS_UNLOCK:
            device_has_a_wallet();
            via_confirmed_session(v, 61);
            via_send_encrypted(v, payload, req(payload, sizeof(payload), "selectWallet"));
            break;

        case CASE_UNKNOWN_METHOD_IN_SESSION:
            device_unlocked();
            via_confirmed_session(v, 62);
            via_send_encrypted(v, payload,
                               req(payload, sizeof(payload), "getMnemonic"));
            break;

        case CASE_PLAINTEXT_IN_SESSION:
            device_unlocked();
            via_confirmed_session(v, 63);
            via_send_plain(v, payload, req(payload, sizeof(payload), "getStatus"));
            break;

        case CASE_NO_METHOD_KEY: {
            CborWriter w;
            cbor_writer_init(&w, payload, sizeof(payload));
            cbor_write_map(&w, 1);
            cbor_write_text(&w, "notmethod");
            cbor_write_uint(&w, 1);
            via_send_plain(v, payload, w.length);
            break;
        }

        case CASE_NOT_CBOR:
            memset(payload, 0xFF, 16);
            via_send_plain(v, payload, 16);
            break;

        case CASE_UNKNOWN_FRAME_TYPE:
            len = req(payload, sizeof(payload), "ping");
            via_send(v, 0x55, payload, len);
            break;

        case CASE_FORGED_TAG: {
            device_unlocked();
            via_confirmed_session(v, 64);
            uint8_t buf[128];
            len = req(payload, sizeof(payload), "ping");
            memcpy(buf, payload, len);
            size_t sealed = host_seal(buf, len);
            buf[0] ^= 0x01;
            via_send(v, T_ENC_REQUEST, buf, sealed);
            break;
        }

        default:
            break;
    }

    return via_reply(v);
}

static void test_both_transports_answer_the_same(void)
{
    printf("== every request is answered identically over USB and BLE (T26)\n");

    for (int c = 0; c < CASE_COUNT; c++) {
        Frame usb = run_case(VIA_USB, (ConformanceCase)c);
        Frame ble = run_case(VIA_BLE, (ConformanceCase)c);

        /* Silence is the failure this whole section exists for, so it is
         * checked first and named for what it costs. */
        CHECK(usb.present, "%s: the cable did not answer at all",
              case_name((ConformanceCase)c));
        CHECK(ble.present,
              "%s: BLE did not answer at all - the host will time out and, "
              "inside a session, the counters are now one apart",
              case_name((ConformanceCase)c));
        if (!usb.present || !ble.present) continue;

        CHECK(usb.type == ble.type,
              "%s: USB answered 0x%02X, BLE answered 0x%02X",
              case_name((ConformanceCase)c), usb.type, ble.type);
        CHECK(usb.len == ble.len && memcmp(usb.payload, ble.payload, usb.len) == 0,
              "%s: the two transports disagreed on the body",
              case_name((ConformanceCase)c));

        /* Every one of these is a refusal except the first two: check that
         * they carry an error code rather than an empty result, so a mutant
         * answering everything with a bare "ok" on both transports would
         * still be caught. */
        if (c != CASE_PING && c != CASE_PLAINTEXT_IN_SESSION) {
            uint32_t code = 0;
            CHECK(error_code(&usb, &code) && code != 0,
                  "%s: the reply carries no error code",
                  case_name((ConformanceCase)c));
        }
    }

    /* Back to the cable, and back to the USB writer. The last case ran over
     * BLE, and leaving the radio selected would send the next suite's replies
     * into a notification buffer nobody reads - which looks exactly like the
     * silence this test is about. */
    via_fresh(VIA_USB);
}

/* ==========================================================================
 * A reply matches the frame type of the request that caused it
 *
 * The device used to decide that on session STATE. Once a session existed, a
 * PLAINTEXT request came back ENCRYPTED - and the host that sent it had no
 * session, which is precisely why it was in plaintext. It had no keys, so it
 * CBOR-decoded raw ciphertext and reported "unsupported CBOR major type 7"
 * and "32 trailing bytes after CBOR value": random bytes spelled out as a data
 * format, where the honest answer was "no session".
 *
 * The counter argument for encrypting cuts the other way here, and this is the
 * part worth writing down. session_encrypt() advances the DEVICE's tx counter;
 * the host advances its rx counter only when it opens a reply (session.ts
 * decrypt()). An encrypted reply the host cannot open therefore leaves the
 * device->host stream one ahead forever - the old behaviour was itself a
 * desync. A plaintext request never advanced the device's rx counter, because
 * only session_decrypt() does, and a plaintext reply never advances the host's
 * tx counter, because the host defers that until it opens a reply. Both sides
 * stand still. That is why this is safe and encrypting was not.
 *
 * The rule the firmware now enforces: an encrypted reply is only ever produced
 * for a request that was successfully decrypted.
 * ========================================================================== */

static void test_a_reply_matches_its_request(void)
{
    printf("== a plaintext request gets a plaintext reply, session or not\n");

    fresh_device();
    device_unlocked();
    confirmed_session(70);

    uint8_t payload[64];

    /* One encrypted exchange first, so both directions are off zero and a
     * drift shows up as a decrypt failure rather than as an accident. */
    send_encrypted(payload, req(payload, sizeof(payload), "getStatus"));
    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE,
          "the session was not working to begin with");

    /* A host that lost its keys and fell back to plaintext. It cannot decrypt
     * anything, so an encrypted answer is unreadable by construction. */
    send_plain(payload, req(payload, sizeof(payload), "getStatus"));
    f = next_frame();
    CHECK(f.present, "a plaintext request inside a session went unanswered");
    CHECK(f.type == T_RESPONSE,
          "a plaintext request was answered with frame type 0x%02X - the host "
          "has no keys and will try to parse ciphertext as CBOR", f.type);

    const uint8_t *body;
    size_t body_len;
    CborItem it;
    CHECK(f.present && result_body(&f, &body, &body_len) &&
          cbor_map_find(body, body_len, "unlocked", &it),
          "the plaintext reply was not a readable getStatus result");

    /* And an unimplemented method the same way: plaintext in, plaintext out. */
    send_plain(payload, req(payload, sizeof(payload), "getMnemonic"));
    expect_error(T_ERROR, E_MALFORMED, "a plaintext unknown method in a session");

    /* The point of all of it: neither counter moved, so the encrypted channel
     * is still exactly where it was. If the device had encrypted either reply
     * above, its tx counter would be ahead and this would fail to open. */
    send_encrypted(payload, req(payload, sizeof(payload), "ping"));
    f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE && f.len > 0,
          "answering plaintext desynchronised the encrypted stream");

    /* Belt and braces on the other direction: a key operation still refuses in
     * plaintext, since a plaintext frame proves nothing about who sent it. */
    send_plain(payload, req(payload, sizeof(payload), "selectWallet"));
    expect_error(T_ERROR, E_SESSION, "plaintext selectWallet inside a session");
}

/* A complete request a transport cannot hand to the dispatcher is refused, not
 * dropped. BLE is the only transport with a queue between the two (its worker
 * cannot be the NimBLE host task, because signing waits on a human), and a
 * full queue used to be one log line the peer never saw. The queue itself
 * lives behind CONFIG_BT_NIMBLE_ENABLED and has no radio here; what is
 * testable, and what matters, is that the refusal it now sends is a real frame
 * and that it leaves the session alone. */
static void test_a_busy_transport_refuses_rather_than_drops(void)
{
    printf("== a request the transport cannot queue is refused, not dropped\n");

    fresh_device();
    device_unlocked();
    confirmed_session(71);

    uint8_t payload[64];
    send_encrypted(payload, req(payload, sizeof(payload), "getStatus"));
    Frame f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE,
          "the session was not working to begin with");

    /* Called directly, because the queue that triggers it on hardware lives
     * behind CONFIG_BT_NIMBLE_ENABLED. Nothing arrived on the port, so the
     * pump has to be told to collect what the device wrote. */
    protocol_send_transport_busy();
    collect_output();
    f = next_frame();
    CHECK(f.present, "a refused request produced no frame - that is the timeout");
    CHECK(f.type == T_ERROR,
          "the refusal came back as 0x%02X; nothing was decrypted, so the host "
          "cannot open an encrypted one", f.type);
    uint32_t code = 0;
    CHECK(error_code(&f, &code) && code == E_BUSY,
          "the refusal reported 0x%04X, not busy", code);

    /* Nothing was decrypted and nothing was encrypted, so the session is
     * untouched and the host may simply send the request again. */
    send_encrypted(payload, req(payload, sizeof(payload), "ping"));
    f = next_reply();
    CHECK(f.present && f.type == T_ENC_RESPONSE,
          "refusing a request broke the session it arrived on");
}

/* ==========================================================================
 * The advertised device name (T56)
 *
 * The bound is the feature. A legacy scan response holds 31 bytes; overflow it
 * and NimBLE rejects the whole advertisement, advertising never starts, and a
 * battery-powered device sits there looking fine while being invisible. That
 * shipped once already, back when the name shared the advertisement with the
 * 128-bit service UUID. A user typing a long name must not be able to
 * reproduce it.
 * ========================================================================== */

static void test_ble_name_is_bounded_and_persisted(void)
{
    printf("== the BLE name is bounded, refused when too long, and persists (T56)\n");

    fake_nvs_reset();
    ble_name_forget();

    CHECK(strcmp(ble_name_get(), BLE_NAME_DEFAULT) == 0,
          "a device nobody renamed does not advertise as " BLE_NAME_DEFAULT);

    CHECK(ble_name_set("Groceries"), "a perfectly ordinary name was refused");
    CHECK(strcmp(ble_name_get(), "Groceries") == 0, "the name did not take");

    /* Exactly at the bound: 2 bytes of AD header + 29 = 31, which fits. */
    char at_limit[BLE_NAME_MAX_LEN + 1];
    memset(at_limit, 'A', BLE_NAME_MAX_LEN);
    at_limit[BLE_NAME_MAX_LEN] = '\0';
    CHECK(ble_name_set(at_limit), "a name of exactly the maximum length was refused");
    CHECK(strlen(ble_name_get()) == BLE_NAME_MAX_LEN,
          "the maximum-length name did not take");

    /* One past it. Refused outright: truncating would advertise a device the
     * user never named, and they are the only one who could notice. */
    char too_long[BLE_NAME_MAX_LEN + 8];
    memset(too_long, 'B', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    CHECK(!ble_name_set(too_long),
          "an over-long name was accepted - advertising would stop");
    CHECK(strlen(ble_name_get()) == BLE_NAME_MAX_LEN,
          "a refused name changed the one already set");
    CHECK(strchr(ble_name_get(), 'B') == NULL,
          "an over-long name was silently truncated and stored");

    /* The bound is the one the radio actually has to satisfy, not a number
     * picked nearby. This is the assertion in ble.c, restated where it can be
     * checked without a build of the firmware. */
    CHECK(2 + BLE_NAME_MAX_LEN <= 31,
          "BLE_NAME_MAX_LEN does not fit a legacy scan response");

    /* Empty is not a name, and neither is anything the device's own keyboard
     * cannot produce or a scanner render. */
    CHECK(!ble_name_set(""), "an empty name was accepted");
    CHECK(!ble_name_set(NULL), "a null name was accepted");
    CHECK(!ble_name_set("Leek\nWallet"), "a control character was accepted");
    CHECK(!ble_name_set("Leek\x80Wallet"), "a non-ASCII byte was accepted");

    /* Persistence across a reboot, which is what "set on-device" has to mean. */
    CHECK(ble_name_set("Toaster"), "could not set a name to reboot with");
    fake_nvs_reboot();
    ble_name_forget();
    CHECK(strcmp(ble_name_get(), "Toaster") == 0, "the name did not survive a reboot");

    /* A stored value this firmware would not have written - a longer name from
     * another version, or a corrupt read - must not reach the radio either.
     * The default is the safe answer: it is known to fit. */
    static const size_t hostile_lengths[] = {
        BLE_NAME_MAX_LEN + 1,       /* one past the bound, and short enough to
                                     * be read back — the case that reaches the
                                     * validity check rather than the read
                                     * guard */
        BLE_NAME_MAX_LEN + 40,      /* and one far past it */
    };
    for (size_t i = 0; i < sizeof(hostile_lengths) / sizeof(hostile_lengths[0]); i++) {
        ble_name_forget();
        fake_nvs_reset();

        nvs_handle_t nvs;
        char hostile[BLE_NAME_MAX_LEN + 41];
        memset(hostile, 'C', sizeof(hostile));
        CHECK(nvs_open("leek_ui", NVS_READWRITE, &nvs) == ESP_OK,
              "could not stage a bad name");
        nvs_set_blob(nvs, "ble_name", hostile, hostile_lengths[i]);
        nvs_commit(nvs);
        nvs_close(nvs);

        CHECK(strlen(ble_name_get()) <= BLE_NAME_MAX_LEN,
              "a %zu-byte stored name was handed to the radio", hostile_lengths[i]);
        CHECK(strcmp(ble_name_get(), BLE_NAME_DEFAULT) == 0,
              "a %zu-byte stored name did not fall back to the default",
              hostile_lengths[i]);
    }

    /* Leave nothing behind for the suites that follow. */
    fake_nvs_reset();
    ble_name_forget();
}


/* ============================================================================
 * T42 - a host-supplied passphrase belongs to the host's session
 * ============================================================================ */

/* The failure: the app types a passphrase, the user approves it, the cable is
 * pulled or the phone walks out of BLE range - and the device is still in the
 * hidden wallet. The next thing to connect inherits it, derives from it and
 * signs with it, and nothing was ever re-approved.
 *
 * A passphrase entered ON the device is a different case and must survive; the
 * second half of this test is that distinction. */
static void test_a_host_passphrase_dies_with_its_session(void)
{
    printf("== a host-supplied passphrase does not outlive its session (T42)\n");
    fresh_device();
    device_has_a_wallet();
    device_unlocked();
    confirmed_session(44);

    uint8_t payload[256];
    size_t len = set_passphrase_request(payload, sizeof(payload), "hunter2");
    scripted_outcome = SIGN_APPROVED;
    send_encrypted(payload, len);
    (void)next_reply();
    CHECK(wallet_has_passphrase(), "setup: the passphrase was not applied");

    /* Every teardown funnels through session_reset(): a disconnect, a new
     * peer, a transport switch, a frame that failed to authenticate. */
    session_reset();
    CHECK(!wallet_has_passphrase(),
          "the passphrase survived the session that supplied it - whatever "
          "connects next inherits a hidden wallet nobody re-approved");

    /* A passphrase the user typed on the device is not the host's to revoke.
     * Dropping it on a disconnect would silently return them to the base
     * wallet, which looks exactly like an empty one.
     *
     * Set up as it actually happens: the host supplies one, and the user then
     * overrides it at the device. The endpoint has to stop considering the
     * passphrase its own at that point, or its own teardown takes the user's
     * passphrase with it. */
    confirmed_session(45);
    /* A new session restarts the device's nonce counters, so the host's have
     * to restart with them - the same thing a real client does on reconnect. */
    host_tx = host_rx = 0;
    scripted_outcome = SIGN_APPROVED;
    len = set_passphrase_request(payload, sizeof(payload), "hunter2");
    send_encrypted(payload, len);
    (void)next_reply();
    CHECK(wallet_has_passphrase(), "setup: the host passphrase was not applied");

    wallet_set_passphrase("typed-here", 10);
    protocol_note_device_passphrase();
    session_reset();
    CHECK(wallet_has_passphrase(),
          "a passphrase entered on the device was dropped when a host "
          "disconnected");
    wallet_clear_passphrase();
}

/* ==========================================================================
 * The mock leg of the conformance suite (ROADMAP T26)
 *
 * The two transports above are compared against each other because both are
 * this file's code. The Node mock is not: it lives in another language, in
 * another package, and every host test in the repository is only as
 * trustworthy as its agreement with what is compiled here. Twice it has
 * drifted and certified code the device refuses.
 *
 * A prose specification did not hold that line and cannot, so the comparison
 * is made mechanical: this pass replays a fixed corpus through the real
 * dispatch and writes down, byte for byte, the request that went in and the
 * plaintext reply that came out. mock-conformance.test.ts feeds the SAME
 * bytes to MockDevice and compares. Neither side gets to describe its own
 * behaviour in words.
 *
 * The requests are emitted rather than restated in TypeScript for the same
 * reason the tests above hand-roll frames: a corpus written twice is two
 * corpora, and they agree only until someone edits one of them.
 * ========================================================================== */

static const char *case_name_mock(int c);
static int run_all_tests(void);

typedef enum {
    MC_PING,
    MC_GET_FEATURES,
    MC_GET_FEATURES_BLIND_ON,
    MC_GET_STATUS_PLAIN,
    MC_GET_STATUS_IN_SESSION,
    MC_GET_STATUS_PLAIN_IN_SESSION,
    MC_UNKNOWN_METHOD_NO_SESSION,
    MC_UNKNOWN_METHOD_IN_SESSION,
    MC_NO_METHOD_KEY,
    MC_SELECT_WALLET_NO_SESSION,
    MC_SELECT_WALLET_LOCKED,
    MC_REMOVED_GET_ADDRESS,
    MC_REMOVED_SIGN_TX,
    MC_UNLOCK_WHILE_LOCKED,
    MC_UNLOCK_WHILE_UNLOCKED,
    MC_UNLOCK_NO_SESSION,
    MC_LOCK,
    MC_LOCK_NO_SESSION,
    MC_SELECT_WALLET_1,
    MC_SELECT_WALLET_MISSING_INDEX,
    MC_SELECT_WALLET_UNKNOWN,
    MC_SET_PASSPHRASE,
    MC_SET_PASSPHRASE_EMPTY,
    MC_SET_PASSPHRASE_NON_ASCII,
    MC_SET_PASSPHRASE_REJECTED,
    MC_SET_PASSPHRASE_LOCKED,
    MC_CASE_COUNT
} MockCase;

/* What the device has to be for the case to mean anything, in the four terms
 * the mock also has: does it hold a seed, is the PIN in, is a session up, is
 * blind signing on, and does the simulated user say yes. Written into the
 * vector file so the TypeScript side configures an equivalent device rather
 * than guessing from the case name. */
typedef struct {
    const char *name;
    bool        wallet;
    bool        unlocked;
    bool        session;
    bool        blind;
    bool        approve;
    bool        encrypted;      /* sent as 0x11 rather than 0x01 */
} MockSetup;

static MockSetup mock_setup(MockCase c)
{
    /* Defaults: a device with a seed, unlocked, session up, protections on,
     * user approves. Each case names only what it changes. */
    MockSetup s = { case_name_mock((int)c), true, true, true, false, true, true };

    switch (c) {
        case MC_PING:
        case MC_GET_FEATURES:
        case MC_GET_STATUS_PLAIN:
        case MC_UNKNOWN_METHOD_NO_SESSION:
        case MC_NO_METHOD_KEY:
        case MC_SELECT_WALLET_NO_SESSION:
        case MC_UNLOCK_NO_SESSION:
        case MC_LOCK_NO_SESSION:
            s.session = false;
            s.unlocked = false;
            s.encrypted = false;
            break;
        case MC_GET_FEATURES_BLIND_ON:
            s.session = false; s.unlocked = false; s.encrypted = false;
            s.blind = true;
            break;
        case MC_GET_STATUS_PLAIN_IN_SESSION:
            s.encrypted = false;    /* session up, request sent in the clear */
            break;
        case MC_SELECT_WALLET_LOCKED:
        case MC_UNLOCK_WHILE_LOCKED:
        case MC_SET_PASSPHRASE_LOCKED:
            s.unlocked = false;
            break;
        case MC_SET_PASSPHRASE_REJECTED:
            s.approve = false;
            break;
        default:
            break;
    }
    return s;
}

/* The corpus. One buffer, one builder per case, and the frame type comes from
 * the setup so a case cannot be sent down a channel its tier does not allow. */
static size_t mock_request(MockCase c, uint8_t *buf, size_t cap)
{
    CborWriter w;

    switch (c) {
        case MC_PING:
            return req(buf, cap, "ping");
        case MC_GET_FEATURES:
        case MC_GET_FEATURES_BLIND_ON:
            return req(buf, cap, "getFeatures");
        case MC_GET_STATUS_PLAIN:
        case MC_GET_STATUS_IN_SESSION:
        case MC_GET_STATUS_PLAIN_IN_SESSION:
            return req(buf, cap, "getStatus");

        /* The request that started all of this: an unknown method, which the
         * cable answered and the radio did not. Here it asks a different
         * question - whether the mock refuses it for the same REASON. */
        case MC_UNKNOWN_METHOD_NO_SESSION:
        case MC_UNKNOWN_METHOD_IN_SESSION:
            return req(buf, cap, "getMnemonic");

        case MC_NO_METHOD_KEY:
            cbor_writer_init(&w, buf, cap);
            cbor_write_map(&w, 1);
            cbor_write_text(&w, "notmethod");
            cbor_write_uint(&w, 1);
            return cbor_writer_ok(&w) ? w.length : 0;

        case MC_SELECT_WALLET_NO_SESSION:
        case MC_SELECT_WALLET_LOCKED:
            return select_wallet_request(buf, cap, 1);

        /* Methods LeekWallet served and this firmware does not.
         * The mock has to refuse them for the same reason the device does. */
        case MC_REMOVED_GET_ADDRESS:
            return req(buf, cap, "getAddress");
        case MC_REMOVED_SIGN_TX:
            return req(buf, cap, "signTransaction");

        case MC_UNLOCK_WHILE_LOCKED:
        case MC_UNLOCK_WHILE_UNLOCKED:
        case MC_UNLOCK_NO_SESSION:
            return req(buf, cap, "unlock");
        case MC_LOCK:
        case MC_LOCK_NO_SESSION:
            return req(buf, cap, "lock");

        case MC_SELECT_WALLET_1:
            return select_wallet_request(buf, cap, 1);
        case MC_SELECT_WALLET_UNKNOWN:
            return select_wallet_request(buf, cap, 9);
        case MC_SELECT_WALLET_MISSING_INDEX:
            return req(buf, cap, "selectWallet");

        case MC_SET_PASSPHRASE:
        case MC_SET_PASSPHRASE_REJECTED:
        case MC_SET_PASSPHRASE_LOCKED:
            return set_passphrase_request(buf, cap, "hunter2");
        case MC_SET_PASSPHRASE_EMPTY:
            return set_passphrase_request(buf, cap, "");
        case MC_SET_PASSPHRASE_NON_ASCII:
            return set_passphrase_request(buf, cap, "caf\xc3\xa9");

        default:
            return 0;
    }
}

static const char *case_name_mock(int c)
{
    switch ((MockCase)c) {
        case MC_PING:                          return "ping";
        case MC_GET_FEATURES:                  return "getFeatures";
        case MC_GET_FEATURES_BLIND_ON:         return "getFeatures, blind signing on";
        case MC_GET_STATUS_PLAIN:              return "getStatus, no session";
        case MC_GET_STATUS_IN_SESSION:         return "getStatus in a session";
        case MC_GET_STATUS_PLAIN_IN_SESSION:   return "plaintext getStatus in a session";
        case MC_UNKNOWN_METHOD_NO_SESSION:     return "unknown method, no session";
        case MC_UNKNOWN_METHOD_IN_SESSION:     return "unknown method in a session";
        case MC_NO_METHOD_KEY:                 return "a request with no method";
        case MC_SELECT_WALLET_NO_SESSION:      return "selectWallet with no session";
        case MC_SELECT_WALLET_LOCKED:          return "selectWallet while locked";
        case MC_REMOVED_GET_ADDRESS:           return "getAddress (removed)";
        case MC_REMOVED_SIGN_TX:               return "signTransaction (removed)";
        case MC_UNLOCK_WHILE_LOCKED:           return "unlock while locked";
        case MC_UNLOCK_WHILE_UNLOCKED:         return "unlock while already unlocked";
        case MC_UNLOCK_NO_SESSION:             return "unlock with no session";
        case MC_LOCK:                          return "lock";
        case MC_LOCK_NO_SESSION:               return "lock with no session";
        case MC_SELECT_WALLET_1:               return "selectWallet 1";
        case MC_SELECT_WALLET_MISSING_INDEX:   return "selectWallet with no index";
        case MC_SELECT_WALLET_UNKNOWN:         return "selectWallet, no such wallet";
        case MC_SET_PASSPHRASE:                return "setPassphrase";
        case MC_SET_PASSPHRASE_EMPTY:          return "setPassphrase, empty";
        case MC_SET_PASSPHRASE_NON_ASCII:      return "setPassphrase, not printable ASCII";
        case MC_SET_PASSPHRASE_REJECTED:       return "setPassphrase rejected on device";
        case MC_SET_PASSPHRASE_LOCKED:         return "setPassphrase while locked";
        default:                               return "?";
    }
}

static void write_hex(FILE *out, const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) fprintf(out, "%02x", bytes[i]);
}

/** Run one case against the real dispatch and write its vector. */
static void emit_case(FILE *out, MockCase c, bool first)
{
    MockSetup s = mock_setup(c);

    fresh_device();
    if (s.wallet) {
        device_has_a_wallet();
    }
    if (s.unlocked) {
        pin_set("123456");
        pin_verify("123456");
    }
    if (s.blind) {
        CHECK(blind_signing_set(true), "%s: could not enable blind signing", s.name);
    }
    if (s.session) {
        confirmed_session((uint8_t)(0x80 + c));
    }
    scripted_outcome = s.approve ? SIGN_APPROVED : SIGN_REJECTED;

    uint8_t payload[1024];
    size_t len = mock_request(c, payload, sizeof(payload));
    if (len == 0) {
        printf("  FAIL: %s: the request did not fit its buffer\n", s.name);
        failures++;
        return;
    }

    if (s.encrypted) {
        send_encrypted(payload, len);
    } else {
        send_plain(payload, len);
    }

    Frame f = next_reply();
    if (!f.present) {
        /* Silence is the one answer that cannot be compared, and the one that
         * kills a session. Nothing downstream can catch it, so it dies here. */
        printf("  FAIL: %s: the device did not answer at all\n", s.name);
        failures++;
        return;
    }

    fprintf(out, "%s\n  {\n", first ? "" : ",");
    fprintf(out, "    \"name\": \"%s\",\n", s.name);
    fprintf(out, "    \"setup\": { \"wallet\": %s, \"unlocked\": %s, "
                 "\"session\": %s, \"blindSigning\": %s, \"approve\": %s },\n",
            s.wallet ? "true" : "false", s.unlocked ? "true" : "false",
            s.session ? "true" : "false", s.blind ? "true" : "false",
            s.approve ? "true" : "false");
    fprintf(out, "    \"frameType\": %u,\n", s.encrypted ? T_ENC_REQUEST : T_REQUEST);
    fprintf(out, "    \"request\": \"");
    write_hex(out, payload, len);
    fprintf(out, "\",\n");
    fprintf(out, "    \"replyType\": %u,\n", f.type);
    fprintf(out, "    \"reply\": \"");
    write_hex(out, f.payload, f.len);
    fprintf(out, "\"\n  }");

    /* Whatever the case left on, the next one starts from a fresh device -
     * except this, which is cached outside the simulated flash. */
    blind_signing_set(false);
}

static int emit_vectors(const char *path)
{
    FILE *out = fopen(path, "w");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }

    fprintf(out, "[");
    for (int c = 0; c < MC_CASE_COUNT; c++) {
        emit_case(out, (MockCase)c, c == 0);
    }
    fprintf(out, "\n]\n");
    fclose(out);

    printf("wrote %d conformance vectors to %s\n", (int)MC_CASE_COUNT, path);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    /* Emitting is a mode rather than a second binary: the corpus has to go
     * through the same stubs, the same fixtures and the same real protocol.c
     * the suite below exercises, and a separate program would be a second
     * copy of all three that drifts from this one exactly as the mock did. */
    if (argc == 3 && strcmp(argv[1], "--emit-vectors") == 0) {
        return emit_vectors(argv[2]);
    }

    return run_all_tests();
}

static int run_all_tests(void)
{
    test_nothing_is_answered_before_a_transport_is_chosen();
    test_plaintext_ping_and_features();
    test_status_is_public_but_thin();
    test_version_mismatch_is_named();
    test_reveal_is_required_before_anything_is_shown();
    test_hello_waits_for_a_user_who_is_answering();
    test_a_reveal_cannot_interrupt_a_confirmation_either();
    test_keys_need_a_session_and_a_passkey();
    test_locked_device_refuses_keys();
    test_lock_answers_before_it_closes_the_channel();
    test_removed_methods_are_unknown();
    test_rejection_and_timeout();
    test_errors_stay_encrypted_once_a_session_exists();
    test_malformed_input();
    test_tampered_frame_tears_down_the_session();
    test_select_wallet();
    test_zget_account();
    test_zset_account_meta();
    test_zcash_waits_for_the_wallet();
    test_zget_features();
    test_zshow_address();
    test_zsign();
    test_set_passphrase();
    test_ble_carries_the_same_frames();
    test_ble_survives_a_split_write_at_mtu_23();
    test_ble_rejects_hostile_writes();
    test_only_one_transport_is_live();
    test_switching_transports_drops_the_session();
    test_both_transports_answer_the_same();
    test_a_reply_matches_its_request();
    test_a_busy_transport_refuses_rather_than_drops();
    test_ble_name_is_bounded_and_persisted();

    test_a_host_passphrase_dies_with_its_session();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
