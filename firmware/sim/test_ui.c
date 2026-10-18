/**
 * Host-native tests for src/ui.c (ROADMAP T0.2-T0.4)
 *
 * The UI is the part of this firmware that has historically only been testable
 * by flashing a board and pressing buttons, which is why screen bugs kept
 * reaching hardware. Here the real ui.c runs against a framebuffer OLED and a
 * scripted button sequence, so "what is on screen after these presses" is an
 * assertion.
 *
 * Assertions are on the text the firmware drew, not on pixels. A pixel golden
 * fails on every cosmetic change and tells you nothing about which one; "row 0
 * says Word 24/24" fails only when the device is actually wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "fake_oled.h"
#include "fake_wallet.h"

#include "ble.h"
#include "transport.h"
#include "ui.h"
#include "zcash-review.h"
#include "pin.h"
#include "button.h"
bool fake_protocol_rx_enabled(void);
#include "mnemonic-entry.h"
#include "text-entry.h"
#include "blind-signing.h"
#include "memzero.h"
#include "leek-wallet.h"
#include "session.h"
#include "qr-out.h"
#include "airgap.h"
#include "ur-encoder.h"
#include "zcash-account.h"
#include "sdcard.h"
#include "text-entry.h"
#include "../../vectors/keys_vectors.h"
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

/* Test hooks from ui.c and pin.c (compiled with -DLEEK_HOST_TEST). */
void        pin__reset_static_state_for_test(void);
void        ui__reset_static_state_for_test(void);
bool        ui__check_autolock_for_test(void);
void        ui__service_sign_expiry_for_test(void);
void        ui__service_wrong_network_for_test(void);
void        ui__service_lock_hold_for_test(void);
void        fake_button_hold(button_id_t id);
void        fake_button_release(void);
void        ui__service_host_lock_for_test(void);
void        ui__service_qr_out_for_test(void);
void        ui__scan_feed_for_test(const char *ur);
const char *ui__scan_status_for_test(void);
const char *ui__master_xfp_for_test(void);
const char *ui__master_zfp_for_test(void);
int ui__receive_page_for_test(void);
TextEntry *ui__sd_entry_for_test(void);
char *mkdtemp(char *tmpl);   /* POSIX; a shim header hides the libc prototype */
uint32_t    ui__account_for_test(void);
bool        ui__wallet_info_pass_shown_for_test(void);
int         fake_protocol_device_passphrase_notices(void);
void        fake_protocol_reset(void);
const char *ui__mnemonic_buffer_for_test(void);
size_t      ui__mnemonic_buffer_size_for_test(void);
int         ui__mnemonic_word_count_for_test(void);
const char *ui__pin_entry_for_test(void);
int         ui__pin_cursor_for_test(void);
int         ui__pin_option_for_test(void);
const MnemonicEntry *ui__entry_for_test(void);
bool        ui__entry_choosing_length_for_test(void);
int         ui__entry_length_choice_for_test(void);
bool        ui__entry_is_temporary_for_test(void);
int         ui__lock_timeout_choice_for_test(void);
int         ui__lock_timeout_stored_choice_for_test(void);

void fake_input_reset(void);
static void settings_goto(const char *label);

/* The 12-word phrase fake_wallet hands out; a real BIP39 test vector. */
static const char *PHRASE_12 =
    "legal winner thank year wave sausage worth useful legal winner thank yellow";

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        printf("  FAIL: "); printf(__VA_ARGS__);     \
        printf("\n"); failures++;                    \
    }                                                \
} while (0)

/* Dump the screen when an assertion about it fails - a bare "expected X" tells
 * you nothing about what the device did instead. */
#define CHECK_SCREEN(cond, ...) do {                 \
    if (!(cond)) {                                   \
        printf("  FAIL: "); printf(__VA_ARGS__);     \
        printf("\n"); fake_oled_dump(); failures++;  \
    }                                                \
} while (0)

/* ------------------------------------------------------------------ driver */

/* One press, then a render - the same order ui_task uses. */
static void press(button_id_t btn)
{
    ui_handle_button(btn);
    ui_render();
}

static void go(screen_id_t screen)
{
    ui_set_screen(screen);
    ui_render();
}

/* One turn of ui_task's loop with nothing pressed: repaint ONLY if something
 * marked the screen dirty.
 *
 * Everything else in this file calls ui_render() unconditionally, which is
 * exactly why a missing ui_invalidate() could never fail a test here - and how
 * the "Signed" acknowledgement shipped never appearing on hardware. Anything
 * the protocol task changes while the screen is already up has to be checked
 * through this, not through press() or go(). */
static void idle_pump(void)
{
    if (ui_needs_render()) {
        ui_render();
    }
    /* Same order as ui_task(): the repaint first, then the work that was
     * deferred so it would happen behind a frame the user has already seen
     * (AUDIT S8f). A pump that ran them the other way round would let a test
     * pass against code that does the slow work inside the button handler. */
    ui_poll_deferred();
}

/* Power-on: empty flash, cleared RAM, registered screens. */
static void boot_device(void)
{
    fake_nvs_reset();
    fake_clock_reset();
    fake_oled_reset();
    fake_input_reset();
    fake_wallet_reset();
    pin__reset_static_state_for_test();
    ui__reset_static_state_for_test();

    pin_init();
    wallet_init();
    ui_init();
    ui_render();
}

/* A device that is past the PIN, with one seed stored. */
static void boot_unlocked_with_seed(void)
{
    boot_device();
    CHECK(pin_set("1234"), "setup: could not set a PIN");
    CHECK(pin_verify("1234"), "setup: could not unlock");
    CHECK(fake_wallet_preload(PHRASE_12) == 1, "setup: could not preload a seed");
}

/* ---------------------------------------------------- typing on 4 buttons */

/* Drive the selector onto option `to` through the real screen. A long option
 * list is shown as blocks, so reaching a letter means scrolling to its block,
 * pressing ACCEPT to open it, then scrolling inside - T44 added that level, and
 * this helper is where the extra press is spent. */
static void select_option(int to)
{
    const MnemonicEntry *e = ui__entry_for_test();

    if (mnemonic_entry_on_group(e)) {
        int want = to / e->group_size;
        for (int guard = 0; guard < 40 && e->option_index / e->group_size != want; guard++) {
            press(BUTTON_UP);
        }
        press(BUTTON_ACCEPT);   /* open the block */
    }
    for (int guard = 0; guard < 40 && e->option_index != to; guard++) {
        press(BUTTON_UP);
    }
}

/*
 * Type `target` on the seed-entry selector, the way a user would: pick the
 * letter, accept, repeat - and once the selector switches to whole words, pick
 * the word itself. Returns false if what is needed is not on offer, which means
 * the screen has stopped following the word.
 */
static bool type_word(const char *target)
{
    const MnemonicEntry *e = ui__entry_for_test();
    const int before = e->word_count;
    size_t next = 0;

    for (int guard = 0; guard < 400; guard++) {
        int found = -1;

        if (e->word_mode) {
            /* option_index is the read-only view of the selector, so probe the
             * candidates by walking it with real presses. */
            for (int i = 0; i < e->option_count; i++) {
                const char *w;
                select_option(i);
                w = mnemonic_entry_selected_word(e);
                if (w && strcmp(w, target) == 0) { found = i; break; }
            }
            if (found < 0) {
                return false;
            }
            select_option(found);
        } else {
            char want = (next < strlen(target)) ? target[next] : MNEMONIC_ENTRY_COMMIT;
            for (int i = 0; i < e->option_count; i++) {
                if (e->options[i] == want) { found = i; break; }
            }
            if (found < 0) {
                return false;
            }
            select_option(found);
            if (want != MNEMONIC_ENTRY_COMMIT) {
                next++;
            }
        }

        press(BUTTON_ACCEPT);

        if (e->word_count != before) {
            return e->word_count > before;
        }
    }
    return false;
}

/* Scroll the PIN selector to `option` (0-9, or 10 for OK) and press it. */
static bool pin_pick(int option)
{
    for (int guard = 0; guard < 24; guard++) {
        if (ui__pin_option_for_test() == option) {
            press(BUTTON_ACCEPT);
            return true;
        }
        press(BUTTON_UP);
    }
    return false;   /* the option is not in the cycle at all */
}

#define PIN_SUBMIT_OPTION 10

/* ============================================================================
 * T3 / AUDIT S8d - the import screen asks how many words
 * ============================================================================ */

static void test_import_word_count_prompt(void)
{
    printf("== import asks 12 or 24 before the first letter (T3)\n");
    boot_unlocked_with_seed();

    go(SCREEN_MNEMONIC_ENTRY);
    CHECK_SCREEN(fake_oled_contains("How many words?"), "no word-count question");
    CHECK_SCREEN(fake_oled_row_contains(4, "> 12"), "12 is not the default choice");
    CHECK_SCREEN(fake_oled_row_contains(5, "  24"), "24 is not offered");

    /* Two options, so either direction toggles. */
    press(BUTTON_DOWN);
    CHECK_SCREEN(fake_oled_row_contains(5, "> 24"), "DOWN did not move to 24");
    CHECK(ui__entry_length_choice_for_test() == 24, "choice is %d, expected 24",
          ui__entry_length_choice_for_test());

    press(BUTTON_ACCEPT);
    CHECK(!ui__entry_choosing_length_for_test(), "still on the length question");
    CHECK_SCREEN(fake_oled_row_contains(0, "Word 1/24"),
                 "header is \"%s\", expected Word 1/24", fake_oled_row(0));
}

static void test_import_back_from_first_character(void)
{
    printf("== BACK from the first letter returns to the prompt, not the menu\n");
    boot_unlocked_with_seed();

    go(SCREEN_MNEMONIC_ENTRY);
    press(BUTTON_DOWN);      /* choose 24 */
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_row_contains(0, "Word 1/24"), "setup: not on word 1");

    /* Nothing typed yet, so this is the press that used to drop the user out of
     * the import flow entirely - a wrong length choice cost a full restart. */
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_ENTRY,
          "BACK left the import screen (now on screen %d)", ui_get_screen());
    CHECK_SCREEN(fake_oled_contains("How many words?"), "did not return to the prompt");
    CHECK(ui__entry_length_choice_for_test() == 24,
          "the 24-word choice was forgotten (now %d)", ui__entry_length_choice_for_test());

    /* And the choice is still live: accepting again resumes a 24-word import. */
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_row_contains(0, "Word 1/24"), "resumed as \"%s\"",
                 fake_oled_row(0));
}

static void test_import_reaches_word_24(void)
{
    printf("== choosing 24 actually reaches word 24\n");
    boot_unlocked_with_seed();

    go(SCREEN_MNEMONIC_ENTRY);
    press(BUTTON_DOWN);
    press(BUTTON_ACCEPT);

    /* "zoo" is a BIP39 word that no other word extends, so it auto-commits and
     * keeps this loop short. The phrase is nonsense; reaching word 24 is the
     * claim, not importing it. */
    for (int i = 0; i < 23; i++) {
        if (!type_word("zoo")) {
            CHECK(false, "could not type word %d of 24", i + 1);
            return;
        }
    }

    CHECK_SCREEN(fake_oled_row_contains(0, "Word 24/24"),
                 "after 23 words the header reads \"%s\"", fake_oled_row(0));
    CHECK(ui__entry_for_test()->target_words == 24, "target is %d words",
          ui__entry_for_test()->target_words);
}

static void test_import_rejects_bad_checksum(void)
{
    printf("== a phrase that fails its checksum is refused\n");
    boot_unlocked_with_seed();

    go(SCREEN_MNEMONIC_ENTRY);
    press(BUTTON_ACCEPT);              /* keep 12 */

    for (int i = 0; i < 12; i++) {
        if (!type_word("zoo") && i < 11) {
            CHECK(false, "could not type word %d", i + 1);
            return;
        }
    }

    /* The 12th word completes the phrase and triggers the import attempt. */
    CHECK_SCREEN(fake_oled_contains("Bad checksum"),
                 "24 identical words were not rejected");
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_ENTRY,
          "a bad checksum navigated away (screen %d)", ui_get_screen());
    CHECK(wallet_get_status().wallet_count == 1,
          "an invalid phrase was stored (%u wallets)",
          wallet_get_status().wallet_count);
}

/* ============================================================================
 * AUDIT S5 / T6 - the seed does not linger in .bss
 *
 * The regression this exists to prevent is not "the buffer is never cleared".
 * It is the over-correction: an exit hook that zeroes unconditionally breaks
 * wallet creation, because the display and verification screens hand the same
 * buffer to each other in both directions.
 * ============================================================================ */

static bool mnemonic_buffer_is_zero(void)
{
    const char *buf = ui__mnemonic_buffer_for_test();
    size_t len = ui__mnemonic_buffer_size_for_test();
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    return true;
}

static void test_seed_survives_the_display_verify_handoff(void)
{
    printf("== the seed survives DISPLAY -> VERIFY (S5 over-correction guard)\n");
    boot_unlocked_with_seed();

    go(SCREEN_MNEMONIC_DISPLAY);
    CHECK(ui__mnemonic_word_count_for_test() == 12, "showing %d words",
          ui__mnemonic_word_count_for_test());
    CHECK_SCREEN(fake_oled_row_contains(2, "1. legal"),
                 "first word row reads \"%s\"", fake_oled_row(2));

    go(SCREEN_MNEMONIC_VERIFY);
    CHECK(!mnemonic_buffer_is_zero(), "the handoff to verification cleared the seed");
    CHECK(strcmp(ui__mnemonic_buffer_for_test(), PHRASE_12) == 0,
          "the seed changed across the handoff: \"%s\"", ui__mnemonic_buffer_for_test());

    /* Behavioural form of the same claim: verification needs the buffer to know
     * what the right answer is, and bails to the wallet screen without it. */
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_VERIFY,
          "verification gave up immediately (screen %d)", ui_get_screen());
    CHECK_SCREEN(fake_oled_row_contains(0, "Verify 1/3"), "header reads \"%s\"",
                 fake_oled_row(0));
}

static void test_seed_survives_the_user_path_into_verification(void)
{
    printf("== pressing through the last page reaches verification with the seed\n");
    boot_unlocked_with_seed();

    go(SCREEN_MNEMONIC_DISPLAY);
    /* 12 words at 3 per page: three presses to the last page, one more to move on. */
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_row_contains(2, "10. winner"),
                 "last page reads \"%s\"", fake_oled_row(2));

    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_VERIFY,
          "the last page did not lead to verification (screen %d)", ui_get_screen());

    /* And back again: CANCEL with nothing typed returns to the phrase, which is
     * only useful if the phrase is still there to read. */
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_DISPLAY,
          "CANCEL did not return to the seed (screen %d)", ui_get_screen());
    CHECK_SCREEN(fake_oled_row_contains(2, "1. legal"),
                 "returned to a blank phrase: \"%s\"", fake_oled_row(2));
}

static void test_seed_is_zeroed_on_leaving_the_flow(void)
{
    printf("== leaving the seed flow zeroes the buffer (S5)\n");

    /* From the display screen. */
    boot_unlocked_with_seed();
    go(SCREEN_MNEMONIC_DISPLAY);
    CHECK(!mnemonic_buffer_is_zero(), "setup: nothing was displayed");
    go(SCREEN_MAIN_MENU);
    CHECK(mnemonic_buffer_is_zero(), "the seed outlived the display screen");
    CHECK(ui__mnemonic_word_count_for_test() == 0, "the word count outlived it too");

    /* And from the verification screen, which is where wallet creation ends.
     * Without this the first check above would still pass with a hook missing
     * on the verify screen entirely. */
    boot_unlocked_with_seed();
    go(SCREEN_MNEMONIC_DISPLAY);
    go(SCREEN_MNEMONIC_VERIFY);
    CHECK(!mnemonic_buffer_is_zero(), "setup: the seed was already gone");
    go(SCREEN_WALLET_INFO);
    CHECK(mnemonic_buffer_is_zero(), "the seed outlived the verification screen");
}

