/**
 * The UI's frame on a colour panel (roadmap: S3CAM colour kit, Firefly Pixie).
 *
 * ---------------------------------------------------------------------------
 * One UI, every panel
 *
 * Every screen is laid out once, for the 128x64 frame in oled-core.c: eight
 * rows of 21 characters. The OLEDs show that frame as it is (128x64) or
 * stretched (panel128.h). A 240x240 TFT is neither: 240/128 is 1.875, so a
 * plain resample gives every glyph column one or two pixels at random and a
 * font that looks chewed.
 *
 * So text is not resampled. oled-core keeps, beside the bits, which character
 * started at each column of each row and which colour role it was drawn in
 * (oled_core_glyphs()). This file lays the same eight rows out across the
 * panel and redraws every glyph from the font at an exact integer scale - 2x3
 * on 240x240, each glyph pixel a 2-wide, 3-tall block - at the position its
 * column maps to. Everything that is not a glyph (the camera preview, a
 * stray pixel) is resampled from the frame as before. Rows are spaced over
 * the full height, so the 21-column layouts fill the glass instead of
 * floating in a letterbox.
 *
 * The same two exceptions as on the tall OLED, for the same reasons:
 *
 *   - a QR code is redrawn from its modules at the largest whole scale that
 *     keeps a two-module quiet zone, square and black on white, the white
 *     running to the panel's edges, so a phone reads it from across a desk;
 *     a short caption (oled_set_qr_caption()) goes under it where it fits
 *     without shrinking the modules;
 *   - a full-panel picture (the boot splash) is shown on its own.
 *
 * ---------------------------------------------------------------------------
 * Colour without touching the layouts
 *
 * A screen asks for a meaning, not a colour: oled_set_role(OLED_ROLE_WARN)
 * (oled.h). Most text never asks, and gets OLED_ROLE_AUTO, which this file
 * resolves from where the text sits, because the layouts are regular enough
 * to say what a row is for: row 0 is a title, row 7 is the hint footer, and a
 * row that starts with '>' is the selected item. The palette (PanelPalette)
 * then turns roles into colours. The OLED drivers never read any of this, so
 * a monochrome panel stays exactly as it was.
 *
 * Pure, like panel128.c: frame in, pixels out, one band of rows at a time so
 * a 115 KB colour frame never has to exist. Host-tested by rendering screens
 * to images and scanning the QR codes back.
 */
#ifndef LEEK_PANEL_COLOR_H
#define LEEK_PANEL_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#include "oled.h"
#include "qrcode.h"

/** 8-bit-per-channel colour to RGB565. */
#define PANEL_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    uint16_t bg;                     /* the panel behind everything */
    uint16_t fg[OLED_ROLE_COUNT];    /* text, per role; [OLED_ROLE_AUTO] unused */
    uint16_t select_bg;              /* the bar behind the selected row */
    uint16_t select_fg;              /* AUTO text on that bar */
    uint16_t rule;                   /* the line under a title */
    uint16_t qr_dark, qr_light;      /* always black and white: a camera reads them */
    uint16_t picture;                /* lit pixels of a full-panel picture */
    /* The picture's colour layer (oled.h, OLED_TINT_*): the splash's key,
       its sparkles and the halo around the key, as the website draws them. */
    uint16_t picture_key, picture_spark, picture_halo;
    /* 64 grays, black to white, for the camera viewfinder: index v >> 2 of
       an 8-bit level. RGB565 has six bits of green and five of red and blue,
       so 64 is every gray it can show. */
    uint16_t gray[64];
} PanelPalette;

/** LeekWallet's palette: leek green on near-black. Native RGB565. */
const PanelPalette *panel_color_palette(void);

/** What is on screen, as oled-core holds it. */
typedef struct {
    const uint8_t *frame;     /* 128x64, page format */
    const uint8_t *glyphs;    /* [OLED_PAGES][OLED_WIDTH]: character starting there, or 0 */
    const uint8_t *roles;     /* same shape: the oled_role_t it was drawn in */
    const uint8_t *picture;   /* 128x128 page format (splash), or NULL */
    const uint8_t *tint;      /* its colour layer (oled.h), or NULL: all `picture` */
    const QRCode  *qr;        /* last code drawn, or NULL */
    bool           qr_inverted;
    const char    *qr_caption; /* the line under it (oled_set_qr_caption()), or NULL */
    bool           qr_fill;    /* fill the panel, uneven modules (oled_set_qr_fill()) */
    /* A grayscale picture over the text (oled_show_panel_gray()), 1:1 in
       panel pixels at (gray_x, gray_y), or gray NULL. */
    const uint8_t *gray;
    int            gray_x, gray_y, gray_w, gray_h;
} PanelFrame;

/** The least quiet zone a code gets, in modules, each side. */
#define PANEL_QR_MIN_QUIET 2

/** Module size, in panel pixels, of a `modules`-wide code on a width x height
 *  panel: the largest whole size that keeps PANEL_QR_MIN_QUIET; 0 if none fits. */
uint8_t panel_color_qr_scale(uint8_t modules, int width, int height);

/** Quiet zone of a filled code (PanelFrame.qr_fill), in modules, each side. */
#define PANEL_QR_FILL_QUIET 1

/** The side, in panel pixels, of a filled `modules`-wide code: the panel's
 *  smaller side less PANEL_QR_FILL_QUIET modules each side. Module m spans
 *  [m * side / modules, (m + 1) * side / modules) from the code's corner. */
int panel_color_qr_fill_side(uint8_t modules, int width, int height);

/** Where `text` goes under a `modules`-wide code: true and its panel rows if
 *  it fits below the quiet zone at the code's own scale, else false. */
bool panel_color_qr_caption_rows(const char *text, uint8_t modules, int width, int height,
                                 int *y0, int *rows);

/**
 * Render panel rows [y0, y0 + rows) of a width x height panel into `out`,
 * row-major, `width` pixels a row. Colours are copied from `pal` as they are,
 * so a transport whose bus wants the bytes swapped passes a swapped palette.
 */
void panel_color_render(const PanelFrame *f, const PanelPalette *pal,
                        int width, int height, int y0, int rows, uint16_t *out);

#endif /* LEEK_PANEL_COLOR_H */
