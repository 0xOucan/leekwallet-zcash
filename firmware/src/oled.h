/**
 * LeekWallet OLED Driver
 * SSD1306 128x64 I2C display driver
 */

#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Display dimensions */
#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_PAGES          (OLED_HEIGHT / 8)

/* Characters per line with 6-pixel width font */
#define OLED_CHARS_PER_LINE (OLED_WIDTH / 6)

/**
 * Initialize I2C bus for OLED communication
 * @return ESP_OK on success
 */
esp_err_t oled_i2c_init(void);

/**
 * Initialize SSD1306 display
 * Must call oled_i2c_init() first
 * @return ESP_OK on success
 */
esp_err_t oled_init(void);

/**
 * Clear entire display
 * @return ESP_OK on success
 */
esp_err_t oled_clear(void);

/**
 * Push the frame buffer to the panel.
 *
 * Drawing composes into RAM; nothing reaches the display until this is called.
 * One I2C transfer per frame instead of one per character, which is the
 * difference between a screen that updates and a screen that flickers.
 */
esp_err_t oled_flush(void);

/** Blank the panel immediately, bypassing the buffer. For shutdown paths. */
esp_err_t oled_clear_panel_now(void);

/** Panel contrast, 0x00 to 0xFF. The SSD1306/SSD1315 "brightness" control. */
esp_err_t oled_set_contrast(uint8_t level);

/**
 * Refresh the panel as fast as the controller allows, or back to normal.
 *
 * The SSD1306 lights one row at a time. At its default ~88 Hz a camera with
 * a short exposure catches only part of each sweep, and the rows not lit in
 * that window come out as dark bands across a QR. Fast refresh (maximum
 * oscillator, shortest pre-charge) is about 156 Hz, so the same exposure
 * spans whole sweeps. For QR screens; normal refresh everywhere else keeps
 * the panel's stock drive.
 */
esp_err_t oled_set_fast_refresh(bool fast);

/**
 * Clear a single page (8-pixel row)
 * @param page Page number (0-7)
 * @return ESP_OK on success
 */
esp_err_t oled_clear_page(uint8_t page);

/**
 * Draw a single character at current cursor position
 * @param c Character to draw (ASCII 32-126)
 * @return ESP_OK on success
 */
esp_err_t oled_draw_char(char c);

/**
 * Draw a string at specified position
 * @param page Page number (0-7)
 * @param col Column position (0-127)
 * @param str Null-terminated string to draw
 * @return ESP_OK on success
 */
esp_err_t oled_draw_string(uint8_t page, uint8_t col, const char *str);

/**
 * Draw a string centered on a page
 * @param page Page number (0-7)
 * @param str Null-terminated string to draw
 * @return ESP_OK on success
 */
esp_err_t oled_draw_string_centered(uint8_t page, const char *str);

/**
 * Invert or un-invert a page (8-pixel row)
 * @param page Page number (0-7)
 * @param invert true to invert, false to normal
 * @return ESP_OK on success
 */
esp_err_t oled_invert_page(uint8_t page, bool invert);

/**
 * Set cursor position for subsequent drawing
 * @param page Page number (0-7)
 * @param col Column position (0-127)
 * @return ESP_OK on success
 */
esp_err_t oled_set_cursor(uint8_t page, uint8_t col);

/**
 * Draw raw data to display
 * @param data Pixel data bytes
 * @param len Number of bytes
 * @return ESP_OK on success
 */
esp_err_t oled_draw_raw(const uint8_t *data, size_t len);

/**
 * Copy one page of column bytes INTO the framebuffer, to be shown by the next
 * flush.
 *
 * Unlike oled_draw_raw, which sends to the panel immediately. The viewfinder
 * used that and flickered hard on the bench: the preview reached the glass,
 * and then the frame's ordinary flush wrote the framebuffer - which had never
 * seen the preview - straight over it. Every frame drew the image and then
 * erased it.
 */
void oled_blit_page(uint8_t page, const uint8_t *cols, size_t len);

/**
 * Fill a page with a pattern (useful for selection highlight)
 * @param page Page number (0-7)
 * @param pattern Byte pattern to fill
 * @return ESP_OK on success
 */
esp_err_t oled_fill_page(uint8_t page, uint8_t pattern);

/**
 * Draw a QR code centered on the display
 * @param data Data string to encode
 * @return ESP_OK on success
 */
esp_err_t oled_draw_qrcode(const char *data);

/**
 * The largest QR version the 64-row panel can show, at scale 1:
 * (10 * 4 + 17) modules plus a 2 px quiet zone each side is 61 rows.
 */
#define OLED_QR_MAX_VERSION 10

/** True if `version` at `scale` (1 or 2) fits 64 rows with its quiet zone. */
bool oled_qr_fits(uint8_t version, uint8_t scale);