/* ============================================================================
 * PIN entry
 * ============================================================================ */

static void test_pin_explicit_submit(void)
{
    printf("== the PIN selector submits only when OK is chosen\n");
    boot_device();
    CHECK(pin_set("54321"), "setup: could not set a PIN");
    pin_lock();

    go(SCREEN_PIN_UNLOCK);
    CHECK_SCREEN(fake_oled_row_contains(0, "Enter PIN"), "header reads \"%s\"",
                 fake_oled_row(0));
    CHECK_SCREEN(fake_oled_row_contains(4, "[________]"), "PIN field reads \"%s\"",
                 fake_oled_row(4));

    /* Below the minimum length there is no OK in the cycle - offering it would
     * mean offering a submit that can only fail. */
    CHECK_SCREEN(!fake_oled_row_contains(2, "OK"),
                 "OK is offered with no digits typed: \"%s\"", fake_oled_row(2));

    for (int i = 0; i < 4; i++) {
        CHECK(pin_pick(5 - i), "could not pick digit %d", 5 - i);
    }
    CHECK(ui__pin_cursor_for_test() == 4, "typed %d digits, expected 4",
          ui__pin_cursor_for_test());

    /* Four ACCEPTs and still here: the fourth digit is a digit, not a submit.
     * This is the bug the explicit OK option was added to fix - without it no
     * PIN longer than the minimum could be entered at all. */
    CHECK(ui_get_screen() == SCREEN_PIN_UNLOCK,
          "the device submitted on the minimum length (screen %d)", ui_get_screen());
    CHECK_SCREEN(fake_oled_row_contains(4, "[****____]"), "PIN field reads \"%s\"",
                 fake_oled_row(4));
    CHECK_SCREEN(fake_oled_contains("pick OK"), "no hint that OK is now reachable");

    /* A fifth digit, then submit. Reaching five proves the selector really did
     * keep appending rather than submitting. */
    CHECK(pin_pick(1), "could not pick the fifth digit");
    CHECK(ui__pin_cursor_for_test() == 5, "typed %d digits, expected 5",
          ui__pin_cursor_for_test());

    CHECK(pin_pick(PIN_SUBMIT_OPTION), "OK is not reachable from the selector");
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU,
          "a correct PIN did not unlock (screen %d)", ui_get_screen());
    CHECK(pin_is_unlocked(), "unlocked screen reached while still locked");
}

static void test_pin_wrong_spends_an_attempt(void)
{
    printf("== a wrong PIN costs an attempt and clears the field\n");
    boot_device();
    CHECK(pin_set("1234"), "setup: could not set a PIN");
    pin_lock();

    go(SCREEN_PIN_UNLOCK);
    for (int i = 0; i < 4; i++) {
        CHECK(pin_pick(9), "could not type digit %d", i + 1);
    }
    CHECK(pin_pick(PIN_SUBMIT_OPTION), "OK unreachable");

    CHECK(ui_get_screen() == SCREEN_PIN_UNLOCK, "a wrong PIN let the user in");
    CHECK(pin_get_remaining_attempts() == 2, "%d attempts left, expected 2",
          pin_get_remaining_attempts());
    CHECK(ui__pin_cursor_for_test() == 0, "the rejected digits were left on screen");
    CHECK_SCREEN(fake_oled_row_contains(4, "[________]"), "PIN field reads \"%s\"",
                 fake_oled_row(4));
    CHECK_SCREEN(fake_oled_row_contains(5, "Tries: 2"), "attempts row reads \"%s\"",
                 fake_oled_row(5));
}

static void test_pin_digits_do_not_outlive_the_screen(void)
{
    printf("== leaving the PIN screen zeroes the typed digits (S5)\n");
    boot_device();
    CHECK(pin_set("1234"), "setup: could not set a PIN");
    pin_lock();

    go(SCREEN_PIN_UNLOCK);
    for (int i = 0; i < 4; i++) {
        CHECK(pin_pick(7), "could not type digit %d", i + 1);
    }
    CHECK(strcmp(ui__pin_entry_for_test(), "7777") == 0,
          "setup: the field holds \"%s\"", ui__pin_entry_for_test());

    go(SCREEN_BOOT);

    /* Every byte, not just the terminator: a shortened string still leaves the
     * tail of the previous PIN in RAM. */
    const char *entry = ui__pin_entry_for_test();
    bool clean = true;
    for (int i = 0; i <= PIN_MAX_LENGTH; i++) {
        if (entry[i] != 0) {
            clean = false;
        }
    }
    CHECK(clean, "the typed PIN survived the screen change");
    CHECK(ui__pin_cursor_for_test() == 0, "cursor left at %d",
          ui__pin_cursor_for_test());
}

/* ============================================================================
 * Harness self-check
 *
 * A screen assertion that cannot fail is worse than no assertion, so confirm
 * the framebuffer really is being written and cleared between frames.
 * ============================================================================ */

static void test_harness_sees_the_screen(void)
{
    printf("== the framebuffer tracks what was drawn\n");
    boot_device();

    CHECK_SCREEN(fake_oled_contains("LeekWallet"), "the boot splash is not on screen");
    CHECK(fake_oled_find_row("LeekWallet") == 1, "splash is on row %d, expected 1",
          fake_oled_find_row("LeekWallet"));
    CHECK(fake_oled_flush_count() > 0, "nothing was ever pushed to the panel");

    /* Each render starts from a cleared frame; stale text must not survive. */
    go(SCREEN_MAIN_MENU);
    CHECK_SCREEN(!fake_oled_contains("LeekWallet"),
                 "the previous frame bled into this one");
}


/* ============================================================================
 * An error is not an identifier (AUDIT S8a, T7)
 * ============================================================================ */

/* The failure this guards against is subtle by design: an error string drawn
 * where an identifier goes reads as a truncated identifier, not as a failure.
 * The wallet screen keeps the two apart, so a derivation that fails says so in
 * words and draws nothing fingerprint-shaped. */

static void test_failed_derivation_is_not_shown_as_an_identifier(void)
{
    printf("== a failed derivation says Error, not something identifier-shaped (S8a)\n");
    boot_unlocked_with_seed();

    fake_wallet_fail_derivation(true);
    go(SCREEN_WALLET_INFO);

    CHECK_SCREEN(fake_oled_contains("Error"), "the screen does not say anything failed");
    CHECK_SCREEN(fake_oled_contains("No fingerprint"), "the reason is missing");
    CHECK_SCREEN(!fake_oled_contains("XFP"), "something fingerprint-shaped was drawn anyway");
    CHECK_SCREEN(!fake_oled_contains("coming soon"),
                 "the failure screen looks like the working one");
    fake_wallet_fail_derivation(false);
}

static void test_passphrase_confirmation_needs_a_fingerprint(void)
{
    printf("== passphrase confirmation refuses to confirm nothing (S8a)\n");
    boot_unlocked_with_seed();

    /* This screen asks the user to recognise a fingerprint. With none there
     * is nothing to recognise, and a blank row under "Match your record" is
     * an invitation to accept a wallet that never derived. */
    fake_wallet_fail_derivation(true);
    go(SCREEN_PASSPHRASE_CONFIRM);

    CHECK_SCREEN(!fake_oled_contains("Match your record"),
                 "the user was asked to match a record against nothing");
    CHECK_SCREEN(fake_oled_contains("Error"), "the failure is not stated");
    fake_wallet_fail_derivation(false);
}

/* ============================================================================
 * Roadmap 1.1 - the old chain screens are gone, and nothing leads nowhere
 * ============================================================================ */

/* Roadmap 1.5: the open seed is named by its Zcash fingerprint as soon as the
 * device is unlocked, beside the BIP32 one on the wallet screen, and a
 * passphrase changes it. */
static void test_the_seed_fingerprint_is_shown(void)
{
    printf("== the home and wallet screens name the seed by its fingerprint\n");
    boot_unlocked_with_seed();

    go(SCREEN_MAIN_MENU);
    CHECK_SCREEN(fake_oled_row_contains(0, "ZFP "),
                 "no fingerprint on the home screen after unlock (\"%s\")", fake_oled_row(0));
    char before[24];
    snprintf(before, sizeof before, "%s", fake_oled_row(0));
    CHECK_SCREEN(fake_oled_contains("Wallet Info"),
                 "the main menu does not offer Wallet Info");
    CHECK_SCREEN(!fake_oled_contains("View Address"),
                 "the main menu still offers an address view");

    go(SCREEN_WALLET_INFO);
    CHECK_SCREEN(fake_oled_contains("XFP"), "no BIP32 fingerprint on the wallet screen");
    CHECK_SCREEN(fake_oled_contains("ZFP "), "no Zcash fingerprint on the wallet screen");
    CHECK_SCREEN(strstr(before, ui__master_zfp_for_test()) != NULL,
                 "home and wallet screens disagree");
    CHECK_SCREEN(!fake_oled_contains("coming soon"), "a placeholder is still drawn");
    CHECK_SCREEN(fake_oled_row_contains(0, "W1/1"),
                 "the title does not name the wallet (\"%s\")", fake_oled_row(0));

    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "BACK did not leave the wallet screen");

    /* A passphrase is a different wallet, and the header has to say so. */
    CHECK(wallet_set_passphrase("TREZOR", 6) == WALLET_OK, "setup: passphrase");
    go(SCREEN_MAIN_MENU);
    CHECK_SCREEN(fake_oled_row_contains(0, "ZFP ") && strcmp(before, fake_oled_row(0)) != 0,
                 "the passphrase did not change the fingerprint (\"%s\")", fake_oled_row(0));
    wallet_clear_passphrase();
    /* That a lock forgets it is check_fully_locked()'s, on every lock path. */
}

static bool select_menu_item(const char *label)
{
    for (int i = 0; i < 12; i++) {
        char want[24];
        snprintf(want, sizeof want, "> %s", label);
        if (fake_oled_contains(want)) {
            return true;
        }
        press(BUTTON_DOWN);
    }
    return false;
}

/* Roadmap 1.6: Receive pages the whole UA, then its QR, then the t-address
 * (called public) and its QR; OK flips between an address and its QR; the
 * network comes from Settings. fake_zcash.c answers with the librustzcash
 * vector, so what is checked is that the screen shows that address whole. */
static void walk_receive(const zv_key_vector *v)
{
    char ua[ZS_UA_MAX] = {0};
    int page = 0;
    /* Text pages until the first QR. */
    while (fake_oled_row_contains(0, "UA ") && page < 8) {
        CHECK_SCREEN(fake_oled_row_contains(0, "UA "), "UA page %d title", page);
        CHECK_SCREEN(fake_oled_row_contains(0, v->network ? "TEST" : "MAIN"),
                     "network not named (\"%s\")", fake_oled_row(0));
        for (int r = 1; r <= 6; r++) strcat(ua, fake_oled_row(r));
        press(BUTTON_DOWN);
        page++;
    }
    CHECK(strcmp(ua, v->ua) == 0, "the UA does not reassemble:\n  %s\n  %s", ua, v->ua);
    CHECK(strcmp(fake_oled_qr_data(), v->ua) == 0, "the UA QR carries \"%s\"",
          fake_oled_qr_data());

    press(BUTTON_ACCEPT);
    CHECK(ui__receive_page_for_test() == 0, "OK on the QR did not return to the text");
    press(BUTTON_ACCEPT);
    CHECK(strcmp(fake_oled_qr_data(), v->ua) == 0, "OK on the text did not show its QR");

    press(BUTTON_DOWN);
    CHECK_SCREEN(fake_oled_contains("T-addr") && fake_oled_contains("PUBLIC"),
                 "the t-address page does not warn");
    char t[64];
    snprintf(t, sizeof t, "%s%s", fake_oled_row(2), fake_oled_row(3));
    CHECK(strcmp(t, v->transparent_address) == 0, "t-address \"%s\"", t);
    press(BUTTON_ACCEPT);
    CHECK(strcmp(fake_oled_qr_data(), v->transparent_address) == 0, "t-address QR");
    press(BUTTON_DOWN);
    CHECK(strcmp(fake_oled_qr_data(), v->transparent_address) == 0, "DOWN ran off the end");
}

static void test_receive_shows_the_whole_address(void)
{
    printf("== Receive shows the whole UA, the t-address, and both as QR\n");
    boot_unlocked_with_seed();
    go(SCREEN_MAIN_MENU);

    CHECK(select_menu_item("Receive"), "Receive is not on the main menu");
    /* No camera on the host board, and Scan must not pretend otherwise. */
    go(SCREEN_MAIN_MENU);
    CHECK(!select_menu_item("Scan"), "Scan is offered on a board with no camera");

    go(SCREEN_MAIN_MENU);
    select_menu_item("Receive");
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_SHOW_ADDRESS, "the menu did not open it");
    walk_receive(&ZV_KEYS[0]);   /* main/abandon-art/acct0 */

    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "BACK did not return to the menu");

    /* Settings > Network: testnet, persisted, and Receive follows. */
    go(SCREEN_SETTINGS);
    CHECK(select_menu_item("Net Mainnet"), "Network is not in Settings");
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_contains("> Net Testnet"), "the setting did not switch");
    CHECK(ui_zcash_network() == 1, "ui_zcash_network() did not follow");
    go(SCREEN_SHOW_ADDRESS);
    walk_receive(&ZV_KEYS[1]);   /* test/abandon-art/acct0 */
    press(BUTTON_CANCEL);

    ui__reset_static_state_for_test();
    ui_init();
    CHECK(ui_zcash_network() == 1, "the network setting was not persisted");
    go(SCREEN_SETTINGS);
    select_menu_item("Net Testnet");
    press(BUTTON_ACCEPT);
    CHECK(ui_zcash_network() == 0, "could not switch back");
}

/* zShowAddress (roadmap 1.7): the whole UA, then the question; YES only on
 * the question page, NO from anywhere. */
static void test_address_check(void)
{
    printf("== zShowAddress: the device pages the UA and asks whether it matches\n");
    boot_unlocked_with_seed();
    const zv_key_vector *v = &ZV_KEYS[1];

    ui_request_show_address(v->ua, v->transparent_address, 0, ZS_NET_TEST);
    go(SCREEN_ADDRESS_CHECK);
    char ua[ZS_UA_MAX] = {0};
    int pages = 0;
    while (!fake_oled_contains("Is this the address") && pages < 8) {
        CHECK_SCREEN(fake_oled_row_contains(0, "UA ") && fake_oled_row_contains(0, "TEST"),
                     "page %d title", pages);
        for (int r = 1; r <= 6; r++) strcat(ua, fake_oled_row(r));
        CHECK(ui_sign_outcome() == SIGN_PENDING, "answered before the question page");
        press(BUTTON_ACCEPT);   /* NEXT */
        pages++;
    }
    CHECK(strcmp(ua, v->ua) == 0, "the UA does not reassemble:\n  %s\n  %s", ua, v->ua);
    CHECK(pages == 2, "%d address pages", pages);
    press(BUTTON_ACCEPT);
    CHECK(ui_sign_outcome() == SIGN_APPROVED, "YES on the question page did not approve");
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "did not go home after the answer");
    ui_sign_clear();

    ui_request_show_address(v->ua, v->transparent_address, 0, ZS_NET_TEST);
    go(SCREEN_ADDRESS_CHECK);
    press(BUTTON_DOWN);
    press(BUTTON_CANCEL);
    CHECK(ui_sign_outcome() == SIGN_REJECTED, "NO did not reject");
    ui_sign_clear();

    /* The host gave up: the question comes off the screen. */
    ui_request_show_address(v->ua, v->transparent_address, 0, ZS_NET_TEST);
    go(SCREEN_ADDRESS_CHECK);
    ui_sign_expire();
    ui__service_sign_expiry_for_test();
    CHECK(ui_get_screen() != SCREEN_ADDRESS_CHECK, "an expired question stayed up");
}

