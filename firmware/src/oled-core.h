/**
 * The panel-independent half of the display. See oled-core.c.
 *
 * A transport (oled.c for the SSD1306, oled-st7789.c for the ST7789) implements
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

/**
 * The text layer, for a panel that redraws glyphs at its own scale
 * (panel-color.h). Two OLED_PAGES x OLED_WIDTH arrays, row-major by page:
 *
 *   glyphs[page * OLED_WIDTH + col]  the character whose glyph starts at that
 *                                    column, or 0;
 *   roles[...]                       the oled_role_t it was drawn in.
 *
 * Kept in step with the frame: anything that overwrites part of a glyph's
 * cell (another glyph, a pixel, a page fill, a QR) forgets it, so what is
 * left of it is drawn from the frame bits like any other pixel. A character
 * clipped at the right edge is never recorded, for the same reason.
 */
const uint8_t *oled_core_glyphs(void);
const uint8_t *oled_core_roles(void);

/** The picture set by oled_show_panel_picture(), or NULL since a clear. */
const uint8_t *oled_core_panel_picture(void);

/** Its colour layer (oled_show_panel_tint()), or NULL with no picture or
 *  since a clear. Colour panels only (BOARD_HAS_SPI_PANEL). */
const uint8_t *oled_core_panel_tint(void);

/** Five columns of a 5x7 glyph, bit 0 the top row. Unprintables render blank. */
const uint8_t *font_5x7_glyph(char c);

#endif /* LEEK_OLED_CORE_H */