/**
 * Draw a QR code at exactly this version and scale, centred.
 *
 * For the QR return path, where the caller sized its parts for one version and
 * must not have the renderer pick another. ESP_ERR_INVALID_ARG if the pair does
 * not fit the panel; ESP_ERR_INVALID_SIZE if the data does not fit the version.
 * Pass an uppercased UR so the encoder reaches alphanumeric mode.
 */
esp_err_t oled_draw_qrcode_at(const char *data, uint8_t version, uint8_t scale);

/** As oled_draw_qrcode_at, optionally with lit modules on a dark background. */
esp_err_t oled_draw_qrcode_ex(const char *data, uint8_t version, uint8_t scale,
                              bool inverted);

/** True once a panel answered at init; false on a headless board. */
bool oled_present(void);

/**
 * Rows the attached panel has: 128 on the SH1107 screen kit, 240 on an
 * ST7789 (panel-color.h), 64 on the SSD1306.
 */
uint8_t oled_panel_rows(void);

/**
 * Show a full-panel 128x128 picture (page format, 2048 bytes) at the next
 * flush, instead of the frame, until the next oled_clear(). The tall
 * panel's counterpart of the QR path (panel128.h), for the boot splash. The
 * pointer is kept, not copied, so it must outlive the picture. Ignored on a
 * 64-row panel: check oled_panel_rows() first.
 */
void oled_show_panel_picture(const uint8_t *picture);

/*
 * A colour layer for that picture, for a colour panel (panel-color.h): 128x128,
 * two bits a pixel, row-major, 32 bytes a row, the leftmost pixel in the top
 * two bits of its byte. Each pixel says which colour a lit pixel of the
 * picture takes, or, for OLED_TINT_HALO, that an unlit one is faintly lit.
 * Kept by pointer, like the picture, until the next oled_clear().
 *
 * Defined only where BOARD_HAS_SPI_PANEL (board.h): a monochrome build has
 * nothing to show it on, so calling it there fails to link.
 */
#define OLED_TINT_BYTES (128 * 128 / 4)
#define OLED_TINT_ROW   (128 / 4)
enum {
    OLED_TINT_PICTURE = 0,   /* as the picture has it */
    OLED_TINT_KEY     = 1,   /* the logo's key, glowing */
    OLED_TINT_SPARK   = 2,   /* a sparkle */
    OLED_TINT_HALO    = 3,   /* unlit in the picture, lit by the key's halo */
};
void oled_show_panel_tint(const uint8_t *tint);

/*
 * A grayscale picture, one byte a pixel, row-major, drawn 1:1 in panel pixels
 * at (x, y) over the frame's text rows at the next flush, until the next
 * oled_clear(): the colour kit's camera viewfinder (viewfinder.h). Kept by
 * pointer. Rows it covers should hold no text; it is drawn over them.
 *
 * Colour panels only, like the tint; the host's fake panel records it.
 */
void oled_show_panel_gray(const uint8_t *px, uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h);

/*
 * The colour panel's backlight, 10..100 percent (clamped), as LEDC PWM on
 * the module's BLK pin where the board wires one (PIN_DISPLAY_BLK). A board
 * with BLK tied to 3V3 accepts it and nothing changes. Colour panels only;
 * the OLEDs dim through oled_set_contrast().
 */
#define OLED_BACKLIGHT_MIN  10
#define OLED_BACKLIGHT_MAX  100
esp_err_t oled_set_backlight(uint8_t pct);

/**
 * What text means, for a panel that can show it in colour.
 *
 * A screen names a meaning, never a colour; the panel's palette decides
 * (panel-color.h). Most text never asks and is OLED_ROLE_AUTO, which a colour
 * panel resolves from where the text sits: row 0 a title, row 7 the hint
 * footer, a row starting with '>' the selected item. Monochrome panels ignore
 * roles entirely, so setting one changes nothing on an OLED.
 */
typedef enum {
    OLED_ROLE_AUTO = 0,
    OLED_ROLE_TEXT,      /* body text */
    OLED_ROLE_DIM,       /* secondary: hints, units, captions */
    OLED_ROLE_ACCENT,    /* titles, the brand colour */
    OLED_ROLE_WARN,      /* look before you continue */
    OLED_ROLE_OK,        /* done, approved */
    OLED_ROLE_DANGER,    /* destructive, refused, failed */
    OLED_ROLE_GOOD,      /* a reading in range: the website's bright green */
    OLED_ROLE_COUNT
} oled_role_t;

/**
 * The role for text drawn from now until the next oled_set_role() or
 * oled_clear(), which goes back to OLED_ROLE_AUTO. Callers that set one for a
 * line set it back to AUTO after it.
 */
void oled_set_role(oled_role_t role);

/**
 * Set a single pixel on the display
 * @param x X coordinate (0-127)
 * @param y Y coordinate (0-63)
 * @param on true for pixel on, false for off
 * @return ESP_OK on success
 */
esp_err_t oled_set_pixel(uint8_t x, uint8_t y, bool on);

/**
 * Refresh the display from framebuffer
 * @return ESP_OK on success
 */
esp_err_t oled_refresh(void);

#endif /* OLED_H */
