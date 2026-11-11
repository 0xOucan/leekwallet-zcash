/**
 * Framebuffer OLED for host tests - the display half of T0.3.
 *
 * Two views of the same draw calls:
 *
 *   Text  - an 8x21 character grid, laid out at the same page/column the
 *           firmware asked for. This is the view tests assert on. "Row 3 says
 *           the recipient address" is the kind of claim that catches a real
 *           bug, and it survives a font change or a two-pixel nudge, which a
 *           pixel golden does not.
 *
 *   Pixels - the 128x64 bit buffer, for the handful of things that genuinely
 *           are pixels. Text is not rasterised into it: inventing a font here
 *           would only produce goldens that assert the fake is consistent with
 *           itself.
 */

#ifndef FAKE_OLED_H
#define FAKE_OLED_H

#include "oled.h"
#include <stdbool.h>
#include <stdint.h>

#define FAKE_OLED_ROWS  8
#define FAKE_OLED_COLS  21   /* 128 px / 6 px per character */

/** Clear both views and the counters. Call between tests. */
void fake_oled_reset(void);

/** Text drawn on `page` since the last clear, trailing blanks removed. */
const char *fake_oled_row(int page);

/** Substring search within one row. */
bool fake_oled_row_contains(int page, const char *needle);

/** The role text cell `cell` (0..20) of row `page` was drawn in. */
oled_role_t fake_oled_role_at(int page, int cell);

/** Substring search across every row. */
bool fake_oled_contains(const char *needle);

/** Row holding `needle`, or -1. Lets a test assert on placement, not just presence. */
int fake_oled_find_row(const char *needle);

/** The panel's height as oled_panel_rows() reports it (default 64; 128 is the SH1107). */
void fake_oled_set_panel_rows(uint8_t rows);
/** The picture oled_show_panel_picture() set, or NULL since a clear. */
const uint8_t *fake_oled_panel_picture(void);
/** Flushes made while a full-panel picture was up. */
int fake_oled_picture_flush_count(void);

/** The grayscale picture oled_show_panel_gray() set, and where, or NULL
 *  since a clear. Any out-pointer may be NULL. */
const uint8_t *fake_oled_panel_gray(uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h);
/** The last oled_set_backlight_duty() duty (BL_DUTY_MAX after a reset), and
 *  how many calls set it. */
uint16_t fake_oled_backlight_duty(void);
/** The caption oled_set_qr_caption() set, "" if none since a clear. */
const char *fake_oled_qr_caption(void);
/** What oled_set_qr_fill() asked for since the last clear. */
bool fake_oled_qr_fill(void);
int fake_oled_backlight_calls(void);

/** Payload of the last oled_draw_qrcode(), or "" if none. */
const char *fake_oled_qr_data(void);

/** Version and scale of the last oled_draw_qrcode_at(), 0 if none. */
uint8_t fake_oled_qr_version(void);
uint8_t fake_oled_qr_scale(void);

/** One pixel of the 128x64 frame buffer (not the full-panel picture). */
bool fake_oled_pixel(int x, int y);

/** Times the frame was pushed to the (imaginary) panel. */
int fake_oled_flush_count(void);

/** Last contrast level written. */
uint8_t fake_oled_contrast(void);

/** Whether the last QR was drawn inverted (lit modules, dark background). */
bool fake_oled_qr_inverted(void);

/** Print the character grid, boxed. For eyeballing a failure. */
void fake_oled_dump(void);

/** Print the bit buffer as ASCII art. Secondary - see the header comment. */
void fake_oled_dump_pixels(void);

#endif /* FAKE_OLED_H */
