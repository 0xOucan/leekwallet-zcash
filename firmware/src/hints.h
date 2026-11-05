/**
 * On-screen input hints, one wording per input kit, chosen at compile time.
 *
 * The footers were written for the four-button module, where each word sits
 * over the key that does it: "UP DN  DEL  SEL" is K1, K2, K3, K4. The S3 CAM
 * kit has a knob and one Cancel button instead, and "UP DN" names keys that
 * are not there. So a board with LEEK_HAS_ENCODER gets its own wording, in a
 * fixed vocabulary:
 *
 *   X:<word>   the Cancel button (K3) does <word>
 *   TURN       turning the knob (UP/DOWN) scrolls or steps
 *   OK:<word>  pressing the knob (K4, accept) does <word>; "OK" alone when
 *              the press is a plain acknowledgement
 *
 * laid out Cancel left, turn middle, press right, the same order the newer
 * 21-column footers (review, receive) already use. Every word says what that
 * input does on that screen, read from its handler, not what the key is
 * called. The four-button boards (reference S3, Pixie) keep their original
 * strings exactly.
 *
 * One rule both ways: a hint fits OLED_CHARS_PER_LINE (21). Checked below at
 * compile time for whichever kit is being built, and on the host for both
 * (sim/test_hints.c).
 */
#ifndef LEEK_HINTS_H
#define LEEK_HINTS_H

#include "board.h"

#if LEEK_HAS_ENCODER

/* PIN pads: turn picks a digit (or OK), Cancel deletes one, press enters it.
 * Also the seed-word keyboard outside a block. */
#define HINT_SCROLL_DEL_SEL   "X:DEL   TURN  OK:SEL"
/* Lists and pickers whose Cancel leaves the screen or closes a block. */
#define HINT_SCROLL_BACK_SEL  "X:BACK  TURN  OK:SEL"
/* Home: Cancel, held, locks (it opens the hold-to-lock screen). */
#define HINT_HOME             "X:LOCK  TURN  OK:SEL"
/* Settings: Cancel returns to the menu. */
#define HINT_SETTINGS         "X:BACK  TURN  OK:SEL"
/* Wallet list. */
#define HINT_WALLET_SELECT    "X:BACK  TURN  OK:SEL"
/* A screen whose only way on is Cancel. */
#define HINT_BACK             "X:BACK"
/* Lock confirmation, after the hold. */
#define HINT_LOCK_CONFIRM     "X:BACK       OK:LOCK"
/* New wallet: turn toggles 12/24, press goes to the entropy screen. */
#define HINT_CREATE           "X:BACK        OK:GEN"
#define HINT_CREATE_LENGTH    "TURN: 12/24"
/* Seed phrase pages. Cancel and turning back go a page back (off the first
 * page, back to the wallet); press and turning on go a page on, and off the
 * last page into the verification. */
#define HINT_SEED_MIDDLE      "X:PREV  TURN OK:NEXT"
#define HINT_SEED_LAST        "X:PREV  TURN OK:DONE"
#define HINT_SEED_FIRST       "X:BACK  TURN OK:NEXT"
#define HINT_SEED_ONLY        "X:BACK       OK:DONE"
/* Seed import: the 12/24 question. */
#define HINT_IMPORT_LENGTH    "X:BACK  TURN  OK:SEL"
/* Seed-word keyboard inside a block: Cancel closes the block. */
#define HINT_WORD_GROUP       "X:BACK  TURN  OK:SEL"
/* Passphrase keyboards (device passphrase, SD card passphrase). Outside a
 * block Cancel deletes a character (leaving when empty); inside one it closes
 * the block; on a block the press opens it. */
#define HINT_TEXT_SEL         "X:DEL   TURN  OK:SEL"
#define HINT_TEXT_OPEN        "X:DEL   TURN OK:OPEN"
#define HINT_TEXT_GROUP       "X:BACK  TURN  OK:SEL"
/* BLE name keyboard, on a block. Its Cancel always deletes. */
#define HINT_BLE_NAME_OPEN    "X:DEL   TURN OK:OPEN"
/* SD card. */
#define HINT_SD_RETRY         "X:BACK      OK:RETRY"
#define HINT_SD_LIST          "X:BACK  TURN  OK:SEL"
/* A notice any press dismisses. */
#define HINT_OK               "                   OK"
/* Entropy: the chooser, the dice (turn picks the face, press commits a roll),
 * and stirring (each detent is a sample; press proceeds, once allowed). */
