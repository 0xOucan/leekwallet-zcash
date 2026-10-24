/**
 * LeekWallet UI Framework
 * Screen state machine and rendering
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include "button.h"
#include "leek-wallet.h"
#include "board.h"

/**
 * How many BIP44 accounts the device offers on its own screens (T45).
 *
 * m/44'/<coin>'/<account>': the account level is a separate identity off the
 * same seed — separate history, separate balance — which docs/VAULT.md argues
 * should be the ordinary way to hold several wallets, rather than storing
 * several seeds and multiplying the number of things to back up.
 *
 * Ten, because a selector on four buttons has to stay a short cycle. It is
 * NOT a limit on what the device can derive — which is why any future signing
 * confirmation must render the whole path rather than an index.
 */
#define HD_ACCOUNT_COUNT 10

/**
 * Screen identifiers
 */
typedef enum {
    SCREEN_BOOT,
    SCREEN_PIN_SETUP,
    SCREEN_PIN_UNLOCK,
    SCREEN_PIN_CHANGE,
    SCREEN_MAIN_MENU,
    SCREEN_WALLET_INFO,
    SCREEN_WALLET_SELECT,
    SCREEN_MNEMONIC_DISPLAY,
    SCREEN_MNEMONIC_ENTRY,
    SCREEN_WALLET_CREATE,
    SCREEN_SETTINGS,
    SCREEN_WIPE_CONFIRM,
    SCREEN_ENTROPY,
    SCREEN_MNEMONIC_VERIFY,
    SCREEN_SESSION_CONFIRM,
    SCREEN_PASSPHRASE,
    SCREEN_PASSPHRASE_CONFIRM,
    SCREEN_HOST_PASSPHRASE_CONFIRM,
    /* Renaming the device for BLE advertising (T56). */
    SCREEN_BLE_NAME,
    /* What a temporary seed costs, before one is typed (T69). Its own screen
     * rather than a line in a list: the user is about to enter a
     * mode where locking, rebooting or a flat battery destroys their seed, and
     * that has to be read rather than discovered. */
    SCREEN_TEMP_SEED,
    /* Locking is one gesture, held rather than tapped. See LOCK_HOLD_US: a
     * tap is how you leave a menu, and BACK on the home screen used to mean
     * both that and "throw away the session". */
    SCREEN_LOCK_HOLD,
    SCREEN_LOCK_CONFIRM,
    /* The QR return path: a UR, animated when it needs more than one frame.
     * Shows whatever ui_show_ur() was handed and nothing else; the screen
     * that decided to share it is the one that said what it is. */
    SCREEN_QR_OUT,
    /* Main menu -> Receive: the account's UA and t-address, as text pages
     * and QR codes (zcash-receive.c). */
    SCREEN_SHOW_ADDRESS,
    /* Main menu -> Scan, on a board with a camera. Hosts the capture loop and
     * assembles URs; every type is refused until Phase 5 (see airgap.h). */
    SCREEN_SCAN,
    /* A host asked the device to sign a Zcash transaction (zSignFinish).
     * Pages from zcash-review.c; ACCEPT on the last page signs. */
    SCREEN_ZSIGN_REVIEW,
    /* Main menu -> SD Card: load a seed from a .zkv file for this session,
     * or save the open seed to one (roadmap 1.4, sd-seed.h). */
    SCREEN_SD_SEED,
    /* A host asked to show an address (zShowAddress): the UA in full, then
     * "same as the app?". Answered through the sign-outcome channel. */
    SCREEN_ADDRESS_CHECK,
    SCREEN_COUNT
} screen_id_t;

/**
 * How long BACK must be held on the home screen to lock the device.
 *
 * BACK on the home screen means "lock", and it used to mean it on a single
 * press -- the same press that leaves every other screen. One stray tap threw
 * away the session: the passphrase (which wallet_lock() zeroes), a temporary
 * seed, and the encrypted channel with it, so the user retyped a passphrase on
 * four buttons to get back where they were.
 *
 * A hold fixes that without making locking harder in the moment that matters.
 * Any hold past a few hundred milliseconds defeats an accidental press
 * outright, so the length is a matter of feel rather than of safety; three
 * seconds is deliberate without being a puzzle. The progress bar is not
 * decoration -- a hold with no feedback reads as a button that did not work,
 * and the user lets go.
 *
 * Only this path holds. Auto-lock is auto-lock: it is not a press, there is no
 * accident to prevent, and nobody is there to hold anything. The host's `lock`
 * is deliberate by construction. End Temp Seed already says what it does.
 */