/* A companion asked for the other network and was refused (protocol.c): the
 * board says so over whatever is on screen, for a few seconds or until a
 * press, and that press does nothing else. */
static void test_wrong_network_notice(void)
{
    printf("== a refused network is said on the board's screen\n");
    boot_unlocked_with_seed();
    go(SCREEN_SETTINGS);
    CHECK(select_menu_item("Net Mainnet"), "Network is not in Settings");
    press(BUTTON_ACCEPT);
    CHECK(ui_zcash_network() == 1, "not set to testnet");
    go(SCREEN_MAIN_MENU);

    ui_notify_wrong_network(0);   /* a mainnet app, from the protocol task */
    ui__service_wrong_network_for_test();
    idle_pump();
    CHECK_SCREEN(fake_oled_contains("Wrong network") && fake_oled_contains("App: Mainnet") &&
                 fake_oled_contains("Board: Testnet") && fake_oled_contains("Settings > Network"),
                 "the refusal was not shown");

    /* A press dismisses it and does nothing else. */
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "the press reached the screen under the notice");
    CHECK_SCREEN(!fake_oled_contains("Wrong network"), "the notice stayed after a press");

    /* Or it goes by itself. */
    ui_notify_wrong_network(0);
    ui__service_wrong_network_for_test();
    idle_pump();
    CHECK_SCREEN(fake_oled_contains("Wrong network"), "the second refusal was not shown");
    fake_clock_advance_us(3 * 1000 * 1000);
    ui__service_wrong_network_for_test();
    idle_pump();
    CHECK_SCREEN(fake_oled_contains("Wrong network"), "the notice went too soon");
    fake_clock_advance_us(2 * 1000 * 1000);
    ui__service_wrong_network_for_test();
    idle_pump();
    CHECK_SCREEN(!fake_oled_contains("Wrong network") && ui_get_screen() == SCREEN_MAIN_MENU,
                 "the notice did not go, or took the screen with it");

    go(SCREEN_SETTINGS);
    select_menu_item("Net Testnet");
    press(BUTTON_ACCEPT);
    CHECK(ui_zcash_network() == 0, "could not switch back");
}

/* ============================================================================
 * Roadmap 1.4 - SD card storage mode, through the real buttons
 * ============================================================================ */

/* Type `s` into the SD screen's text entry with UP and ACCEPT only, switching
 * character sets through the ring's own mode entries, then pick OK. */
static bool sd_type(const char *s)
{
    TextEntry *e = ui__sd_entry_for_test();
    if (!e) return false;
    for (const char *p = s; ; p++) {
        char want = *p ? *p : TEXT_ENTRY_OK;
        bool ok = false;
        for (int tries = 0; tries < 400 && !ok; tries++) {
            char opt = text_entry_option(e);
            if (opt == want) {
                press(BUTTON_ACCEPT);
                ok = true;
            } else if (opt >= TEXT_ENTRY_MODE_ABC && opt <= TEXT_ENTRY_MODE_NUM && want >= ' ' &&
                       ((opt == TEXT_ENTRY_MODE_ABC && islower((unsigned char)want)) ||
                        (opt == TEXT_ENTRY_MODE_CAPS && isupper((unsigned char)want)) ||
                        (opt == TEXT_ENTRY_MODE_NUM && !isalpha((unsigned char)want)))) {
                press(BUTTON_ACCEPT);   /* switch set, then look again */
            } else {
                press(BUTTON_UP);
            }
        }
        if (!ok) return false;
        if (!*p) return true;
    }
}

static int zfp_from_header(char out[9])
{
    const char *h = strstr(fake_oled_row(0), "ZFP ");
    if (!h) return 0;
    snprintf(out, 9, "%s", h + 4);
    return 1;
}

static void open_sd_screen(void)
{
    go(SCREEN_MAIN_MENU);
    CHECK(select_menu_item("SD Card"), "SD Card is not on the main menu");
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_SD_SEED, "the menu did not open the SD screen");
}

static void test_sd_card_save_and_load(void)
{
    printf("== SD card: save the open seed, load it back as a session seed\n");
    char card[64] = "/tmp/leek-ui-card-XXXXXX";
    CHECK(mkdtemp(card) != NULL, "setup: no card directory");

    boot_unlocked_with_seed();
    sdcard__set_root_for_test(NULL);
    open_sd_screen();
    CHECK_SCREEN(fake_oled_contains("No SD card"), "a missing card is not said");

    sdcard__set_root_for_test(card);
    press(BUTTON_ACCEPT);   /* RETRY */
    CHECK_SCREEN(fake_oled_contains("> Save open seed"), "nothing offers to save");

    /* Save: a new passphrase twice. */
    go(SCREEN_MAIN_MENU);
    char zfp_stored[9] = {0};
    CHECK(zfp_from_header(zfp_stored), "no ZFP on the home header");
    open_sd_screen();
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_contains("New card passphrase"), "save did not ask for a passphrase");
    CHECK(sd_type("Card pass 1"), "could not type the passphrase");
    CHECK_SCREEN(fake_oled_contains("Repeat passphrase"), "the passphrase was not asked twice");
    CHECK(sd_type("Card pass 2"), "could not type the repeat");
    CHECK_SCREEN(fake_oled_contains("Passphrases differ"), "a mismatch was not caught");
    press(BUTTON_ACCEPT);

    press(BUTTON_ACCEPT);   /* Save open seed, still the only item */
    sd_type("short");
    CHECK_SCREEN(fake_oled_contains("8-64 characters"), "a short passphrase was not refused");
    press(BUTTON_ACCEPT);

    press(BUTTON_ACCEPT);
    sd_type("Card pass 1");
    sd_type("Card pass 1");
    CHECK_SCREEN(fake_oled_contains("Saved SEED1.ZKV"), "not saved");
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_contains("> SEED1.ZKV"), "the new file is not listed");
    CHECK(ui__sd_entry_for_test()->length == 0, "the passphrase outlived the save");

    /* A device with nothing stored loads it. */
    boot_device();
    CHECK(pin_set("1234") && pin_verify("1234"), "setup: PIN");
    sdcard__set_root_for_test(card);
    open_sd_screen();
    CHECK_SCREEN(fake_oled_contains("> SEED1.ZKV") && !fake_oled_contains("Save open seed"),
                 "list on an empty device");
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_contains("Card passphrase"), "load did not ask for the passphrase");
    sd_type("Card pass 9");
    CHECK_SCREEN(fake_oled_contains("Wrong passphrase"), "a wrong passphrase was not said");
    CHECK(!wallet_has_temporary_mnemonic(), "a wrong passphrase loaded something");
    press(BUTTON_ACCEPT);

    press(BUTTON_ACCEPT);
    sd_type("Card pass 1");
    CHECK_SCREEN(fake_oled_contains("Loaded SEED1.ZKV"), "not loaded");
    CHECK_SCREEN(fake_oled_contains("Gone on lock/reboot"), "the session-only nature is not said");
    CHECK(wallet_has_temporary_mnemonic(), "an SD seed is a session seed");
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "OK did not go home");
    char zfp_loaded[9] = {0};
    CHECK_SCREEN(fake_oled_row_contains(0, "TEMP") && zfp_from_header(zfp_loaded),
                 "the home header does not name the loaded seed (\"%s\")", fake_oled_row(0));
    CHECK(strcmp(zfp_loaded, zfp_stored) == 0, "loaded %s, saved %s", zfp_loaded, zfp_stored);

    sdcard__set_root_for_test(NULL);
    char p[128];
    snprintf(p, sizeof p, "%s/SEED1.ZKV", card);
    unlink(p);
    rmdir(card);
}

/* Roadmap 5: a PCZT on the card is reviewed on the same pages as over USB,
 * and ACCEPT writes LEEKZ/signed.zsig; BACK writes nothing. */
#include "../../vectors/pczt_vectors.h"

static void put_card_file(const char *card, const char *rel, const char *src)
{
    char p[256];
    snprintf(p, sizeof p, "%s/LEEKZ", card);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s", card, rel);
    FILE *in = fopen(src, "rb"), *out = fopen(p, "wb");
    uint8_t b[4096];
    size_t n;
    while (in && out && (n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);
}

static bool card_has_file(const char *card, const char *rel)
{
    char p[256];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", card, rel);
    return stat(p, &st) == 0;
}

static void test_sd_card_signing(void)
{
    printf("== SD card: a transaction on the card is reviewed and signed to signed.zsig\n");
    const zv_pczt_vector *v = &ZV_PCZT[0];   /* iw_send_shielded, testnet */
    char card[64] = "/tmp/leek-ui-sign-XXXXXX";
    CHECK(mkdtemp(card) != NULL, "setup: card");

    boot_device();
    CHECK(pin_set("1234") && pin_verify("1234"), "setup: PIN");
    CHECK(fake_wallet_preload(v->mnemonic) == 1, "setup: seed");
    go(SCREEN_SETTINGS);
    select_menu_item("Net Mainnet");
    press(BUTTON_ACCEPT);   /* testnet: the vector's network */
    sdcard__set_root_for_test(card);

    /* No transaction on the card: not offered. */
    open_sd_screen();
    CHECK_SCREEN(!fake_oled_contains("Sign transaction"), "offered with nothing to sign");

    put_card_file(card, "LEEKZ/unsigned.pczt", v->file);
    open_sd_screen();
    CHECK_SCREEN(fake_oled_contains("> Sign transaction"), "the transaction is not offered first");
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_ZSIGN_REVIEW, "did not open the review (screen %d)", ui_get_screen());
    for (int i = 0; i < 40 && ui_get_screen() == SCREEN_ZSIGN_REVIEW; i++) {
        CHECK(!card_has_file(card, "LEEKZ/signed.zsig"), "signed before the last page");
        press(BUTTON_ACCEPT);
    }
    CHECK(ui_get_screen() == SCREEN_SD_SEED, "did not come back to the SD screen");
    CHECK_SCREEN(fake_oled_contains("Signed: 1 signature"), "the result is not said");
    CHECK(card_has_file(card, "LEEKZ/signed.zsig"), "no signed.zsig on the card");
    CHECK(ui_sign_outcome() == SIGN_PENDING, "a card signature answered a host");

    /* BACK on the review: nothing written. */
    char p[256];
    snprintf(p, sizeof p, "%s/LEEKZ/signed.zsig", card);
    unlink(p);
    press(BUTTON_ACCEPT);   /* result -> list */
    press(BUTTON_ACCEPT);   /* Sign transaction */
    CHECK(ui_get_screen() == SCREEN_ZSIGN_REVIEW, "second review did not open");
    press(BUTTON_DOWN);
    press(BUTTON_CANCEL);
    CHECK_SCREEN(fake_oled_contains("Rejected"), "a rejection is not said");
    CHECK(!card_has_file(card, "LEEKZ/signed.zsig"), "a rejected transaction was signed");

    sdcard__set_root_for_test(NULL);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", card);
    CHECK(system(cmd) == 0, "cleanup");
}

/* QR air gap: a ur:leek-pczt animated by the companion, scanned part by part,
 * reviewed on the usual pages; ACCEPT shows ur:leek-zsig. */
#include "ur-encoder.h"

static void test_qr_airgap_signing(void)
{
    printf("== QR: a scanned ur:leek-pczt is reviewed and answered with ur:leek-zsig\n");
    const zv_pczt_vector *v = &ZV_PCZT[0];
    boot_device();
    CHECK(pin_set("1234") && pin_verify("1234"), "setup: PIN");
    CHECK(fake_wallet_preload(v->mnemonic) == 1, "setup: seed");
    go(SCREEN_SETTINGS);
    select_menu_item("Net Mainnet");
    press(BUTTON_ACCEPT);

    FILE *f = fopen(v->file, "rb");
    static uint8_t cbor[8192];
    size_t n = f ? fread(cbor + 3, 1, sizeof cbor - 3, f) : 0;
    if (f) fclose(f);
    cbor[0] = 0x59;   /* CBOR byte string, 2-byte length */
    cbor[1] = (uint8_t)(n >> 8);
    cbor[2] = (uint8_t)n;
    static UrEncoder enc;
    CHECK(ur_encoder_init(&enc, "leek-pczt", cbor, n + 3, 200), "encoder");
    go(SCREEN_SCAN);
    char part[600];
    for (int i = 0; i < 400 && ui_get_screen() == SCREEN_SCAN; i++) {
        size_t pl = ur_encoder_next_part(&enc, part, sizeof part);
        ui__scan_feed_for_test(part);
        (void)pl;
    }
    CHECK(ui_get_screen() == SCREEN_ZSIGN_REVIEW, "the scanned PCZT did not reach the review (%d)", ui_get_screen());
    for (int i = 0; i < 40 && ui_get_screen() == SCREEN_ZSIGN_REVIEW; i++) press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_QR_OUT, "no signature QR (%d)", ui_get_screen());
    CHECK(strncmp(fake_oled_qr_data(), "UR:LEEK-ZSIG/", 13) == 0, "QR shows \"%.20s\"", fake_oled_qr_data());
    CHECK(ui_sign_outcome() == SIGN_PENDING, "a QR signature answered a host");
}

/* ============================================================================
 * The Wi-Fi test fixture is gone from release builds (AUDIT S8g, T17)
 * ============================================================================ */

static void test_settings_has_no_wifi_entry(void)
{
    printf("== settings offers no Wi-Fi AP in a build without it (S8g)\n");
    boot_unlocked_with_seed();
    go(SCREEN_SETTINGS);

    /* The host build defines no CONFIG_ESP_WIFI_ENABLED, so this exercises
     * exactly what ships. Walk the whole menu rather than reading one page:
     * the entry used to sit below the fold. */
    bool seen_back = false;
    for (int i = 0; i < 40 && !seen_back; i++) {
        CHECK_SCREEN(!fake_oled_contains("WiFi"), "the Wi-Fi entry is still offered");
        seen_back = fake_oled_contains("Back");
        press(BUTTON_DOWN);
    }
    CHECK(seen_back, "never reached the end of the settings menu");
}


/* ============================================================================
 * T57 - the transport is chosen on the device, and only one is ever live
 * ============================================================================ */