#define HINT_ENTROPY_CHOOSE   "X:BACK  TURN  OK:SEL"
#define HINT_DICE             "X:BACK  TURN OK:ROLL"
#define HINT_MIX_READY        "X:BACK TURN:MIX OK:GO"
#define HINT_MIX_WAIT         "X:BACK TURN:MIX"
#define HINT_ENTROPY_ITEM     "Turn the knob"
#define HINT_ENTROPY_EVENTS   "turns"
/* Wipe: anything but the press aborts. */
#define HINT_WIPE             "X:BACK       OK:WIPE"
/* Verification passed: any input continues. */
#define HINT_DONE             "             OK:DONE"
/* BLE pairing code: anything but the press denies. */
#define HINT_SESSION          "X:DENY      OK:ALLOW"
/* Passphrase fingerprint: Cancel retypes it, anything else accepts. */
#define HINT_RETRY            "X:RETRY"
#define HINT_RETRY_OK         "X:RETRY            OK"
/* Host passphrase fingerprint. */
#define HINT_NO               "X:NO"
#define HINT_NO_YES           "X:NO          OK:YES"
/* Address check: the UA pages, then the question. */
#define HINT_ADDR_PAGE        "X:NO   TURN   OK:NEXT"
#define HINT_ADDR_ASK         "X:NO          OK:YES"
/* Temporary-seed warning: two pages; the press accepts from either. */
#define HINT_TEMP_FIRST       "X:BACK TURN:more   OK"
#define HINT_TEMP_LAST        "X:BACK TURN:prev   OK"
/* Zcash transaction review (zcash-review.c). */
#define HINT_REVIEW_NEXT      "X:REJECT TURN OK:NEXT"
#define HINT_REVIEW_SIGN      "X:REJECT      OK:SIGN"
/* Receive pages (zcash-receive.c): press flips address and QR. */
#define HINT_RECEIVE          "X:BACK   TURN   OK:QR"
/* Prose rows, not footers. */
#define HINT_PRESS_ANY        "Push the knob"
#define HINT_KEEP_HOLDING     "Keep holding X"

#else /* four buttons: K1 UP, K2 DOWN, K3 CANCEL, K4 ACCEPT */

#define HINT_SCROLL_DEL_SEL   "UP DN  DEL  SEL"
#define HINT_SCROLL_BACK_SEL  "UP DN  BCK  SEL"
#define HINT_HOME             "UP DN       SEL"
#define HINT_SETTINGS         "UP DN       SEL"
#define HINT_WALLET_SELECT    "BCK  UP DN  SEL"
#define HINT_BACK             "BACK"
#define HINT_LOCK_CONFIRM     "BACK        LOCK"
#define HINT_CREATE           "BCK         GEN"
#define HINT_CREATE_LENGTH    "UP/DN: 12/24"
#define HINT_SEED_MIDDLE      "DONE PREV   NEXT"
#define HINT_SEED_LAST        "DONE PREV"
#define HINT_SEED_FIRST       "DONE        NEXT"
#define HINT_SEED_ONLY        "DONE"
#define HINT_IMPORT_LENGTH    "UP DN  BCK  SEL"
#define HINT_WORD_GROUP       "UP DN  BCK  SEL"
#define HINT_TEXT_SEL         "UP DN  BCK  SEL"
#define HINT_TEXT_OPEN        "UP DN  BCK OPEN"
#define HINT_TEXT_GROUP       "UP DN  BCK  SEL"
#define HINT_BLE_NAME_OPEN    "UP DN  BCK  SEL"
#define HINT_SD_RETRY         "BACK        RETRY"
#define HINT_SD_LIST          "BACK  UP/DN     OK"
#define HINT_OK               "                OK"
#define HINT_ENTROPY_CHOOSE   "UP  DN  BCK SEL"
#define HINT_DICE             "+1  -1  BCK ROLL"
#define HINT_MIX_READY        "MIX MIX BCK NEXT"
#define HINT_MIX_WAIT         "MIX MIX BCK ----"
#define HINT_ENTROPY_ITEM     "Tap buttons"
#define HINT_ENTROPY_EVENTS   "taps"
#define HINT_WIPE             "BACK        WIPE"
#define HINT_DONE             "            DONE"
#define HINT_SESSION          "DENY       ALLOW"
#define HINT_RETRY            "RETRY"
#define HINT_RETRY_OK         "RETRY         OK"
#define HINT_NO               "NO"
#define HINT_NO_YES           "NO           YES"
#define HINT_ADDR_PAGE        "NO    UP/DN    NEXT"
#define HINT_ADDR_ASK         "NO               YES"
#define HINT_TEMP_FIRST       "DN:more BCK  SEL"
#define HINT_TEMP_LAST        "UP:back BCK  SEL"
#define HINT_REVIEW_NEXT      "REJECT   ^v      NEXT"
#define HINT_REVIEW_SIGN      "REJECT           SIGN"
#define HINT_RECEIVE          "BACK    UP/DN    QR"
#define HINT_PRESS_ANY        "Press any key"
#define HINT_KEEP_HOLDING     "Keep holding BACK"