#define LOCK_HOLD_US 3000000

/**
 * Screen interface - each screen implements these callbacks
 */
typedef struct {
    void (*enter)(void);                    /* Called when screen becomes active */
    void (*render)(void);                   /* Called to draw the screen */
    void (*on_button)(button_id_t btn);     /* Called on button press */
    /* Called when leaving the screen, with the screen being moved to.
     *
     * The destination is passed because the decision this hook exists for -
     * whether a secret in a static buffer is still needed - depends on it.
     * The seed display and the seed verification screen share
     * `mnemonic_buffer` and hand off to each other in both directions, so a
     * hook that zeroed unconditionally would break wallet creation rather
     * than secure it (AUDIT S5). */
    void (*exit)(screen_id_t next);
} screen_t;

/**
 * Initialize UI subsystem
 * Call after OLED and button init
 */
void ui_init(void);

/**
 * Set the current screen
 * Calls exit() on old screen and enter() on new screen
 * @param screen Screen to switch to
 */
void ui_set_screen(screen_id_t screen);

/**
 * Get the current screen ID
 * @return Current screen
 */
screen_id_t ui_get_screen(void);

/**
 * Handle a button press event
 * Routes to current screen's on_button callback
 * @param btn Button that was pressed
 */
void ui_handle_button(button_id_t btn);

/**
 * Render the current screen
 * Calls the current screen's render callback
 */
void ui_render(void);

/**
 * Request a screen re-render
 * Call this when screen data has changed
 */
void ui_invalidate(void);

/**
 * Check if screen needs re-rendering
 * @return true if ui_invalidate() was called
 */
bool ui_needs_render(void);

/**
 * Clear the invalidation flag after rendering
 */
void ui_clear_invalidation(void);

/**
 * Register a custom screen
 * @param id Screen ID to register
 * @param screen Screen callbacks
 */
void ui_register_screen(screen_id_t id, const screen_t *screen);

/**
 * Show `cbor` as a `ur:<type>` QR, animated if it needs several frames, and
 * return to `back` when the user leaves. The bytes are copied.
 *
 * UI task only. It does not say what is being shared: the caller must already
 * have put that on screen and had it confirmed, because once this is up the
 * panel is all QR. Returns false (and changes nothing) if the message cannot
 * be shown.
 */
bool ui_show_ur(const char *type, const uint8_t *cbor, size_t len,
                screen_id_t back);

/**
 * UI main task - runs the screen state machine
 * This should be started as a FreeRTOS task
 * @param pvParameters Unused
 */
void ui_task(void *pvParameters);

/**
 * The boot splash (splash.h): the logo animation, then the handle, the
 * version and codename. Blocks for at most splash_total_ms(); any input ends
 * it early and is consumed. Called once by ui_task() before the first
 * render; does nothing on a board with no panel.
 */
void ui_boot_splash(void);

/**
 * Work the UI task owes itself, run once per loop *after* the repaint.
 *
 * Two things live here, and both are the same shape: something that must not
 * happen inside a button handler.
 *
 *   - Slow work a screen announced (AUDIT S8f). Seed generation takes long
 *     enough to need a "Generating..." frame in front of it, and the old code
 *     got that frame onto the panel by calling ui_render() from inside the
 *     button handler. That gave the screen two render paths and let a repaint
 *     re-enter a screen that was mid-transition. Now the handler only sets the
 *     text and marks the screen dirty; the loop paints it, and the generation
 *     runs here, once the frame is already on the glass.
 *
 *   - State another task changed under a screen that is already up (T42): a
 *     host clearing the passphrase while the wallet screen shows an address
 *     and a fingerprint derived from it.
 *
 * Called by ui_task(). The host tests call it directly, which is the whole
 * point of it being a function rather than a block inside the loop.
 */
void ui_poll_deferred(void);

/**
 * Ask the user to compare the session passkey.
 *
 * Called from the protocol task when a handshake begins. The UI task picks it
 * up on its next pass rather than switching screens from another task.
 */