static void test_settings_selects_one_transport(void)
{
    printf("== settings picks USB or BLE, and never both (T57)\n");
    boot_unlocked_with_seed();
    go(SCREEN_SETTINGS);

    transport_init();
    CHECK(transport_get() == TRANSPORT_USB, "the device did not default to USB");
    CHECK(!ble_transport_running(), "BLE was advertising before it was chosen");

    /* Reach the entry the way a user does. There is one item, not a pair of
     * radio toggles: a screen offering "BLE [ON]" alongside a live USB link
     * would be offering a state the device must never be in. */
    bool found = false;
    for (int i = 0; i < 40 && !found; i++) {
        found = fake_oled_contains("> Link USB");
        if (!found) press(BUTTON_DOWN);
    }
    CHECK(found, "no transport entry in the settings menu");
    if (!found) return;

    press(BUTTON_ACCEPT);
    CHECK(transport_get() == TRANSPORT_BLE, "accepting did not switch to BLE");
    CHECK(ble_transport_running(), "BLE was selected but is not up");
    CHECK(!fake_protocol_rx_enabled(), "the USB endpoint stayed live under BLE");
    CHECK_SCREEN(fake_oled_contains("Link BLE"), "the screen still claims USB");

    press(BUTTON_ACCEPT);
    CHECK(transport_get() == TRANSPORT_USB, "accepting again did not return to USB");
    CHECK(!ble_transport_running(), "BLE kept advertising after USB was chosen");
    CHECK(fake_protocol_rx_enabled(), "the cable stayed deaf after being chosen");
}

static void test_advertising_window_reopens_on_a_press(void)
{
    printf("== a lapsed BLE advertising window re-opens on any button press\n");

    /* The radio stops advertising after two minutes with nobody connecting
     * (ble.c, BLE_ADV_WINDOW_MS) because it was the board's largest continuous
     * draw and the Pixie was uncomfortably warm to hold. The cost of that is a
     * device nobody can find, so the way back must be something a user does
     * without being told. This pins that it is any press, and that the press
     * still does its normal job. */
    boot_unlocked_with_seed();
    go(SCREEN_SETTINGS);
    transport_init();

    bool found = false;
    for (int i = 0; i < 40 && !found; i++) {
        found = fake_oled_contains("> Link USB");
        if (!found) press(BUTTON_DOWN);
    }
    CHECK(found, "no transport entry in the settings menu");
    if (!found) return;
    press(BUTTON_ACCEPT);
    CHECK(ble_transport_running(), "BLE was selected but is not up");
    CHECK(ble_transport_advertising(), "BLE came up without advertising");

    /* Only the controller's timer can close the window, so the stand-in is
     * closed by hand -- the behaviour under test is the re-opening. */
    ble_transport_stop();
    ble_transport_start();
    extern void ble_test_close_window(void);
    ble_test_close_window();
    CHECK(!ble_transport_advertising(), "the window did not close");

    press(BUTTON_DOWN);
    CHECK(ble_transport_advertising(), "a press did not re-open the window");

    /* And a press must never start a transport the user switched off. */
    ble_transport_stop();
    press(BUTTON_DOWN);
    CHECK(!ble_transport_running(), "a press started a stopped transport");
    CHECK(!ble_transport_advertising(), "a press advertised on a stopped transport");
}

/* ============================================================================
 * T61 - one job per button on the entropy screen
 * ============================================================================ */

/* Mirrors ENTROPY_TARGET_BITS / BITS_PER_EVENT in ui.c and entropy.c, which are
 * static. Written once here and formatted into the expectations below rather
 * than spelled into a dozen string literals: the press count moved from 32 to 64
 * when BITS_PER_EVENT was corrected to 2, and it should be able to move again
 * without this file needing a search-and-replace to notice. */
#define UI_ENTROPY_TARGET_BITS 128
#define UI_ENTROPY_TARGET      (UI_ENTROPY_TARGET_BITS / 2)

static void counter_text(char *out, size_t n, int events)
{
    snprintf(out, n, "%d / %d", events, UI_ENTROPY_TARGET);
}

/* The screen opens on a chooser now, because there are two ways to feed the
 * pool. Item 0 is dice, item 1 is taps. */
static void enter_entropy_mode(int item)
{
    go(SCREEN_ENTROPY);
    for (int i = 0; i < item; i++) press(BUTTON_DOWN);
    press(BUTTON_ACCEPT);
}

/* Walk the real creation flow up to the point of generation.
 *
 * The length is chosen first now, because the entropy gate depends on it (128
 * bits for 12 words, 256 for 24). Tests that used to open the create screen and
 * press ACCEPT have to come through here instead -- which is the point of the
 * change, and worth their going the long way for. */
static void reach_generation_ready(void)
{
    go(SCREEN_WALLET_CREATE);          /* the length chooser, 12 by default */
    press(BUTTON_ACCEPT);              /* -> entropy, target 128 bits */
    press(BUTTON_DOWN);                /* chooser: dice -> taps */
    press(BUTTON_ACCEPT);              /* -> taps mode */
    for (int i = 0; i < UI_ENTROPY_TARGET; i++) press(BUTTON_UP);
    press(BUTTON_ACCEPT);              /* full pool proceeds -> create, ready */
}

static void test_entropy_accept_only_proceeds(void)
{
    printf("== ACCEPT collects nothing and only ever proceeds (T61)\n");
    boot_unlocked_with_seed();
    enter_entropy_mode(1);

    char want[24];
    counter_text(want, sizeof(want), 0);
    CHECK_SCREEN(fake_oled_contains(want), "the pool did not start empty");
    CHECK_SCREEN(fake_oled_row_contains(7, "----"),
                 "the footer offers something for ACCEPT before the target");
    CHECK_SCREEN(!fake_oled_contains("NEXT"), "NEXT is offered before the target");

    /* ACCEPT before the target does nothing at all - not a sample, not a
     * screen change. It used to count as a sample, which taught the reflex
     * that creates a wallet. */
    for (int i = 0; i < 5; i++) {
        press(BUTTON_ACCEPT);
    }
    CHECK(ui_get_screen() == SCREEN_ENTROPY, "ACCEPT left the screen early");
    counter_text(want, sizeof(want), 0);
    CHECK_SCREEN(fake_oled_contains(want),
                 "ACCEPT was counted as a sample");

    /* UP and DOWN are the samples. */
    press(BUTTON_UP);
    counter_text(want, sizeof(want), 1);
    CHECK_SCREEN(fake_oled_contains(want), "UP did not collect a sample");
    press(BUTTON_DOWN);
    counter_text(want, sizeof(want), 2);
    CHECK_SCREEN(fake_oled_contains(want), "DOWN did not collect a sample");

    for (int i = 2; i < UI_ENTROPY_TARGET; i++) {
        press(BUTTON_UP);
    }
    counter_text(want, sizeof(want), UI_ENTROPY_TARGET);
    CHECK_SCREEN(fake_oled_contains(want), "the target did not fill the pool");
    CHECK_SCREEN(fake_oled_contains("Ready"), "a full pool does not say Ready");
    CHECK_SCREEN(fake_oled_row_contains(7, "NEXT"), "NEXT is still not offered");

    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_WALLET_CREATE,
          "ACCEPT on a full pool did not proceed");
}

static void test_entropy_cancel_abandons(void)
{
    printf("== CANCEL abandons wallet creation rather than lowering the bar\n");
    boot_unlocked_with_seed();
    enter_entropy_mode(1);

    for (int i = 0; i < 10; i++) {
        press(BUTTON_UP);
    }
    /* One step back to the chooser, keeping what was collected, then out. */
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_ENTROPY, "CANCEL skipped the chooser");
    CHECK_SCREEN(fake_oled_contains("taps 10"),
                 "backing out of tap mode discarded the pool");
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "CANCEL did not leave the screen");

    /* And the half-full pool does not survive to be topped up later. */
    enter_entropy_mode(1);
    char want[24];
    counter_text(want, sizeof(want), 0);
    CHECK_SCREEN(fake_oled_contains(want), "the abandoned pool was kept");
    press(BUTTON_CANCEL);
    press(BUTTON_CANCEL);
}

/* ============================================================================
 * Dice entropy
 *
 * The reason this mode exists is that its bit count is arithmetic rather than
 * modelled, so the tests are about the arithmetic being on screen and about
 * ROLL never turning into "create a wallet" under the user's thumb.
 * ============================================================================ */

/* log2(6) = 2.5849625, credited as floor(n * 2585 / 1000). */
static int dice_bits_for(int rolls)
{
    return (rolls * 2585) / 1000;
}

/* Commit `face` on the wrapping 1..6 selector, which starts (and stays) on
 * whatever was last armed. Walks up because that is all these tests need. */
static void roll(int from, int face)
{
    int steps = (face - from + 6) % 6;
    for (int i = 0; i < steps; i++) press(BUTTON_UP);
    press(BUTTON_ACCEPT);
}

static void test_dice_counts_are_arithmetic(void)
{
    printf("== the dice screen shows a defensible bit count\n");
    boot_unlocked_with_seed();
    enter_entropy_mode(0);

    CHECK_SCREEN(fake_oled_contains("REAL dice"),
                 "the screen does not say the dice must be physical");
    CHECK_SCREEN(fake_oled_contains("phone apps"),
                 "the screen does not warn against phone dice apps");
    CHECK_SCREEN(fake_oled_contains("roll 0/50"),
                 "50 rolls is the 128-bit target and the screen does not say so");
    CHECK_SCREEN(fake_oled_contains("[1]"), "the selector does not start on 1");

    /* The selector is a position, not a free-typed digit: it must show which
     * face is armed before ROLL commits it. */
    press(BUTTON_UP);
    CHECK_SCREEN(fake_oled_contains("[2]"), "UP did not move the selector");
    CHECK_SCREEN(fake_oled_contains("roll 0/50"),
                 "moving the selector was counted as a roll");
    press(BUTTON_DOWN);
    CHECK_SCREEN(fake_oled_contains("[1]"), "DOWN did not move the selector back");

    press(BUTTON_ACCEPT);
    char want[32];
    snprintf(want, sizeof(want), "roll 1/50  ~%d bits", dice_bits_for(1));
    CHECK_SCREEN(fake_oled_contains(want), "one roll is not credited %d bits",
                 dice_bits_for(1));
    CHECK_SCREEN(fake_oled_contains("entered: 1"),
                 "the committed roll is not echoed back");

    /* Ten rolls of assorted faces: 10 * 2.585 = 25 bits, floored. */
    int cur = 1;
    static const int faces[9] = {4, 4, 6, 2, 5, 1, 3, 6, 2};
    for (int i = 0; i < 9; i++) { roll(cur, faces[i]); cur = faces[i]; }
    snprintf(want, sizeof(want), "roll 10/50  ~%d bits", dice_bits_for(10));
    CHECK_SCREEN(fake_oled_contains(want), "10 rolls is not %d bits",
                 dice_bits_for(10));
    CHECK(dice_bits_for(10) == 25, "log2(6) arithmetic drifted: %d",
          dice_bits_for(10));
}

static void test_dice_roll_never_creates_a_wallet(void)
{
    printf("== ROLL only ever commits a roll; creating is elsewhere\n");
    boot_unlocked_with_seed();
    enter_entropy_mode(0);

    int cur = 1;
    for (int i = 0; i < 60; i++) { roll(cur, (i % 6) + 1); cur = (i % 6) + 1; }

    /* Past the target, and pressing the collect button dozens more times still
     * cannot generate a seed - the reflex button is not the commit button. */
    CHECK(ui_get_screen() == SCREEN_ENTROPY, "ROLL wandered off the screen");
    for (int i = 0; i < 5; i++) press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_ENTROPY, "ROLL created a wallet");

    press(BUTTON_CANCEL);
    char want[40];
    snprintf(want, sizeof(want), "dice 65 = %d bits", dice_bits_for(65));
    CHECK_SCREEN(fake_oled_contains(want), "the chooser lost the dice total");
    CHECK_SCREEN(fake_oled_contains("Create wallet"),
                 "a met target does not offer to create the wallet");

    press(BUTTON_DOWN);
    press(BUTTON_DOWN);
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_WALLET_CREATE,
          "the chooser would not proceed on a met target");
}

static void test_dice_and_taps_compose(void)
{
    printf("== dice and tap bits add up to one gate\n");
    boot_unlocked_with_seed();
    enter_entropy_mode(0);

    int cur = 1;
    for (int i = 0; i < 20; i++) { roll(cur, (i % 6) + 1); cur = (i % 6) + 1; }
    press(BUTTON_CANCEL);

    /* 20 rolls = 51 bits, so tap mode should now be asking for the remaining
     * 77 bits as ceil(77/2) = 39 presses on top of nothing - not 64. */
    int have = dice_bits_for(20);
    int taps = (UI_ENTROPY_TARGET_BITS - have + 1) / 2;
    press(BUTTON_DOWN);
    press(BUTTON_ACCEPT);
    char want[32];
    snprintf(want, sizeof(want), "0 / %d", taps);
    CHECK_SCREEN(fake_oled_contains(want),
                 "tap mode ignored the dice already rolled (wanted %s)", want);

    for (int i = 0; i < taps; i++) press(BUTTON_UP);
    CHECK_SCREEN(fake_oled_contains("Ready"),
                 "dice plus taps did not reach the gate");
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_WALLET_CREATE,
          "a combined pool would not proceed");
}

/* ============================================================================
 * T39b - the master fingerprint is on screen
 * ============================================================================ */

