/**
 * The panel-independent half of the display. See oled-core.c.
 *
 * A transport (oled.c for the SSD1306, oled-pixie.c for the ST7789) implements
 * bus bring-up, flush, contrast and physical clear; everything else is here and
 * is shared, so the two boards cannot drift into showing different layouts.
 */

#ifndef LEEK_OLED_CORE_H
#define LEEK_OLED_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "qrcode.h"

/**
 * The frame, one bit per pixel, in SSD1306 page order: one byte per column per
 * page, bit 0 the top row. A transport reads this and does whatever its panel
 * needs — send it verbatim, or expand it to RGB565.
 */
uint8_t *oled_core_framebuffer(void);

/**
 * The QR code most recently drawn into the frame, or NULL if the frame has
 * been cleared since. For a taller panel that redraws codes at its own
 * scale; `inverted` (optional) is the polarity it was drawn with.
 */
const QRCode *oled_core_last_qr(bool *inverted);

/** Five columns of a 5x7 glyph, bit 0 the top row. Unprintables render blank. */
const uint8_t *font_5x7_glyph(char c);

#endif /* LEEK_OLED_CORE_H */