/**
 * Which BIP44 account the device's own screens are browsing.
 *
 * Exposed so `getStatus` can report it. The host derives under whatever path it
 * asks for and does not have to agree with this — but if it does not *know*
 * what the device is set to, the device's address browser and the app's address
 * list can show different addresses for the same wallet with nothing saying so.
 * That is a receive-side hazard only: every signing confirmation renders the
 * whole path, so nothing can be signed from an account the user did not see.
 */
uint32_t ui_hd_account(void);

/**
 * The Zcash network the receive screens show addresses for: ZS_NET_MAIN (0)
 * or ZS_NET_TEST (1), set under Settings > Network and persisted. The
 * board's network: a companion's request for the other one is refused
 * (protocol.c, network_is_the_boards), and signing also checks the network
 * the PCZT declares. Read from the protocol task; see zcash_network in ui.c.
 */
uint8_t ui_zcash_network(void);

/**
 * A companion asked for the other network than ui_zcash_network(), and the
 * protocol task refused it (PROTOCOL.md, error 0x0528). The board says so
 * on its screen for a few seconds, over whatever is there; any button
 * dismisses it. `asked` is the network the companion named. Safe from the
 * protocol task: it only raises a flag.
 */
void ui_notify_wrong_network(uint8_t asked);

void ui_request_session_confirm(void);

/**
 * Whether the device is presently asking the user a question (M-2).
 *
 * True while a screen is up that the user has to answer and that an incoming
 * frame must not be allowed to replace: a signing approval, a passphrase the
 * host proposed, a wipe, a pairing comparison, a seed on display. The protocol
 * endpoint consults this before honouring a plaintext `hello`, because that
 * message is unauthenticated and resets the session — so without the check,
 * anyone able to write to the port could yank an approval out from under the
 * user's eyes at a moment of their choosing, and the request the user was
 * reading would vanish with it.
 *
 * Read from the protocol task while the UI task owns the screen graph. That is
 * safe in the direction it is used: the answer can go stale between the read
 * and the reply, and the worst outcome is a handshake refused a moment late,
 * which the host retries. The reverse mistake — letting a handshake through
 * because the flag had not been set yet — is not reachable, because the screen
 * is switched before the request that raised it is answered.
 */
bool ui_user_is_answering(void);

/**
 * Ask the user to unlock, on behalf of a host request.
 *
 * The PIN is entered on the device and never travels. The host polls
 * `getStatus` to learn whether it worked.
 */
void ui_request_unlock(void);

/** Lock on behalf of a host request. */
void ui_request_lock(void);

/**
 * Show the wallet a host-supplied passphrase produced, and ask the user to
 * confirm it is theirs (PROTOCOL.md 5).
 *
 * A mistyped or substituted passphrase does not error — it derives a different,
 * perfectly valid wallet — so recognising this seed fingerprint is the only
 * thing that catches it. The caller derives it, on the task that applied the
 * passphrase. Rejection is the recoverable path: the caller clears the
 * passphrase.
 */
void ui_request_passphrase_confirm(uint32_t fingerprint);

#if LEEK_HAS_ZCASH
#include "zsign_rust.h"
/**
 * Show a checked Zcash transaction for review (roadmap 3.4) and ask the user
 * to sign it. The answer arrives through ui_sign_outcome(). `s` must stay
 * valid until then; it is the summary zcash_sign_check() produced.
 */
void ui_request_zsign_review(const zs_summary *s, uint8_t network);

/**
 * A host asked the device to show an address (zShowAddress), so the user can
 * compare it with what the app displays. The unified address is paged in
 * full; the last page asks whether it matches. The answer arrives through
 * ui_sign_outcome(): APPROVED = same address, REJECTED = not. Both strings
 * are copied.
 */
void ui_request_show_address(const char *ua, const char *t_address, uint32_t account,
                             uint8_t network);
#endif

typedef enum {
    SIGN_PENDING,
    SIGN_APPROVED,
    SIGN_REJECTED,
} SignOutcome;

/* The approval window closed with nobody answering: take the question off the
 * screen. Safe from the protocol task. */
void ui_sign_expire(void);

SignOutcome ui_sign_outcome(void);
void ui_sign_clear(void);

#endif /* UI_H */