static void test_xfp_is_shown_on_the_wallet_screen(void)
{
    printf("== the wallet screen names the seed (T39b)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    go(SCREEN_WALLET_INFO);
    int row = fake_oled_find_row("XFP");
    CHECK_SCREEN(row >= 0, "no fingerprint on the wallet screen");

    uint32_t fp = 0;
    CHECK(wallet_get_master_fingerprint(&fp) == WALLET_OK, "no fingerprint to show");
    char expect[16];
    snprintf(expect, sizeof(expect), "XFP %08lX", (unsigned long)fp);
    CHECK_SCREEN(fake_oled_contains(expect), "the screen does not show %s", expect);

    /* It identifies the seed, so changing account must not change it. */
    settings_goto("Account");
    press(BUTTON_ACCEPT);
    go(SCREEN_WALLET_INFO);
    CHECK_SCREEN(fake_oled_contains(expect),
                 "the fingerprint changed when the account did");
}

static void test_xfp_tells_passphrase_wallets_apart(void)
{
    printf("== the passphrase confirmation shows which seed it produced (T39b)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    go(SCREEN_WALLET_INFO);
    uint32_t plain = 0;
    CHECK(wallet_get_master_fingerprint(&plain) == WALLET_OK, "no base fingerprint");

    wallet_set_passphrase("hunter2", 7);
    go(SCREEN_PASSPHRASE_CONFIRM);

    uint32_t with = 0;
    CHECK(wallet_get_master_fingerprint(&with) == WALLET_OK, "no passphrase fingerprint");
    CHECK(with != plain, "the passphrase did not change the fingerprint");

    char expect[16];
    snprintf(expect, sizeof(expect), "XFP %08lX", (unsigned long)with);
    CHECK_SCREEN(fake_oled_contains(expect),
                 "the confirmation does not show %s", expect);
    /* A wrong passphrase is a valid wallet, so the screen still has to say
     * what the user is meant to do with the value. */
    CHECK_SCREEN(fake_oled_contains("Match"), "nothing tells the user to compare");

    wallet_clear_passphrase();
}

/* A fingerprint that cannot be derived must be absent, not a placeholder: a
 * row reading "XFP --------" is read as a value. */
static void test_xfp_is_absent_when_it_cannot_be_derived(void)
{
    printf("== no fingerprint is shown when none could be derived\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    /* Only the fingerprint fails: the wallet itself is fine, so this is the
     * case where an "XFP --------" placeholder would sit next to genuine data
     * and be read as genuine too. */
    fake_wallet_fail_fingerprint(true);

    go(SCREEN_WALLET_INFO);
    CHECK_SCREEN(fake_oled_row_contains(0, "W1/1"), "setup: the wallet is not shown");
    CHECK_SCREEN(!fake_oled_contains("XFP"), "a fingerprint was drawn without one");

    fake_wallet_fail_fingerprint(false);
}

/* ============================================================================
 * T60 - the passphrase selector honours the seed selector's setting
 * ============================================================================ */

static void test_entry_style_drives_both_selectors(void)
{
    printf("== the Entry setting drives seed entry only (T60)\n");
    boot_unlocked_with_seed();

    CHECK(!mnemonic_entry_blocks_enabled() && !text_entry_blocks_enabled(),
          "the selectors did not start flat");

    go(SCREEN_SETTINGS);
    bool found = false;
    for (int i = 0; i < 40 && !found; i++) {
        found = fake_oled_contains("> Entry ");
        if (!found) press(BUTTON_DOWN);
    }
    CHECK(found, "no Entry style item in the settings menu");
    if (!found) return;

    press(BUTTON_ACCEPT);
    CHECK(mnemonic_entry_blocks_enabled(), "the seed selector did not change");

    /* The passphrase ring deliberately stays flat, whatever the setting says.
     *
     * Blocks are fewer presses there too - 9.1 per character against 14.4 -
     * and were still reported worse to use on hardware. The reason is
     * structural: seed entry has one dimension, 26 letters narrowing as you
     * type, while the passphrase ring already carries a character-set mode on
     * top of ~95 characters. Blocks make that three levels of navigation on
     * four unlabelled buttons. A passphrase typed wrong is worse than one
     * typed slowly, so speed loses here. */
    CHECK(!text_entry_blocks_enabled(),
          "blocks reached the passphrase ring; it is meant to stay flat");

    go(SCREEN_PASSPHRASE);
    CHECK_SCREEN(!fake_oled_row_contains(7, "OPEN"),
                 "the passphrase footer offers a block level it should not have");
}

/* ============================================================================
 * T4 / AUDIT S8e - Change PIN is wired up and asks for three PINs
 * ============================================================================ */

/* Type a PIN on the selector and submit it. */
static bool type_pin(const char *pin)
{
    for (const char *p = pin; *p; p++) {
        if (!pin_pick(*p - '0')) {
            return false;
        }
    }
    return pin_pick(PIN_SUBMIT_OPTION);
}

static void test_change_pin_is_reachable_and_works(void)
{
    printf("== Change PIN asks for the old PIN then the new one twice (T4)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    /* Reach it the way a user does, by walking the settings menu. */
    go(SCREEN_SETTINGS);
    bool found = false;
    for (int i = 0; i < 40 && !found; i++) {
        /* The cursor, not merely the text: several entries are on screen at
         * once, and pressing ACCEPT acts on the selected one. */
        if (fake_oled_contains("> Change PIN")) {
            press(BUTTON_ACCEPT);
            found = ui_get_screen() == SCREEN_PIN_CHANGE;
            break;
        }
        press(BUTTON_DOWN);
    }
    CHECK(found, "Change PIN did not open its screen (screen %d)", ui_get_screen());
    CHECK_SCREEN(fake_oled_row_contains(0, "Current PIN"),
                 "header reads \"%s\"", fake_oled_row(0));

    CHECK(type_pin("1234"), "could not enter the current PIN");
    CHECK_SCREEN(fake_oled_row_contains(0, "New PIN"), "header reads \"%s\"",
                 fake_oled_row(0));

    CHECK(type_pin("8765"), "could not enter the new PIN");
    CHECK_SCREEN(fake_oled_row_contains(0, "Confirm"), "header reads \"%s\"",
                 fake_oled_row(0));

    CHECK(type_pin("8765"), "could not confirm the new PIN");
    CHECK(ui_get_screen() == SCREEN_SETTINGS,
          "a completed change did not return to settings (screen %d)",
          ui_get_screen());

    pin_lock();
    CHECK(pin_verify("8765"), "the new PIN does not unlock the device");
    pin_reset_attempts();
    CHECK(!pin_verify("1234"), "the old PIN still unlocks the device");
}

static void test_change_pin_rejects_a_wrong_current_pin(void)
{
    printf("== a wrong current PIN is refused and said so on screen (T4)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    go(SCREEN_PIN_CHANGE);
    CHECK(type_pin("9999"), "could not enter a wrong current PIN");
    CHECK(type_pin("8765"), "could not enter the new PIN");
    CHECK(type_pin("8765"), "could not confirm the new PIN");

    CHECK(ui_get_screen() == SCREEN_PIN_CHANGE,
          "a wrong current PIN was accepted (screen %d)", ui_get_screen());
    CHECK_SCREEN(fake_oled_contains("Wrong current PIN"),
                 "the refusal does not say why");

    pin_lock();
    pin_reset_attempts();
    CHECK(pin_verify("1234"), "the original PIN stopped working");
}

static void test_change_pin_catches_a_mismatch(void)
{
    printf("== a mistyped confirmation restarts at the new PIN (T4)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    go(SCREEN_PIN_CHANGE);
    CHECK(type_pin("1234"), "could not enter the current PIN");
    CHECK(type_pin("8765"), "could not enter the new PIN");
    CHECK(type_pin("8760"), "could not enter the mismatched confirmation");

    CHECK_SCREEN(fake_oled_contains("don't match"), "the mismatch is not stated");
    CHECK_SCREEN(fake_oled_row_contains(0, "New PIN"),
                 "did not return to the new PIN (header \"%s\")", fake_oled_row(0));

    pin_lock();
    pin_reset_attempts();
    CHECK(pin_verify("1234"), "the PIN changed despite the mismatch");
}

/* ------------------------------------------------- blind signing (T16) */

/* Walk the settings list until `label` is the selected row. The list scrolls
 * three at a time, so "press DOWN n times" would encode the position of every
 * item above it and break on the next one added. */
static bool settings_select(const char *label)
{
    go(SCREEN_SETTINGS);
    for (int i = 0; i < 30; i++) {
        for (int row = 0; row < FAKE_OLED_ROWS; row++) {
            const char *text = fake_oled_row(row);
            if (text[0] == '>' && strstr(text, label)) {
                return true;
            }
        }
        press(BUTTON_DOWN);
    }
    return false;
}

/* Enabling the hatch has to cost something. One press must not do it, and the
 * screen has to say what is being given up before it asks. */
static void test_blind_signing_takes_a_deliberate_act(void)
{
    printf("== turning blind signing on warns, and needs more than one press\n");
    boot_unlocked_with_seed();

    CHECK(!blind_signing_enabled(), "blind signing is not off on a fresh device");
    CHECK(settings_select("Blind"), "no blind-signing entry in settings");
    CHECK_SCREEN(fake_oled_contains("[OFF]"),
                 "the settings row does not show the setting is off");

    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_BLIND_WARN,
          "selecting it did not open the warning (screen %d)", ui_get_screen());

    /* What it costs, in words. Not "advanced mode" or "expert settings". */
    CHECK_SCREEN(fake_oled_contains("cannot read"),
                 "the warning does not say the device cannot read the calls");
    CHECK_SCREEN(fake_oled_contains("drain"),
                 "the warning does not say what can go wrong");

    /* Four presses is not five. */
    for (int i = 0; i < 4; i++) {
        press(BUTTON_ACCEPT);
        CHECK(!blind_signing_enabled(),
              "blind signing turned on after only %d presses", i + 1);
        CHECK(ui_get_screen() == SCREEN_BLIND_WARN,
              "the warning left the screen after %d presses", i + 1);
    }
    press(BUTTON_ACCEPT);
    CHECK(blind_signing_enabled(), "five presses did not enable blind signing");
    CHECK(ui_get_screen() == SCREEN_SETTINGS, "did not return to settings");
    CHECK(settings_select("Blind"), "the entry vanished once enabled");
    CHECK_SCREEN(fake_oled_contains("[ON]"),
                 "a weakened device does not say so on the settings row");

    /* Turning it back off is one press: nothing is lost by doing it early. */
    press(BUTTON_ACCEPT);
    CHECK(!blind_signing_enabled(), "one press did not turn the protection back on");

    /* And abandoning the warning part-way leaves it off, with no partial
     * credit carried into the next visit. */
    CHECK(settings_select("Blind"), "the entry vanished");
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_SETTINGS, "CANCEL did not leave the warning");
    CHECK(!blind_signing_enabled(), "an abandoned warning enabled it anyway");

    /* Leaving reset the settings cursor, so find the row again rather than
     * assuming where it is. */
    CHECK(settings_select("Blind"), "the entry vanished after a cancel");
    press(BUTTON_ACCEPT);          /* re-enter the warning */
    CHECK(ui_get_screen() == SCREEN_BLIND_WARN, "could not re-enter the warning");
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    CHECK(!blind_signing_enabled(),
          "the earlier presses were still counted on a second visit");
    press(BUTTON_CANCEL);
}

/* Locking takes the channel with it.
 *
 * The encrypted session used to survive a lock: the wallet closed, the host
 * kept talking, and a user returning to enter their PIN resumed on a session
 * authorised before the lock -- possibly hours before, with the device
 * unattended in between. Seen on hardware, where the companion carried on
 * polling for two minutes after a 300 s auto-lock.
 *
 * Checked through the auto-lock because it is the path nobody chooses; the
 * others reach the same lock_device().
 */
static void test_locking_closes_the_channel(void)
{
    printf("== locking the device closes the channel too\n");
    boot_unlocked_with_seed();

    /* Any well-formed peer key will do: this is about the lock, not the
     * handshake, which test_session covers. */
    uint8_t host_pub[SESSION_PUBKEY_SIZE];
    for (size_t i = 0; i < sizeof(host_pub); i++) {
        host_pub[i] = (uint8_t)(0x40 + i);
    }
    uint8_t dev_pub[SESSION_PUBKEY_SIZE], commit[SESSION_COMMIT_SIZE];
    uint8_t host_nonce[SESSION_NONCE_SIZE], dev_nonce[SESSION_NONCE_SIZE];
    memset(host_nonce, 0x5a, sizeof(host_nonce));

    CHECK(session_begin(host_pub, dev_pub, commit), "session_begin refused");
    CHECK(session_reveal(host_nonce, dev_nonce), "session_reveal refused");
    session_confirm();
    CHECK(session_state() == SESSION_ACTIVE, "no session to lose");

    /* Idle past the auto-lock deadline. */
    fake_clock_advance_us((int64_t)31 * 60 * 1000000);
    CHECK(ui__check_autolock_for_test(), "the device did not auto-lock");

    CHECK(session_state() == SESSION_IDLE,
          "the channel survived the lock: a returning host resumes on a session "
          "authorised before the device was left unattended");
}

/* An approval nobody answered has to leave the screen.
 *
 * wait_for_user()'s 120 s deadline answers the host and calls ui_sign_clear(),
 * which resets the outcome and nothing else. A question left on the glass
 * after the host has been told no would still take presses, and the device
 * must never appear to accept something nothing is waiting for. Found on
 * hardware while reproducing H-1, on the signing screen; the host passphrase
 * question shares wait_for_user() and the same gap.
 */
static void test_an_expired_approval_leaves_the_screen(void)
{
    printf("== an approval nobody answered comes off the screen\n");
    boot_unlocked_with_seed();

    ui_request_passphrase_confirm(0x1234ABCDu);
    go(SCREEN_HOST_PASSPHRASE_CONFIRM);
    CHECK(ui_user_is_answering(), "the passphrase question does not count as one");

    /* The protocol task gives up: answers the host, clears the request. */
    ui_sign_clear();
    ui_sign_expire();
    ui__service_sign_expiry_for_test();
    ui_render();
    CHECK(ui_get_screen() != SCREEN_HOST_PASSPHRASE_CONFIRM,
          "an expired passphrase question left its confirmation screen up");
    CHECK(ui_sign_outcome() == SIGN_PENDING, "the expiry recorded an answer");
}

/* The host-entry passphrase path (PROTOCOL.md 5): the fingerprint is the
 * whole defence, and saying no has to be a real answer rather than a delay. */
static void test_host_passphrase_confirmation(void)
{
    printf("== a host-supplied passphrase is confirmed against its fingerprint\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    ui_request_passphrase_confirm(0x1234ABCDu);
    CHECK(ui_user_is_answering(), "a pending question does not count as one");
    go(SCREEN_HOST_PASSPHRASE_CONFIRM);

    CHECK_SCREEN(fake_oled_contains("XFP 1234ABCD"),
                 "the resulting seed's fingerprint is not on screen");
    /* The user has to know this came from the host, which is the weaker path. */
    CHECK_SCREEN(fake_oled_contains("host"),
                 "the screen does not say the passphrase came from the app");
    CHECK(ui_sign_outcome() == SIGN_PENDING, "the prompt answered itself");

    press(BUTTON_CANCEL);
    CHECK(ui_sign_outcome() == SIGN_REJECTED, "CANCEL did not reject");
    CHECK(ui_get_screen() == SCREEN_WALLET_INFO, "a rejection did not land on the wallet");
    ui_sign_clear();

    ui_request_passphrase_confirm(0x00000001u);
    go(SCREEN_HOST_PASSPHRASE_CONFIRM);
    CHECK_SCREEN(fake_oled_contains("XFP 00000001"),
                 "a small fingerprint is not shown at full width");
    press(BUTTON_ACCEPT);
    CHECK(ui_sign_outcome() == SIGN_APPROVED, "ACCEPT did not approve");
    ui_sign_clear();
}


/* Roadmap 3.4: the Zcash review. A signature is reachable only by walking
 * every page; CANCEL refuses from any of them; an unanswered review comes
 * off the screen. The page text itself is test_zcash_review's subject. */
static void test_zsign_review_walks_every_page(void)
{
    printf("== a Zcash review signs only from its last page\n");
    boot_unlocked_with_seed();
    static zs_summary s;
    memset(&s, 0, sizeof s);
    s.branch_id = 0x37a5165b;
    s.fee = 10000;
    s.n_spends = 1;
    s.n_outputs = 2;
    s.outputs[0].pool = ZS_POOL_IRONWOOD;
    s.outputs[0].kind = ZS_KIND_EXTERNAL;
    s.outputs[0].value = 400000;
    strcpy(s.outputs[0].address, "utest1lpcp9q0703pkz3g8cqn7gnedx35pfcps0uwm4a84cwz8sxpsm8y");
    s.outputs[1].pool = ZS_POOL_IRONWOOD;
    s.outputs[1].kind = ZS_KIND_CHANGE;
    s.outputs[1].value = 590000;
    int pages = zreview_page_count(&s, ZS_NET_TEST);

    ui_request_zsign_review(&s, ZS_NET_TEST);
    CHECK(ui_user_is_answering(), "a pending review does not count as a question");
    go(SCREEN_ZSIGN_REVIEW);
    CHECK_SCREEN(fake_oled_row_contains(0, "Send 1 of 1"), "first page is \"%s\"", fake_oled_row(0));
    CHECK_SCREEN(fake_oled_contains("0.004 TAZ"), "amount not shown");
    press(BUTTON_UP);
    CHECK_SCREEN(fake_oled_row_contains(0, "Send 1 of 1"), "UP moved before the first page");
    for (int i = 1; i < pages; i++) {
        CHECK(ui_sign_outcome() == SIGN_PENDING, "answered on page %d of %d", i, pages);
        press(BUTTON_ACCEPT);
    }
    CHECK_SCREEN(fake_oled_row_contains(0, "Sign?"), "last page is \"%s\"", fake_oled_row(0));
    CHECK(ui_sign_outcome() == SIGN_PENDING, "reaching the last page signed");
    press(BUTTON_DOWN);
    CHECK_SCREEN(fake_oled_row_contains(0, "Sign?"), "DOWN moved past the last page");
    press(BUTTON_ACCEPT);
    CHECK(ui_sign_outcome() == SIGN_APPROVED, "ACCEPT on the last page did not sign");
    CHECK(ui_get_screen() == SCREEN_WALLET_INFO, "approval did not leave the review");
    ui_sign_clear();

    ui_request_zsign_review(&s, ZS_NET_TEST);
    go(SCREEN_ZSIGN_REVIEW);
    press(BUTTON_DOWN);
    press(BUTTON_CANCEL);
    CHECK(ui_sign_outcome() == SIGN_REJECTED, "CANCEL mid-review did not reject");
    CHECK(ui_get_screen() == SCREEN_WALLET_INFO, "rejection did not leave the review");
    ui_sign_clear();

    ui_request_zsign_review(&s, ZS_NET_TEST);
    go(SCREEN_ZSIGN_REVIEW);
    ui_sign_clear();
    ui_sign_expire();
    ui__service_sign_expiry_for_test();
    ui_render();
    CHECK(ui_get_screen() != SCREEN_ZSIGN_REVIEW, "an expired review stayed on screen");
    CHECK(ui_sign_outcome() == SIGN_PENDING, "the expiry recorded an answer");
}

/* ============================================================================
 * AUDIT S8f - the wallet-create handler no longer renders re-entrantly
 * ============================================================================ */

/* What S8f actually was: screen_wallet_create_on_button() called ui_render()
 * itself, from inside a button handler, to get "Generating..." onto the panel
 * before a second of blocking work. The intent was real - the device must not
 * look dead - so the fix keeps it and moves the work instead: the handler sets
 * a flag, the loop paints, and ui_poll_deferred() does the generating.
 *
 * The property under test is the one the second render path put at risk: after
 * the press and before the pump, the screen says it is working and NOTHING has
 * happened yet. Delete the deferral and the wallet is already created by the
 * time press() returns; keep the re-entrant render and the frame is produced
 * from inside a handler that is still mutating the screen's state. */
static void test_seed_generation_happens_behind_its_own_frame(void)
{
    printf("== generating a seed is deferred out of the button handler (S8f)\n");
    boot_unlocked_with_seed();

    reach_generation_ready();

    CHECK(ui_get_screen() == SCREEN_WALLET_CREATE,
          "the handler left the screen before the work was announced");
    CHECK_SCREEN(fake_oled_contains("Generating"),
                 "the user is not told the device is working");
    /* The seed buffer is the evidence: it is filled by the generation and by
     * nothing else on this path. */
    CHECK(ui__mnemonic_buffer_for_test()[0] == '\0',
          "the seed was generated inside the button handler - the frame that "
          "announces it can never have been on the panel first (S8f)");

    /* One turn of the loop: paint, then the deferred work. */
    idle_pump();

    CHECK(ui__mnemonic_buffer_for_test()[0] != '\0',
          "the deferred generation never ran");
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_DISPLAY,
          "the new seed was not shown to be written down");
}

/* Leaving the screen while "Generating..." is up must cancel the request, not
 * queue a seed for whatever screen comes next. Only reachable at all because
 * the handler no longer blocks. */
static void test_leaving_the_create_screen_cancels_the_generation(void)
{
    printf("== abandoning the create screen abandons the generation (S8f)\n");
    boot_unlocked_with_seed();

    go(SCREEN_WALLET_CREATE);
    press(BUTTON_ACCEPT);
    go(SCREEN_MAIN_MENU);
    idle_pump();

    CHECK(ui__mnemonic_buffer_for_test()[0] == '\0',
          "a seed was generated after the user left the screen");
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "the deferred work stole the screen");
}

/* ============================================================================
 * T42 - when the passphrase clears, and what the screen says about it
 *
 * A passphrase is not a setting; it selects a wallet. Every one of these is a
 * path where the device could go on deriving from a passphrase the user
 * believes is gone, or show an address from one they believe is applied.
 * ============================================================================ */

static void test_autolock_drops_the_passphrase(void)
{
    printf("== auto-lock drops the passphrase, not just the screen (T42)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");
    wallet_set_passphrase("hunter2", 7);
    CHECK(wallet_has_passphrase(), "setup: the passphrase did not apply");

    go(SCREEN_WALLET_INFO);
    /* Default timeout is 5 minutes; go well past it. */
    fake_clock_advance_us(6 * 60 * 1000000LL);
    CHECK(ui__check_autolock_for_test(), "the device did not auto-lock");

    CHECK(!wallet_has_passphrase(),
          "the passphrase survived an auto-lock - the PIN alone would reopen "
          "a hidden wallet, which is the second factor gone");
    CHECK(ui_get_screen() == SCREEN_PIN_UNLOCK, "auto-lock did not ask for the PIN");

    /* And unlocking again does not bring it back. */
    CHECK(pin_verify("1234"), "could not unlock again");
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "vault would not reopen");
    CHECK(!wallet_has_passphrase(), "unlocking restored a passphrase nobody typed");
}

static void test_switching_wallets_drops_the_passphrase(void)
{
    printf("== switching seeds drops the passphrase and the fingerprint with it\n");
    boot_unlocked_with_seed();
    CHECK(fake_wallet_preload(PHRASE_12) == 2, "setup: need a second seed");
    CHECK(wallet_select_wallet(1) == WALLET_OK, "setup: could not select seed 1");
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    wallet_set_passphrase("hunter2", 7);
    go(SCREEN_WALLET_INFO);
    char with_pass[43];
    snprintf(with_pass, sizeof(with_pass), "%s", fake_oled_row(2));

    go(SCREEN_WALLET_SELECT);
    press(BUTTON_DOWN);
    press(BUTTON_ACCEPT);

    CHECK(!wallet_has_passphrase(),
          "the passphrase followed the user to another seed - a wallet nobody "
          "named, that looks empty");
    CHECK(ui_get_screen() == SCREEN_WALLET_INFO, "the switch did not show the wallet");
    CHECK_SCREEN(strcmp(fake_oled_row(2), with_pass) != 0,
                 "the fingerprint did not change with the wallet");
}

/* The one that cannot be fixed by clearing state alone: the screen is ALREADY
 * showing a fingerprint when the passphrase changes underneath
 * it. Nothing presses a button, so nothing re-derives unless the UI task
 * notices. Every real trigger is another task - a host setPassphrase, a
 * host-rejected confirmation, a disconnect dropping a host passphrase. */
static void test_a_passphrase_change_under_the_wallet_screen_re_derives(void)
{
    printf("== the wallet screen never shows a fingerprint from a passphrase that "
           "is no longer applied (T42)\n");
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");

    go(SCREEN_WALLET_INFO);
    char base_row[43];
    snprintf(base_row, sizeof(base_row), "%s", fake_oled_row(2));
    uint32_t base_fp = 0;
    CHECK(wallet_get_master_fingerprint(&base_fp) == WALLET_OK, "no base fingerprint");

    /* A host applies one while the screen sits there. */
    wallet_set_passphrase("hunter2", 7);
    idle_pump();    /* notices, re-derives, marks dirty */
    idle_pump();    /* paints */

    CHECK_SCREEN(strcmp(fake_oled_row(2), base_row) != 0,
                 "the screen still shows the base wallet's fingerprint after a "
                 "passphrase was applied");
    CHECK_SCREEN(fake_oled_row_contains(0, "P"),
                 "nothing on screen says a passphrase is applied (\"%s\")",
                 fake_oled_row(0));
    uint32_t with_fp = 0;
    CHECK(wallet_get_master_fingerprint(&with_fp) == WALLET_OK, "no fingerprint");
    char expect[16];
    snprintf(expect, sizeof(expect), "XFP %08lX", (unsigned long)with_fp);
    CHECK_SCREEN(fake_oled_contains(expect),
                 "the fingerprint is stale relative to the applied passphrase");

    /* And the other direction: the host, or a rejection, clears it again. */
    wallet_clear_passphrase();
    idle_pump();
    idle_pump();

    CHECK_SCREEN(strcmp(fake_oled_row(2), base_row) == 0,
                 "the screen kept a fingerprint derived from a passphrase that "
                 "is gone");
    snprintf(expect, sizeof(expect), "XFP %08lX", (unsigned long)base_fp);
    CHECK_SCREEN(fake_oled_contains(expect), "the fingerprint stayed stale");
    CHECK(!ui__wallet_info_pass_shown_for_test(),
          "the screen still believes a passphrase is applied");
}

/* ============================================================================
 * T45 - accounts
 * ============================================================================ */

/* Walk the settings list to an item by its label, the way a user would. */
static void settings_goto(const char *label)
{
    go(SCREEN_SETTINGS);
    for (int guard = 0; guard < 40; guard++) {
        if (fake_oled_contains(label)) {
            /* Only stop when it is the SELECTED line: pressing ACCEPT acts on
             * the selection, not on whatever happens to be visible. */
            for (int page = 2; page <= 6; page += 2) {
                const char *row = fake_oled_row(page);
                if (row && row[0] == '>' && strstr(row, label)) {
                    return;
                }
            }
        }
        press(BUTTON_DOWN);
    }
    CHECK(false, "settings has no item labelled \"%s\"", label);
}

static void test_the_account_is_selectable_and_bounded(void)
{
    printf("== the account level is selectable, and bounded (T45)\n");
    boot_unlocked_with_seed();

    settings_goto("Account");
    CHECK_SCREEN(fake_oled_contains("Account 0"), "the account is not shown");

    press(BUTTON_ACCEPT);
    CHECK(ui__account_for_test() == 1, "the account did not advance");
    CHECK_SCREEN(fake_oled_contains("Account 1"), "the new account is not shown");

    /* Round the whole cycle: it must wrap, and it must wrap at the documented
     * bound rather than running off into accounts no screen can name. */
    for (int i = 1; i < HD_ACCOUNT_COUNT; i++) {
        press(BUTTON_ACCEPT);
    }
    CHECK(ui__account_for_test() == 0,
          "the account selector did not wrap at HD_ACCOUNT_COUNT (%u after a "
          "full cycle)", (unsigned)ui__account_for_test());
}

static void test_the_wallet_screen_names_the_account(void)
{
    printf("== the wallet screen shows the account the device is on (T45)\n");
    boot_unlocked_with_seed();

    go(SCREEN_WALLET_INFO);
    CHECK_SCREEN(fake_oled_row_contains(0, "acct 0"),
                 "no account on the wallet screen (\"%s\")", fake_oled_row(0));

    settings_goto("Account");
    press(BUTTON_ACCEPT);
    go(SCREEN_WALLET_INFO);

    CHECK_SCREEN(fake_oled_row_contains(0, "acct 1"),
                 "the wallet screen does not follow the account (\"%s\")",
                 fake_oled_row(0));
}

/* The selection is the analogue of active_idx, so it survives a reboot for the
 * same reason: a device that silently reverts to account 0 shows a different
 * address for the same wallet, which reads as funds having vanished. */
static void test_the_account_survives_a_reboot(void)
{
    printf("== the selected account persists like the selected wallet (T45)\n");
    boot_unlocked_with_seed();

    settings_goto("Account");
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    CHECK(ui__account_for_test() == 2, "setup: could not select account 2");

    /* Power-cycle without erasing flash - everything boot_device() does except
     * fake_nvs_reset(). */
    fake_clock_reset();
    fake_oled_reset();
    fake_input_reset();
    fake_wallet_reset();
    pin__reset_static_state_for_test();
    ui__reset_static_state_for_test();
    pin_init();
    wallet_init();
    ui_init();
    ui_render();

    CHECK(ui__account_for_test() == 2,
          "the account reverted to 0 across a reboot (got %u)",
          (unsigned)ui__account_for_test());
}

/* A wipe erases the vault namespaces, not the settings one, so the account has
 * to be reset explicitly or the next owner's first seed comes up at the
 * previous owner's account. */
static void test_a_wipe_resets_the_account(void)
{
    printf("== a wipe returns the device to account 0 (T45)\n");
    boot_unlocked_with_seed();

    settings_goto("Account");
    press(BUTTON_ACCEPT);
    CHECK(ui__account_for_test() == 1, "setup: could not select account 1");

    go(SCREEN_WIPE_CONFIRM);
    for (int i = 0; i < 8; i++) {
        press(BUTTON_ACCEPT);
    }
    CHECK(ui_get_screen() == SCREEN_PIN_SETUP, "setup: the wipe did not complete");
    CHECK(ui__account_for_test() == 0, "the account selection survived a wipe");
}

/* ============================================================================
 * T42 - every lock path locks the same thing
 *
 * Reported from hardware: lock the device from the menu, unlock it, and you
 * are still in the passphrase wallet. pin_lock() and wallet_lock() were each
 * individually right; the four CALL SITES disagreed, and the one a user
 * reaches for deliberately was the one that only closed the PIN gate.
 *
 * That is why these are per-path tests rather than one test of a function. A
 * test of lock_device() would have passed on the broken firmware, because
 * lock_device() is not what the menu called.
 * ============================================================================ */

/* The shared assertion: after this, nothing about the previous session is
 * still live. Takes the path's name so a failure says which one. */
static void check_fully_locked(const char *path)
{
    CHECK(!pin_is_unlocked(), "%s: the PIN gate is still open", path);

    WalletStatus st = wallet_get_status();
    CHECK(!st.unlocked,
          "%s: the vault stayed open behind the PIN gate - the seed never left "
          "RAM and unlocking returns to the same wallet", path);
    CHECK(!wallet_has_passphrase(),
          "%s: the passphrase survived the lock, so the PIN alone reopens a "
          "hidden wallet (T42)", path);
    CHECK(ui__mnemonic_buffer_for_test()[0] == '\0',
          "%s: the UI's copy of the seed outlived the lock", path);
    CHECK(ui_get_screen() == SCREEN_PIN_UNLOCK,
          "%s: the device did not ask for the PIN", path);

    /* The fingerprint names the seed the passphrase produced. Left cached, the
     * next screen to draw it would vouch for a wallet the device can no longer
     * derive - the same bug, one row quieter. */
    CHECK_SCREEN(!fake_oled_contains("XFP"),
                 "%s: a stale fingerprint is still on screen", path);
    CHECK(ui__master_xfp_for_test()[0] == '\0',
          "%s: the cached fingerprint survived the lock - the next screen to "
          "draw it would vouch for a wallet the device can no longer derive",
          path);
    CHECK_SCREEN(!fake_oled_contains("ZFP"),
                 "%s: a stale Zcash fingerprint is still on screen", path);
    CHECK(ui__master_zfp_for_test()[0] == '\0',
          "%s: the cached Zcash fingerprint survived the lock", path);
}

/* Put the device in the state the bug was reported from: unlocked, seed
 * loaded, a passphrase applied, and a fingerprint on screen derived from it. */
static void unlocked_in_a_passphrase_wallet(void)
{
    boot_unlocked_with_seed();
    CHECK(wallet_unlock("1234", 4) == WALLET_OK, "setup: vault would not unlock");
    wallet_set_passphrase("hunter2", 7);
    go(SCREEN_WALLET_INFO);
    CHECK(wallet_has_passphrase(), "setup: no passphrase applied");
    CHECK_SCREEN(fake_oled_contains("XFP"), "setup: no fingerprint on screen");
}

/* Perform the deliberate lock: hold BACK on the home screen for its full
 * duration, and answer the confirmation if one is raised.
 *
 * BACK is held rather than tapped because a tap is how you leave every other
 * screen, and on the home screen it used to also throw away the passphrase,
 * the temporary seed and the channel. `confirm` says whether this call expects
 * the device to ask first -- it does exactly when there is a RAM-only secret
 * to lose. */
static void hold_back_to_lock(bool expect_confirm)
{
    go(SCREEN_MAIN_MENU);
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_LOCK_HOLD,
          "BACK on the home screen did not start a hold");

    fake_button_hold(BUTTON_CANCEL);
    fake_clock_advance_us(LOCK_HOLD_US);
    ui__service_lock_hold_for_test();
    fake_button_release();

    if (expect_confirm) {
        CHECK(ui_get_screen() == SCREEN_LOCK_CONFIRM,
              "a lock that costs a RAM-only secret did not say so first");
        CHECK(pin_is_unlocked(), "the confirmation screen had already locked");
        press(BUTTON_ACCEPT);
    }
    ui_render();
}

/* Defined further down, with the other temporary-seed fixtures. */
static void enter_temporary_seed(void);

/* A tap is not a lock.
 *
 * The whole reason this screen exists: BACK is how you leave every other
 * screen, and on the home screen the same press used to throw away the
 * passphrase, the temporary seed and the encrypted channel.
 */
static void test_a_tap_on_back_does_not_lock(void)
{
    printf("== a tap on BACK does not lock; only a full hold does\n");
    unlocked_in_a_passphrase_wallet();

    go(SCREEN_MAIN_MENU);
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_LOCK_HOLD, "BACK did not start a hold");

    /* Released immediately -- which is what a tap is. */
    fake_button_release();
    ui__service_lock_hold_for_test();

    CHECK(ui_get_screen() == SCREEN_MAIN_MENU,
          "letting go did not abandon the lock");
    CHECK(pin_is_unlocked(), "a tap locked the device");
    CHECK(wallet_has_passphrase(), "a tap threw away the passphrase");

    /* Nor does letting go part-way through. */
    press(BUTTON_CANCEL);
    fake_button_hold(BUTTON_CANCEL);
    fake_clock_advance_us(LOCK_HOLD_US - 1);
    ui__service_lock_hold_for_test();
    CHECK(ui_get_screen() == SCREEN_LOCK_HOLD, "the hold ended early");
    CHECK(pin_is_unlocked(), "the device locked before the hold completed");

    fake_button_release();
    ui__service_lock_hold_for_test();
    CHECK(pin_is_unlocked(), "letting go one tick short still locked");
}

