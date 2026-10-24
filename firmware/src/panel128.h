/**
 * A 128x128 panel (SH1107) showing the UI's 128x64 frame (roadmap: S3CAM
 * screen kit).
 *
 * The whole UI is laid out for 128x64, and every screen and suite agrees on
 * that, so the tall panel shows that frame stretched 2x vertically: each of
 * its 64 rows is drawn twice, filling all 128 rows, widths unchanged. Text
 * comes out as tall 5x14 glyphs that fill the glass instead of a letterboxed
 * band with 32 dark rows above and below. QR codes are the exception that
 * matters: a stretched code is not square and on 64 rows a unified
 * address is a 1-pixel-per-module code a phone struggles with. When the
 * frame is a QR (oled-core remembers the last one drawn), it is redrawn here
 * across the full 128 rows at the largest whole scale that fits with a
 * two-module quiet zone - a UA at 2 px per module, a t-address at 3.
 *
 * The boot splash is the other full-panel image: it draws its own 128x128
 * picture (splash.h) and that is shown as it is, so the logo keeps its
 * proportions rather than being stretched.
 *
 * Pure: frame in, panel image out, in the same page format (one byte per
 * column per page, bit 0 on top). Host-tested by scanning the result.
 */
#ifndef LEEK_PANEL128_H
#define LEEK_PANEL128_H

#include <stdbool.h>
#include <stdint.h>

#include "qrcode.h"

#define PANEL128_SIZE   128
#define PANEL128_PAGES  16
#define PANEL128_BYTES  (PANEL128_SIZE * PANEL128_PAGES)

/* Panel rows per frame row: the 64-row frame is stretched to 128. */
#define PANEL128_UI_STRETCH 2

/** Module size for a `modules`-wide code on the panel; 0 if it cannot fit. */
uint8_t panel128_qr_scale(uint8_t modules);

/**
 * Compose the panel image, in this order of precedence:
 *
 *   - `picture` non-NULL: a full-panel 128x128 image already in page format
 *     (the boot splash, oled_show_panel_picture()), shown as it is;
 *   - `qr` non-NULL: the code, full panel, lit quiet zone on a dark panel (or
 *     lit modules on dark when `inverted`), as oled-core draws it;
 *   - otherwise the frame, each row doubled.
 */
void panel128_compose(const uint8_t *frame, const uint8_t *picture,
                      const QRCode *qr, bool inverted,
                      uint8_t out[PANEL128_BYTES]);

#endif /* LEEK_PANEL128_H */
