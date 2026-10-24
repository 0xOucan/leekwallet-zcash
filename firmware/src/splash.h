/**
 * The boot splash: the LeekWallet logo opening, then who and what it is.
 *
 * The pot from the website opens its lid, the key rises and sparkles, and the
 * logo holds with three lines under it: "@gmLeekWallet", the version and
 * codename, and "the white road". About 2.4 s in all, and any input skips
 * it. It runs before the boot screen and changes nothing after it: the PIN
 * logic does not know it exists.
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

/** Frame `frame` for the 128x128 panel. */
void splash_render_tall(unsigned frame, uint8_t out[SPLASH_TALL_BYTES]);

/** Frame `frame` for a 128x64 panel. */
void splash_render_wide(unsigned frame, uint8_t out[SPLASH_WIDE_BYTES]);

#endif /* LEEK_SPLASH_H */