/* The bar has to move, or the user lets go thinking the button is broken. */
static void test_the_hold_shows_progress(void)
{
    printf("== the hold draws a bar that fills\n");
    boot_unlocked_with_seed();

    go(SCREEN_MAIN_MENU);
    press(BUTTON_CANCEL);
    fake_button_hold(BUTTON_CANCEL);

    ui_render();
    CHECK_SCREEN(fake_oled_contains("[") && !fake_oled_contains("#"),
                 "the bar was not empty at the start of the hold");

    fake_clock_advance_us(LOCK_HOLD_US / 2);
    ui_render();
    CHECK_SCREEN(fake_oled_contains("########") &&
                 !fake_oled_contains("################"),
                 "the bar was not half full half way through the hold");

    fake_button_release();
}

/* With nothing RAM-only to lose, the hold is the whole gesture. A confirmation
 * every time is answered reflexively within a week and then protects nothing. */
static void test_a_hold_with_nothing_to_lose_locks_outright(void)
{
    printf("== a hold with no passphrase or temp seed locks without asking\n");
    boot_unlocked_with_seed();
    CHECK(!wallet_has_passphrase(), "setup: a passphrase is applied");
    CHECK(!wallet_has_temporary_mnemonic(), "setup: a temporary seed is loaded");

    hold_back_to_lock(false);       /* nothing to lose: no question */

    CHECK(ui_get_screen() == SCREEN_PIN_UNLOCK, "the device did not ask for the PIN");
    CHECK(!pin_is_unlocked(), "the hold did not lock the device");
}