#endif

/* Show Seed, before it locks the device to re-ask the PIN: the same two
 * choices as the lock confirmation, so the same words on every board. */
#define HINT_SHOW_SEED        HINT_LOCK_CONFIRM

/* Every hint in one list, for the length check here and the host test. */
#define HINT_FOR_EACH(X) \
    X(HINT_SCROLL_DEL_SEL) X(HINT_SCROLL_BACK_SEL) X(HINT_HOME) X(HINT_SETTINGS) \
    X(HINT_WALLET_SELECT) X(HINT_BACK) X(HINT_LOCK_CONFIRM) X(HINT_SHOW_SEED) X(HINT_CREATE) \
    X(HINT_CREATE_LENGTH) X(HINT_SEED_MIDDLE) X(HINT_SEED_LAST) X(HINT_SEED_FIRST) \
    X(HINT_SEED_ONLY) X(HINT_IMPORT_LENGTH) X(HINT_WORD_GROUP) X(HINT_TEXT_SEL) \
    X(HINT_TEXT_OPEN) X(HINT_TEXT_GROUP) X(HINT_BLE_NAME_OPEN) X(HINT_SD_RETRY) \
    X(HINT_SD_LIST) X(HINT_OK) X(HINT_ENTROPY_CHOOSE) X(HINT_DICE) \
    X(HINT_MIX_READY) X(HINT_MIX_WAIT) X(HINT_ENTROPY_ITEM) X(HINT_ENTROPY_EVENTS) \
    X(HINT_WIPE) X(HINT_DONE) X(HINT_SESSION) X(HINT_RETRY) X(HINT_RETRY_OK) \
    X(HINT_NO) X(HINT_NO_YES) X(HINT_ADDR_PAGE) X(HINT_ADDR_ASK) \
    X(HINT_TEMP_FIRST) X(HINT_TEMP_LAST) X(HINT_REVIEW_NEXT) X(HINT_REVIEW_SIGN) \
    X(HINT_RECEIVE) X(HINT_PRESS_ANY) X(HINT_KEEP_HOLDING)

/* 21 = OLED_CHARS_PER_LINE (128 / 6), spelled out so this header needs no
 * panel driver. The entropy item is drawn after a two-column marker. */
#define HINT_FITS_(h) _Static_assert(sizeof(h) - 1 <= 21, "hint wider than the panel: " h);
HINT_FOR_EACH(HINT_FITS_)
#undef HINT_FITS_
_Static_assert(sizeof(HINT_ENTROPY_ITEM) - 1 <= 21 - 2, "entropy item too wide");

#endif /* LEEK_HINTS_H */
