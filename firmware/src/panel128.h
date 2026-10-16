/**
 * A 128x128 panel (SH1107) showing the UI's 128x64 frame (roadmap: S3CAM
 * screen kit).
 *
 * The whole UI is laid out for 128x64, and every screen and suite agrees on
 * that, so the tall panel shows that frame unchanged in its middle eight
 * pages. QR codes are the exception that matters: on 64 rows a unified
 * address is a 1-pixel-per-module code a phone struggles with. When the
 * frame is a QR (oled-core remembers the last one drawn), it is redrawn here
 * across the full 128 rows at the largest whole scale that fits with a
 * two-module quiet zone - a UA at 2 px per module, a t-address at 3.
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

/* Where the 64-row frame sits: pages 4-11. */
#define PANEL128_UI_FIRST_PAGE 4

/** Module size for a `modules`-wide code on the panel; 0 if it cannot fit. */
uint8_t panel128_qr_scale(uint8_t modules);

/**
 * Compose the panel image. `qr` non-NULL: the code, full panel, lit quiet
 * zone on a dark panel (or lit modules on dark when `inverted`), as oled-core
 * draws it. Otherwise the frame, centred.
 */
void panel128_compose(const uint8_t *frame, const QRCode *qr, bool inverted,
                      uint8_t out[PANEL128_BYTES]);

#endif /* LEEK_PANEL128_H */