/* And when there is something to lose, the screen says what it is. A bare
 * "Are you sure?" is friction; naming the cost is information. */
static void test_the_confirmation_names_what_is_lost(void)
{
    printf("== the lock confirmation names what the lock costs\n");

    /* A passphrase on a stored wallet. */
    unlocked_in_a_passphrase_wallet();
    go(SCREEN_MAIN_MENU);
    press(BUTTON_CANCEL);
    fake_button_hold(BUTTON_CANCEL);
    fake_clock_advance_us(LOCK_HOLD_US);
    ui__service_lock_hold_for_test();
    fake_button_release();
    ui_render();

    CHECK(ui_get_screen() == SCREEN_LOCK_CONFIRM, "no confirmation was raised");
    CHECK_SCREEN(fake_oled_contains("Passphrase"),
                 "the confirmation does not say the passphrase is at stake");
    CHECK(pin_is_unlocked(), "the confirmation had already locked the device");

    /* Backing out leaves everything exactly as it was. */
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "backing out did not return home");
    CHECK(pin_is_unlocked() && wallet_has_passphrase(),
          "backing out of the confirmation locked anyway");

    /* A temporary seed, which is the more expensive one to retype. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    go(SCREEN_MAIN_MENU);
    press(BUTTON_CANCEL);
    fake_button_hold(BUTTON_CANCEL);
    fake_clock_advance_us(LOCK_HOLD_US);
    ui__service_lock_hold_for_test();
    fake_button_release();
    ui_render();

    CHECK(ui_get_screen() == SCREEN_LOCK_CONFIRM, "no confirmation for a temp seed");
    CHECK_SCREEN(fake_oled_contains("Temp seed"),
                 "the confirmation does not say the temporary seed is at stake");
}

static void test_the_menu_lock_actually_locks(void)
{
    printf("== \"Lock device\" on the menu locks the vault, not just the PIN\n");
    unlocked_in_a_passphrase_wallet();

    hold_back_to_lock(true);    /* a passphrase is live: it must ask */

    check_fully_locked("menu lock");
}

static void test_the_host_lock_actually_locks(void)
{
    printf("== a host-requested lock drops the same things (T42)\n");
    unlocked_in_a_passphrase_wallet();

    /* What the protocol task does; the UI task picks it up on its next pass. */
    ui_request_lock();
    ui__service_host_lock_for_test();
    idle_pump();

    check_fully_locked("host lock");
}

static void test_the_autolock_timeout_locks(void)
{
    printf("== the auto-lock timeout drops the same things (T42)\n");
    unlocked_in_a_passphrase_wallet();

    fake_clock_advance_us(6 * 60 * 1000000LL);
    CHECK(ui__check_autolock_for_test(), "the device did not auto-lock");
    idle_pump();

    check_fully_locked("auto-lock");
}

/* Show Seed re-asks for the PIN so a reveal cannot ride on a session unlocked
 * minutes ago. It now locks fully, deliberately: asking "prove you are the
 * owner" while holding the decrypted mnemonic in RAM answers a different
 * question, and the correct PIN re-derives it anyway. */
static void test_show_seed_relocks_the_vault_too(void)
{
    printf("== Show Seed re-asks for the PIN with nothing left in RAM (S5/T42)\n");
    unlocked_in_a_passphrase_wallet();

    settings_goto("Show Seed");
    press(BUTTON_ACCEPT);

    check_fully_locked("show seed");

    /* And the request itself survives, or the item would do nothing. */
    CHECK(pin_verify("1234"), "could not re-enter the PIN");
    press(BUTTON_ACCEPT);
}


/* ============================================================================
 * T69 - the temporary seed
 *
 * The mode's whole claim is that nothing is stored. That property is asserted
 * against real NVS bytes in test_temp_seed.c, which links the real vault; what
 * belongs here is the half a user can see and act on: that the device says it
 * is in the mode, that it says what the mode costs before they type 24 words,
 * and that every way of leaving it actually leaves it.
 * ============================================================================ */

/* Walk the main menu to the item with this label and stop on it selected. The
 * temporary-seed entry moves as wallets appear and disappear, so the tests find
 * it by name rather than by index. */
static bool menu_goto(const char *label)
{
    go(SCREEN_MAIN_MENU);
    for (int guard = 0; guard < 12; guard++) {
        for (int row = 2; row <= 6; row += 2) {
            const char *r = fake_oled_row(row);
            if (r && r[0] == '>' && strstr(r, label)) {
                return true;
            }
        }
        press(BUTTON_DOWN);
    }
    CHECK(false, "the main menu has no item labelled \"%s\"", label);
    return false;
}

/* Menu -> warning -> twelve words, the way a user gets there. */
static void enter_temporary_seed(void)
{
    if (!menu_goto("Temp Seed")) {
        return;
    }
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_TEMP_SEED, "the warning screen did not open");
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_MNEMONIC_ENTRY, "seed entry did not open");
    CHECK(ui__entry_is_temporary_for_test(), "entry is not marked temporary");

    press(BUTTON_ACCEPT);   /* 12 words, the default */

    char words[16][16];
    int n = 0;
    const char *p = PHRASE_12;
    while (*p && n < 16) {
        int k = 0;
        while (*p && *p != ' ' && k < 15) { words[n][k++] = *p++; }
        words[n][k] = '\0';
        n++;
        while (*p == ' ') { p++; }
    }
    /* The last word is the one that commits the phrase: mnemonic_entry_finish()
     * runs inside that press, clears the entry and changes screen, so
     * type_word() sees an emptied buffer and reports failure. What proves the
     * word landed there is the mode being on, not the word counter. */
    for (int i = 0; i < n; i++) {
        const bool last = (i == n - 1);
        if (!type_word(words[i]) && !last) {
            CHECK(false, "could not type word %d (\"%s\")", i + 1, words[i]);
            return;
        }
    }
    CHECK(wallet_has_temporary_mnemonic(),
          "the phrase was typed in full and the device did not adopt it");
}

static void test_temp_seed_warns_before_it_is_used(void)
{
    printf("== the temporary seed says what it costs before a word is typed (T69)\n");
    boot_unlocked_with_seed();

    CHECK(menu_goto("Temp Seed"), "the mode is not offered on the home screen");
    press(BUTTON_ACCEPT);

    /* Consequence one: it is not stored, and losing power loses it. */
    CHECK_SCREEN(fake_oled_contains("NOT saved"),
                 "the warning does not say the seed is not stored");
    CHECK_SCREEN(fake_oled_contains("reboot") && fake_oled_contains("erases"),
                 "the warning does not say a reboot destroys the seed");

    press(BUTTON_DOWN);
    /* Consequence two: the PIN is guarding nothing at rest, and the longer
     * auto-lock this mode runs on. */
    CHECK_SCREEN(fake_oled_contains("PIN guards nothing"),
                 "the warning does not say what the PIN is worth here");
    CHECK_SCREEN(fake_oled_contains("30 min"),
                 "the warning does not state this mode's auto-lock");

    /* And it is escapable without entering anything. */
    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "CANCEL did not return to the menu");
    CHECK(!wallet_has_temporary_mnemonic(), "backing out started the mode anyway");
}

static void test_temp_seed_is_visible_on_screen(void)
{
    printf("== a temporary seed is named on screen, not merely in effect (T69)\n");
    boot_unlocked_with_seed();

    /* Typing it says so on every word, not only on the warning. */
    CHECK(menu_goto("Temp Seed"), "the mode is not offered");
    press(BUTTON_ACCEPT);
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_contains("Temp seed"),
                 "the entry screen does not say which flow this is");
    press(BUTTON_ACCEPT);
    CHECK_SCREEN(fake_oled_row_contains(0, "TEMP word 1/12"),
                 "the word header reads \"%s\"", fake_oled_row(0));
    go(SCREEN_MAIN_MENU);

    enter_temporary_seed();
    CHECK(wallet_has_temporary_mnemonic(), "the seed was not adopted");
    CHECK(ui_get_screen() == SCREEN_WALLET_INFO,
          "the device did not land on the wallet screen");

    /* The wallet screen names the seed it derived from. "W1/1" here would be
     * a lie about a wallet that is not the one in use. */
    CHECK_SCREEN(fake_oled_row_contains(0, "TEMP"),
                 "the wallet screen reads \"%s\"", fake_oled_row(0));
    CHECK_SCREEN(!fake_oled_row_contains(0, "W1/1"),
                 "the wallet screen claims a stored wallet");
    CHECK_SCREEN(fake_oled_contains("XFP"),
                 "no fingerprint - the user cannot tell which seed this is");

    go(SCREEN_MAIN_MENU);
    CHECK_SCREEN(fake_oled_row_contains(0, "TEMP"),
                 "the home screen does not say the device is in temporary mode: \"%s\"",
                 fake_oled_row(0));
    CHECK(menu_goto("End Temp Seed"), "there is no way out of the mode on the menu");
}

/* The stored wallet is untouched underneath: the temporary seed did not become
 * wallet 2, and it did not replace wallet 1. */
static void test_temp_seed_does_not_become_a_stored_wallet(void)
{
    printf("== a temporary seed never joins the stored wallets (T69)\n");
    boot_unlocked_with_seed();
    enter_temporary_seed();

    WalletStatus st = wallet_get_status();
    CHECK(st.wallet_count == 1, "the wallet count moved to %u", (unsigned)st.wallet_count);
    CHECK(st.active_wallet_index == 0,
          "a stored wallet is still selected (%u) while a temporary seed is in use",
          (unsigned)st.active_wallet_index);
    CHECK(!fake_wallet_backup_verified(1),
          "the stored wallet's backup state was rewritten");
}

/* Every way out. Each is checked through the same predicate, because "cleared"
 * has to mean the same thing on all of them. */
static void check_temp_seed_gone(const char *path)
{
    CHECK(!wallet_has_temporary_mnemonic(), "%s: the temporary seed survived", path);

    /* And nothing is deriving from it. While the vault is open that means a
     * stored wallet has taken over the selection; while it is locked it means
     * the vault answers nothing at all. Both are checked, because the paths
     * out of this mode are of both kinds. */
    WalletStatus st = wallet_get_status();
    if (st.unlocked) {
        CHECK(st.active_wallet_index != 0,
              "%s: the vault is open with no wallet selected - something is "
              "still deriving from a seed nobody named", path);
    } else {
        char leaked[256];
        CHECK(wallet_get_mnemonic(leaked, sizeof(leaked)) != WALLET_OK,
              "%s: a locked device still hands out a seed phrase", path);
        memzero(leaked, sizeof(leaked));
    }
}

