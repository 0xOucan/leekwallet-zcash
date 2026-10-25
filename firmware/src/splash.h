/**
 * The boot splash: the LeekWallet logo opening, then who and what it is.
 *
 * The pot from the website opens its lid, the key rises and sparkles, and the
 * logo holds with three lines under it: "@gmLeekWallet", the version and
 * codename, and "the white road", plus a blinking prompt ("Push the knob").
 * It IS the boot screen: it loops until the user asks for the PIN, and the
 * unlock screen falls back to it when left alone. The PIN logic does not
 * know it exists.
 *
 * The frames are a table generated from the website's logo grids by
 * tools/splash/gen_splash.py (src/splash-frames.h), so nothing here depends
 * on a clock or a random number, and the build stays reproducible.
 *
 * Pure: frame index and time in, page-format image out (one byte per column
 * per page, bit 0 on top, as oled-core and panel128 use). Two renderings:
 *
 *   - tall, the full 128x128 SH1107 panel: the logo at full size, the text
 *     below it. Shown through the panel's picture path (oled.h), not the
 *     stretched 128x64 frame, so the logo keeps its proportions.
 *   - wide, the 128x64 frame every other panel shows: the logo at half
 *     size on the left, the text on the right.
 */
#ifndef LEEK_SPLASH_H
#define LEEK_SPLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "version.h"

#define SPLASH_LINE_1 "@gmLeekWallet"
#define SPLASH_LINE_2 "v" LEEK_FW_VERSION " " LEEK_FW_CODENAME
#define SPLASH_LINE_3 "the white road"

#define SPLASH_TALL_BYTES (128 * 128 / 8)
#define SPLASH_WIDE_BYTES (128 * 64 / 8)

/* ---------------------------------------------------------------- looping
 *
 * The boot screen plays the splash over and over: the animation (every frame
 * but the last, at its own pace), then the last frame, logo and text, held
 * for SPLASH_LOOP_HOLD_MS with the prompt blinking under it, then again.
 * Left alone for SPLASH_BLANK_AFTER_MS it goes black (OLED burn-in, and an
 * unlit OLED draws next to nothing); the next input only wakes it.
 *
 * Input, as splash_loop_input() decides it:
 *   - while blank: any input wakes it, restarts the loop and is spent there.
 *     Never a dismissal, not even OK: the user cannot see what they press.
 *   - OK (the knob press, K4) dismisses it, on every kit.
 *   - any other input: with `ok_only` (the knob kit, where the prompt says
 *     "Push the knob"), a turn or Cancel does not dismiss; it jumps to the
 *     text and prompt so a user fiddling with the knob is told what to do.
 *     Without it (four buttons, "Press any key") every key dismisses.
 *   Every input counts as activity for the blanking timer.
 *
 * Times are milliseconds on any monotonic clock (ui.c passes esp_timer's);
 * nothing is read from a clock here.
 */
#define SPLASH_LOOP_HOLD_MS     4000u            /* text and prompt, per cycle */
#define SPLASH_PROMPT_BLINK_MS  500u             /* prompt on, then off, ... */
#define SPLASH_BLANK_AFTER_MS   (10u * 60u * 1000u)  /* untouched: go black */

typedef struct {
    int64_t  cycle_start_ms;   /* when the current pass began */
    int64_t  last_input_ms;    /* for the blanking timer */
    bool     blank;            /* sticky until the next input */
} splash_loop_t;

/* What the panel should show at a given instant. Two views that compare
 * equal draw the same pixels, so the UI repaints only when this changes. */
typedef struct {
    bool     blank;    /* all black; frame and prompt are meaningless */
    unsigned frame;
    bool     prompt;   /* the blinking prompt line is lit */
} splash_view_t;

typedef enum {
    SPLASH_INPUT_WOKE,      /* was blank; now playing again. Spent. */
    SPLASH_INPUT_PROMPT,    /* ignored for dismissal; jumped to the prompt */
    SPLASH_INPUT_DISMISS,   /* leave the splash for the PIN */
} splash_input_t;

/** Start (or restart) the loop at `now_ms`, lit, from the first frame. */
void splash_loop_start(splash_loop_t *s, int64_t now_ms);

/** One full pass, animation plus hold, in ms. */
uint32_t splash_loop_cycle_ms(void);

/** What to show at `now_ms`. Goes (and stays) blank once the idle time is up. */
splash_view_t splash_loop_view(splash_loop_t *s, int64_t now_ms);

/** Whether two views draw the same thing. */
bool splash_view_equal(splash_view_t a, splash_view_t b);

/** An input at `now_ms`; `is_ok` for the press/accept key. See above. */
splash_input_t splash_loop_input(splash_loop_t *s, int64_t now_ms, bool is_ok, bool ok_only);

/** Ms from `now_ms` until the view next changes on its own (UINT32_MAX: never). */
uint32_t splash_loop_next_change_ms(const splash_loop_t *s, int64_t now_ms);

/* ----------------------------------------------------------------- frames */

/** Frames in the animation; the last is the logo held with the text. */
unsigned splash_frame_count(void);

/** When frame `frame` ends, in ms from the start. The last one's is the total. */
uint32_t splash_frame_end_ms(unsigned frame);

/** The whole splash, in ms. */
uint32_t splash_total_ms(void);

/** The frame due `ms` after the start, or splash_frame_count() once it is over. */
unsigned splash_frame_at(uint32_t ms);

/** Whether frame `frame` shows the three text lines. */
bool splash_frame_has_text(unsigned frame);

/** One pixel of the full-size logo in frame `frame`; false outside it. */
bool splash_logo_pixel(unsigned frame, int x, int y);

/**
 * Frame `frame` for the 128x128 panel. `prompt`, when not NULL, is drawn as a
 * fourth line under the text (text frames only); at most 21 characters.
 */
void splash_render_tall(unsigned frame, const char *prompt, uint8_t out[SPLASH_TALL_BYTES]);

/** Frame `frame` for a 128x64 panel; the prompt goes under the text, in at
 *  most 15 characters (the room beside the logo). */
void splash_render_wide(unsigned frame, const char *prompt, uint8_t out[SPLASH_WIDE_BYTES]);

#endif /* LEEK_SPLASH_H */