static void test_temp_seed_every_clearing_path(void)
{
    printf("== every way out of temporary mode destroys the seed (T69)\n");

    /* 1. The deliberate exit, from the menu. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    CHECK(menu_goto("End Temp Seed"), "no exit on the menu");
    press(BUTTON_ACCEPT);
    check_fully_locked("end temp seed");
    check_temp_seed_gone("end temp seed");

    /* 2. CANCEL on the menu, the other deliberate lock. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    hold_back_to_lock(true);    /* a temporary seed is live: it must ask */
    check_fully_locked("menu lock");
    check_temp_seed_gone("menu lock");

    /* 3. Auto-lock. The one nobody presses, and the one that matters most:
     * here it destroys a seed that exists nowhere else. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    fake_clock_advance_us(31 * 60 * 1000000LL);
    CHECK(ui__check_autolock_for_test(), "the device did not auto-lock");
    idle_pump();
    check_fully_locked("auto-lock");
    check_temp_seed_gone("auto-lock");

    /* 4. A host-requested lock. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    ui_request_lock();
    ui__service_host_lock_for_test();
    idle_pump();
    check_temp_seed_gone("host lock");

    /* 5. Switching to a stored wallet. Not a lock, so the device stays
     * unlocked - which is exactly why the temporary seed has to go: two seeds
     * live at once is the ambiguous state this feature refuses to have. */
    boot_unlocked_with_seed();
    CHECK(fake_wallet_preload(PHRASE_12) == 2, "setup: could not preload a second seed");
    enter_temporary_seed();
    CHECK(wallet_select_wallet(1) == WALLET_OK, "could not switch wallets");
    check_temp_seed_gone("wallet switch");
    CHECK(wallet_get_status().active_wallet_index == 1, "the switch did not take effect");

    /* 6. A wipe. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    CHECK(wallet_wipe() == WALLET_OK, "the wipe failed");
    check_temp_seed_gone("wipe");
}

/* The longer default is this mode's, and only this mode's. A stored wallet's
 * timeout is the user's setting, before and after. */
static void test_temp_seed_autolock_is_scoped_to_the_mode(void)
{
    printf("== temporary mode lengthens its own auto-lock, and nothing else's (T69)\n");
    boot_unlocked_with_seed();

    const int before = ui__lock_timeout_choice_for_test();
    CHECK(before == 1, "the stored default is not 5 min (choice %d)", before);

    enter_temporary_seed();
    CHECK(ui__lock_timeout_choice_for_test() == 3,
          "temporary mode did not take the 30 min default (choice %d)",
          ui__lock_timeout_choice_for_test());
    CHECK(ui__lock_timeout_stored_choice_for_test() == 1,
          "the stored preference was overwritten - a stored wallet would now "
          "sit unlocked for 30 minutes too");

    /* Five idle minutes no longer lock it, which is the point of the change. */
    fake_clock_advance_us(6 * 60 * 1000000LL);
    CHECK(!ui__check_autolock_for_test(),
          "the temporary session locked at the stored wallet's timeout");
    CHECK(wallet_has_temporary_mnemonic(), "the seed was dropped anyway");

    /* And once it does lock, the user's own choice is what is back in force. */
    fake_clock_advance_us(31 * 60 * 1000000LL);
    CHECK(ui__check_autolock_for_test(), "the device never auto-locked");
    idle_pump();
    CHECK(ui__lock_timeout_choice_for_test() == 1,
          "the 30 min timeout outlived the temporary seed (choice %d)",
          ui__lock_timeout_choice_for_test());

    /* The user is still free to choose mid-session, and their choice sticks
     * rather than being reverted by the next lock. */
    boot_unlocked_with_seed();
    enter_temporary_seed();
    /* The row reads "Lock 30 min" - the label carries the value, which is
     * what makes the override visible in Settings as well. */
    settings_goto("Lock 30 min");
    press(BUTTON_ACCEPT);
    const int picked = ui__lock_timeout_choice_for_test();
    CHECK(picked == 0, "cycling from 30 min did not wrap to 1 min (choice %d)", picked);
    CHECK(ui__lock_timeout_stored_choice_for_test() == picked,
          "a choice made during a temporary session was not stored");
}

/* The return path's screen. Nothing shows a UR on it until Zcash signing
 * lands, but the machinery is kept and checked: the frames advance from the
 * task loop, not from a button, so a missing ui_invalidate() would freeze the
 * animation on part 1 and every test that calls ui_render() directly would
 * still pass. */
static void test_qr_out_animates_and_leaves_cleanly(void)
{
    printf("== an animated UR steps on its timer and BACK clears it\n");
    boot_unlocked_with_seed();
    go(SCREEN_MAIN_MENU);

    uint8_t msg[90];
    for (size_t i = 0; i < sizeof msg; i++) {
        msg[i] = (uint8_t)i;
    }
    CHECK(ui_show_ur("bytes", msg, sizeof msg, SCREEN_MAIN_MENU),
          "a 90-byte message could not be shown");
    idle_pump();
    CHECK(ui_get_screen() == SCREEN_QR_OUT, "the QR screen did not open");

    char first[400];
    snprintf(first, sizeof first, "%s", fake_oled_qr_data());
    CHECK(strncmp(first, "UR:BYTES/1-", 11) == 0,
          "the first frame is not an uppercase part 1: '%.40s'", first);
    /* v3 at scale 2 is the default: the first CAM-board pairing, at v6 x1,
       was never read by a laptop webcam, and the rendered framebuffer shows
       the scale-1 modes need three camera pixels per OLED pixel where v3 x2
       needs two. */
    CHECK(fake_oled_qr_version() == 3 && fake_oled_qr_scale() == 2,
          "the default mode is v%u x%u, not v3 x2",
          fake_oled_qr_version(), fake_oled_qr_scale());
    /* Every QR opens at Dim, whatever the panel is set to: at High a phone
       camera saw rolling lines across the panel. */
    CHECK(fake_oled_contrast() == 0x04,
          "a QR opened at contrast 0x%02x, not Dim 0x04", fake_oled_contrast());
    /* OK steps this QR's brightness, dimmest to brightest. */
    press(BUTTON_ACCEPT);
    CHECK(ui_get_screen() == SCREEN_QR_OUT, "OK left the QR screen");
    CHECK(fake_oled_contrast() == 0x10, "OK after Dim gave 0x%02x, not Low",
          fake_oled_contrast());
    press(BUTTON_ACCEPT);
    CHECK(fake_oled_contrast() == 0x50, "OK after Low gave 0x%02x, not Mid",
          fake_oled_contrast());
    press(BUTTON_ACCEPT);
    CHECK(fake_oled_contrast() == 0xA0, "OK after Mid gave 0x%02x, not High",
          fake_oled_contrast());

    /* Nothing pressed, time passes: the next part appears. */
    fake_clock_advance_us(400000);
    ui__service_qr_out_for_test();
    idle_pump();
    CHECK(strncmp(fake_oled_qr_data(), "UR:BYTES/2-", 11) == 0,
          "the animation did not advance on its own: '%.40s'", fake_oled_qr_data());

    CHECK(!fake_oled_qr_inverted(), "the default QR is drawn inverted");

    /* DOWN steps through the table and starts each sequence over: from the
       default, the same v3 x2 inverted, then v6 inverted, then it wraps to
       v6 and on to v10. */
    press(BUTTON_DOWN);
    CHECK(fake_oled_qr_version() == 3 && fake_oled_qr_scale() == 2 &&
          fake_oled_qr_inverted(), "DOWN did not switch to v3 x2 inverted");
    press(BUTTON_DOWN);
    CHECK(fake_oled_qr_version() == 6 && fake_oled_qr_inverted(),
          "DOWN did not switch to v6 inverted");
    press(BUTTON_DOWN);
    CHECK(fake_oled_qr_version() == 6 && !fake_oled_qr_inverted(),
          "DOWN did not wrap to plain v6");
    press(BUTTON_DOWN);
    CHECK(fake_oled_qr_version() == 10, "DOWN did not switch to version 10");
    CHECK(strncmp(fake_oled_qr_data(), "UR:BYTES/", 9) == 0,
          "version 10 frame is not a UR");

    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "BACK did not return where it was told");
    /* The QR was left at High; the user's own Dim must come back. */
    CHECK(fake_oled_contrast() == 0x04,
          "leaving the QR left the panel at 0x%02x, not the user's 0x04",
          fake_oled_contrast());
    fake_clock_advance_us(10000000);
    ui__service_qr_out_for_test();
    CHECK(!ui_needs_render(), "the animation kept running after the screen closed");

    uint8_t big[QR_OUT_MAX_MESSAGE + 1] = {0};
    CHECK(!ui_show_ur("bytes", big, sizeof big, SCREEN_MAIN_MENU),
          "an oversized message was accepted");
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "a refused UR still changed screen");
}

static void scan_it(const char *type, const uint8_t *cbor, size_t len)
{
    static UrEncoder e;
    ur_encoder_init(&e, type, cbor, len, 12);
    for (uint32_t i = 0; i < e.seq_len + 20; i++) {
        char part[200];
        ur_encoder_next_part(&e, part, sizeof part);
        ur_to_upper(part);
        ui__scan_feed_for_test(part);
        if (ui_get_screen() != SCREEN_SCAN ||
            strstr(ui__scan_status_for_test(), "parts to go") == NULL) {
            break;
        }
    }
}

/* The scanner keeps assembling, and refuses every complete UR until a Zcash
 * type exists (airgap.h). Nothing scanned may move the screen. */
static void test_scan_assembles_then_refuses_every_type(void)
{
    printf("== the scanner assembles a UR and refuses it, for every type\n");
    boot_unlocked_with_seed();
    go(SCREEN_SCAN);
    CHECK(fake_oled_contains("Scan request"), "the scan screen did not draw");

    /* Not a UR at all. */
    ui__scan_feed_for_test("HELLO");
    CHECK(strcmp(ui__scan_status_for_test(), "Not a UR part") == 0,
          "a non-UR read as \"%s\"", ui__scan_status_for_test());

    /* A multi-part message reports progress before it completes. */
    uint8_t big[300];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)(i * 7);
    }
    static UrEncoder e;
    ur_encoder_init(&e, "bytes", big, sizeof big, 40);
    CHECK(e.seq_len > 1, "setup: the message is not multi-part");
    char part[200];
    ur_encoder_next_part(&e, part, sizeof part);
    ur_to_upper(part);
    ui__scan_feed_for_test(part);
    CHECK(strstr(ui__scan_status_for_test(), "parts to go") != NULL,
          "a first part did not report progress (\"%s\")", ui__scan_status_for_test());

    /* Complete, and refused. */
    go(SCREEN_SCAN);
    scan_it("bytes", big, sizeof big);
    idle_pump();
    CHECK(ui_get_screen() == SCREEN_SCAN, "a complete UR moved the screen");
    CHECK(strcmp(ui__scan_status_for_test(), "Not a Zcash request") == 0,
          "a complete UR read as \"%s\"", ui__scan_status_for_test());

    /* The sign-request type LeekWallet accepted gets no special treatment. */
    const uint8_t req[] = { 0xA1, 0x01, 0x41, 0x02 };
    scan_it("eth-sign-request", req, sizeof req);
    idle_pump();
    CHECK(ui_get_screen() == SCREEN_SCAN && fake_oled_contains("Not a Zcash request"),
          "an old sign request was not refused like any other UR");

    press(BUTTON_CANCEL);
    CHECK(ui_get_screen() == SCREEN_MAIN_MENU, "BACK did not leave the scanner");
}

int main(void)
{
    test_the_seed_fingerprint_is_shown();
    test_receive_shows_the_whole_address();
    test_sd_card_save_and_load();
    test_address_check();
    test_wrong_network_notice();
    test_sd_card_signing();
    test_qr_airgap_signing();
    test_scan_assembles_then_refuses_every_type();
    test_qr_out_animates_and_leaves_cleanly();
    test_blind_signing_takes_a_deliberate_act();
    test_locking_closes_the_channel();
    test_an_expired_approval_leaves_the_screen();
    test_host_passphrase_confirmation();
    test_zsign_review_walks_every_page();
    test_harness_sees_the_screen();

    test_import_word_count_prompt();
    test_import_back_from_first_character();
    test_import_reaches_word_24();
    test_import_rejects_bad_checksum();

    test_seed_survives_the_display_verify_handoff();
    test_seed_survives_the_user_path_into_verification();
    test_seed_is_zeroed_on_leaving_the_flow();

    test_pin_explicit_submit();
    test_pin_wrong_spends_an_attempt();
    test_pin_digits_do_not_outlive_the_screen();

    test_failed_derivation_is_not_shown_as_an_identifier();
    test_passphrase_confirmation_needs_a_fingerprint();

    test_settings_has_no_wifi_entry();

    test_settings_selects_one_transport();
    test_entropy_accept_only_proceeds();
    test_entropy_cancel_abandons();
    test_dice_counts_are_arithmetic();
    test_dice_roll_never_creates_a_wallet();
    test_dice_and_taps_compose();
    test_xfp_is_shown_on_the_wallet_screen();
    test_xfp_tells_passphrase_wallets_apart();
    test_xfp_is_absent_when_it_cannot_be_derived();
    test_entry_style_drives_both_selectors();
    test_change_pin_is_reachable_and_works();
    test_change_pin_rejects_a_wrong_current_pin();
    test_change_pin_catches_a_mismatch();

    test_seed_generation_happens_behind_its_own_frame();
    test_leaving_the_create_screen_cancels_the_generation();

    test_autolock_drops_the_passphrase();
    test_switching_wallets_drops_the_passphrase();
    test_a_passphrase_change_under_the_wallet_screen_re_derives();

    test_the_account_is_selectable_and_bounded();
    test_the_wallet_screen_names_the_account();
    test_the_account_survives_a_reboot();
    test_a_wipe_resets_the_account();

    test_a_tap_on_back_does_not_lock();
    test_the_hold_shows_progress();
    test_a_hold_with_nothing_to_lose_locks_outright();
    test_the_confirmation_names_what_is_lost();
    test_the_menu_lock_actually_locks();
    test_the_host_lock_actually_locks();
    test_the_autolock_timeout_locks();
    test_show_seed_relocks_the_vault_too();

    test_temp_seed_warns_before_it_is_used();
    test_temp_seed_is_visible_on_screen();
    test_temp_seed_does_not_become_a_stored_wallet();
    test_temp_seed_every_clearing_path();
    test_temp_seed_autolock_is_scoped_to_the_mode();
    test_advertising_window_reopens_on_a_press();

    /* ---------------------------------------------------------------------
     * Seed creation talks to nothing.
     *
     * A host that cannot reach the device cannot observe or steer the seed,
     * and -- less obviously -- nothing else is drawing from the entropy gate
     * while the seed is drawn, which matters because bootloader_random_enable()
     * is not reference-counted and the protocol task exists whether or not a
     * link is selected.
     *
     * Asserted through the endpoint's own state rather than a flag of its own, so
     * the test fails if the suspend stops actually silencing the endpoint,
     * not merely if somebody renames something.
     */
    printf("== seed creation runs with both links down\n");
    {
        transport_set(TRANSPORT_USB);
        CHECK(fake_protocol_rx_enabled(), "USB should answer before seed creation");

        go(SCREEN_ENTROPY);
        CHECK(!fake_protocol_rx_enabled(),
              "the USB endpoint kept answering during seed creation");

        /* Every way out of the flow goes through the main menu, which is where
         * the link is restored -- including this one, cancelling. */
        go(SCREEN_MAIN_MENU);
        CHECK(fake_protocol_rx_enabled(), "the link was not restored after leaving");

        /* Suspending twice, and resuming what was never suspended, are both
         * no-ops: the resume sits on a screen reachable without ever having
         * created a wallet. */
        go(SCREEN_MAIN_MENU);
        CHECK(fake_protocol_rx_enabled(), "a second resume disturbed the link");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
