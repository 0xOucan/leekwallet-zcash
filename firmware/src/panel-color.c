/**
 * The UI's frame on a colour panel. See panel-color.h.
 */
#include "panel-color.h"

#include <string.h>

#include "oled-core.h"

/*
 * The palette. Leek greens from the logo (docs/leekwallet-logo.svg: #7ABF6A,
 * #5A8F45, #3F6E31) on a background just off black, with the logo's cream for
 * body text: white on black is harsher than it needs to be on an IPS panel at
 * full backlight, and cream keeps the contrast well over 12:1.
 *
 * The warning colours are chosen to be told apart from green by somebody who
 * cannot tell red from green: amber for "look at this" and a red that is also
 * much darker than the greens, so WARN and DANGER differ in brightness as
 * well as hue. Neither depends on colour alone anyway; the words that carry
 * them ("!Has public output", "Wrong network") say the same thing in mono.
 *
 * The boot splash is the exception to the leek greens: it is the website's
 * logo animation (leekwallet-zcash-web src/game/logo-anim.ts), so it takes
 * that file's colours - GREEN for the pot and lid, KEY for the glowing key,
 * SPARK for the sparkles, and the halo, SPARK laid over the background at
 * the halo's alpha (SPLASH_HALO_ALPHA, 90 of 255, at full glow).
 */
#define PANEL_BG_R 0x08
#define PANEL_BG_G 0x0B
#define PANEL_BG_B 0x08
#define HALO_ALPHA 90   /* gen_splash.py HALO_ALPHA, SPLASH_HALO_ALPHA */
#define OVER(src, dst) ((dst) + ((src) - (dst)) * HALO_ALPHA / 255)
static const PanelPalette LEEK_PALETTE = {
    .bg        = PANEL_RGB565(PANEL_BG_R, PANEL_BG_G, PANEL_BG_B),
    .fg = {
        [OLED_ROLE_AUTO]   = PANEL_RGB565(0xEE, 0xE8, 0xD6),
        [OLED_ROLE_TEXT]   = PANEL_RGB565(0xEE, 0xE8, 0xD6),   /* logo cream, lifted */
        [OLED_ROLE_DIM]    = PANEL_RGB565(0x8E, 0x9B, 0x88),
        [OLED_ROLE_ACCENT] = PANEL_RGB565(0x7A, 0xBF, 0x6A),   /* leek green */
        [OLED_ROLE_WARN]   = PANEL_RGB565(0xFF, 0xB3, 0x40),
        [OLED_ROLE_OK]     = PANEL_RGB565(0x9C, 0xE0, 0x7A),
        [OLED_ROLE_DANGER] = PANEL_RGB565(0xF2, 0x4C, 0x3D),
        [OLED_ROLE_GOOD]   = PANEL_RGB565(57, 255, 122),       /* #39FF7A, logo-anim.ts GREEN */
    },
    .select_bg = PANEL_RGB565(0x3F, 0x6E, 0x31),               /* the logo's dark leek */
    .select_fg = PANEL_RGB565(0xFF, 0xFF, 0xFF),
    .rule      = PANEL_RGB565(0x3F, 0x6E, 0x31),
    .qr_dark   = PANEL_RGB565(0x00, 0x00, 0x00),
    .qr_light  = PANEL_RGB565(0xFF, 0xFF, 0xFF),
    .picture       = PANEL_RGB565(57, 255, 122),     /* logo-anim.ts GREEN */
    .picture_key   = PANEL_RGB565(236, 255, 240),    /* KEY */
    .picture_spark = PANEL_RGB565(255, 236, 150),    /* SPARK */
    .picture_halo  = PANEL_RGB565(OVER(255, PANEL_BG_R), OVER(236, PANEL_BG_G),
                                  OVER(150, PANEL_BG_B)),
    .gray = {
#define GRAY1(i) PANEL_RGB565((i) * 255 / 63, (i) * 255 / 63, (i) * 255 / 63)
#define GRAY8(i) GRAY1(i), GRAY1((i) + 1), GRAY1((i) + 2), GRAY1((i) + 3), \
                 GRAY1((i) + 4), GRAY1((i) + 5), GRAY1((i) + 6), GRAY1((i) + 7)
        GRAY8(0), GRAY8(8), GRAY8(16), GRAY8(24),
        GRAY8(32), GRAY8(40), GRAY8(48), GRAY8(56),
#undef GRAY8
#undef GRAY1
    },
};

const PanelPalette *panel_color_palette(void) { return &LEEK_PALETTE; }

/* ------------------------------------------------------------------ QR */

/*
 * The largest whole number of panel pixels per module that leaves at least
 * two modules of quiet zone each side. ISO 18004 asks for four; the whole
 * panel around the code is drawn light (render_qr), so the quiet zone is
 * everything up to the glass's edge, and it is four modules or more wherever
 * the scale leaves that much room anyway. Insisting on four before choosing
 * the scale cost a whole pixel per module for nothing: a version 3 code was
 * 6 px a module where 7 fits with 2.6 modules of margin (board, 2026-11-10:
 * "we are not using the full 240x240"). Two is the floor because below it
 * the bezel's dark edge reaches the finder patterns.
 */
uint8_t panel_color_qr_scale(uint8_t modules, int width, int height)
{
    const int side = width < height ? width : height;
    if (modules == 0) {
        return 0;
    }
    const int s = side / (modules + 2 * PANEL_QR_MIN_QUIET);
    return (uint8_t)(s < 1 ? 0 : (s > 255 ? 255 : s));
}

int panel_color_qr_fill_side(uint8_t modules, int width, int height)
{
    const int side = width < height ? width : height;
    if (modules == 0) {
        return 0;
    }
    /* Whole multiples of `modules` would be the plain layout again; any side
       will do, as each module edge is rounded on its own. */
    return side * modules / (modules + 2 * PANEL_QR_FILL_QUIET);
}

/* The caption's glyph size under a code at `scale`: the largest of 2x3 (the
 * UI's own), 2x2 and 1x1 panel pixels per font pixel that fits between the
 * minimum quiet zone and the bottom of the panel, and across it; 0 if none
 * does. A caption never shrinks the code: it is laid out after the scale. */
static void caption_fit(const char *text, uint8_t scale, int px, int width, int height,
                        int *xs, int *ys)
{
    static const uint8_t SIZES[][2] = { { 2, 3 }, { 2, 2 }, { 1, 1 } };
    *xs = *ys = 0;
    if (text == NULL || !text[0]) {
        return;
    }
    const int oy = (height - px) / 2;
    const int room = height - (oy + px) - PANEL_QR_MIN_QUIET * scale;
    const int n = (int)strlen(text);
    for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
        if (7 * SIZES[i][1] <= room && n * 6 * SIZES[i][0] - SIZES[i][0] <= width) {
            *xs = SIZES[i][0];
            *ys = SIZES[i][1];
            return;
        }
    }
}

bool panel_color_qr_caption_rows(const char *text, uint8_t modules, int width, int height,
                                 int *y0, int *rows)
{
    const uint8_t scale = panel_color_qr_scale(modules, width, height);
    int xs, ys;
    caption_fit(text, scale, modules * scale, width, height, &xs, &ys);
    if (!xs) {
        return false;
    }
    const int px = modules * scale;
    const int top = (height - px) / 2 + px + PANEL_QR_MIN_QUIET * scale;
    if (y0) *y0 = top + (height - top - 7 * ys) / 2;
    if (rows) *rows = 7 * ys;
    return true;
}

/*
 * The filled layout (PanelFrame.qr_fill): the code `side` pixels square,
 * module m from m * side / n to (m + 1) * side / n, so modules are the
 * floor or the ceiling of side / n pixels and the code ends exactly at
 * `side`. One module of quiet zone each side and no caption.
 */
static void render_qr_fill(const QRCode *qr, bool inverted, const PanelPalette *pal,
                           int width, int height, int y0, int rows, uint16_t *out)
{
    const int n = qr->size;
    const int side = panel_color_qr_fill_side((uint8_t)n, width, height);
    const int ox = (width - side) / 2;
    const int oy = (height - side) / 2;
    const uint16_t lit = inverted ? pal->qr_light : pal->qr_dark;
    const uint16_t field = inverted ? pal->qr_dark : pal->qr_light;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *row = out + (size_t)r * width;
        for (int x = 0; x < width; x++) {
            row[x] = field;
        }
        if (y < oy || y >= oy + side) {
            continue;
        }
        /* The module whose span [m*side/n, (m+1)*side/n) holds this row:
           the largest m with m*side/n <= y - oy. */
        const int m = ((y - oy + 1) * n - 1) / side;
        for (int mx = 0; mx < n; mx++) {
            if (!qrcode_getModule((QRCode *)qr, (uint8_t)mx, (uint8_t)m)) continue;
            const int xa = ox + mx * side / n;
            const int xb = ox + (mx + 1) * side / n;
            for (int x = xa; x < xb; x++) row[x] = lit;
        }
    }
}

static void render_qr(const QRCode *qr, bool inverted, uint8_t scale, const char *caption,
                      const PanelPalette *pal, int width, int height,
                      int y0, int rows, uint16_t *out)
{
    const int px = qr->size * scale;
    const int ox = (width - px) / 2;
    const int oy = (height - px) / 2;
    /*
     * Dark modules on light, the light running to every edge of the panel:
     * the whole glass is the code's quiet zone, so a camera finds a light
     * square with the code in it and nothing else (the OLEDs frame theirs in
     * dark because their lit area glared; here the backlight is turned down
     * for every QR instead, ui.c). Inverted: lit modules on dark, as the user
     * chose it.
     */
    const uint16_t lit = inverted ? pal->qr_light : pal->qr_dark;
    const uint16_t field = inverted ? pal->qr_dark : pal->qr_light;

    /* The caption, if one fits under the code. */
    int cxs, cys;
    caption_fit(caption, scale, px, width, height, &cxs, &cys);
    int cy0 = 0, cx0 = 0, clen = 0;
    if (cxs) {
        panel_color_qr_caption_rows(caption, qr->size, width, height, &cy0, NULL);
        clen = (int)strlen(caption);
        cx0 = (width - (clen * 6 * cxs - cxs)) / 2;
    }

    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *row = out + (size_t)r * width;
        const int my = (y - oy) / scale;
        const bool in_code_y = y >= oy && my < qr->size;
        for (int x = 0; x < width; x++) {
            row[x] = field;
        }
        if (in_code_y) {
            for (int mx = 0; mx < qr->size; mx++) {
                if (qrcode_getModule((QRCode *)qr, (uint8_t)mx, (uint8_t)my)) {
                    uint16_t *p = row + ox + mx * scale;
                    for (int k = 0; k < scale; k++) p[k] = lit;
                }
            }
        }
        if (cxs && y >= cy0 && y < cy0 + 7 * cys) {
            const int bit = (y - cy0) / cys;
            for (int c = 0; c < clen; c++) {
                const uint8_t *cols = font_5x7_glyph(caption[c]);
                for (int gx = 0; gx < 5; gx++) {
                    if (!((cols[gx] >> bit) & 1)) continue;
                    const int x = cx0 + (c * 6 + gx) * cxs;
                    for (int k = 0; k < cxs; k++) {
                        if (x + k >= 0 && x + k < width) row[x + k] = lit;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------- picture */

static void render_picture(const uint8_t *pic, const uint8_t *tint, const PanelPalette *pal,
                           int width, int height, int y0, int rows, uint16_t *out)
{
    /* Nearest-neighbour from 128x128. A picture is the one thing with no
       glyphs to redraw, and the splash is a logo, not text. The colour
       layer, when there is one, picks each pixel's colour (oled.h). */
    const uint16_t lit[4] = {
        [OLED_TINT_PICTURE] = pal->picture, [OLED_TINT_KEY] = pal->picture_key,
        [OLED_TINT_SPARK] = pal->picture_spark, [OLED_TINT_HALO] = pal->picture,
    };
    for (int r = 0; r < rows; r++) {
        const int ly = (y0 + r) * 128 / height;
        uint16_t *row = out + (size_t)r * width;
        const uint8_t *trow = tint ? tint + (size_t)ly * OLED_TINT_ROW : NULL;
        for (int x = 0; x < width; x++) {
            const int lx = x * 128 / width;
            const bool on = (pic[(ly / 8) * 128 + lx] >> (ly % 8)) & 1;
            const unsigned t = trow ? (trow[lx / 4] >> (6 - 2 * (lx % 4))) & 3u : OLED_TINT_PICTURE;
            if (on) {
                row[x] = lit[t];
            } else {
                row[x] = t == OLED_TINT_HALO ? pal->picture_halo : pal->bg;
            }
        }
    }
}

/* --------------------------------------------------------------- text */

/* The geometry of the eight rows on this panel. */
typedef struct {
    int pitch;     /* panel rows per frame row (page) */
    int ys;        /* panel rows per frame pixel row, and per glyph pixel */
    int margin;    /* leading above the first pixel row of a page */
    int xs;        /* panel columns per glyph pixel */
} Layout;

static Layout layout_for(int width, int height)
{
    Layout l;
    l.pitch = height / OLED_PAGES;
    l.ys = l.pitch / 8;
    if (l.ys < 1) l.ys = 1;
    l.margin = (l.pitch - 8 * l.ys) / 2;
    if (l.margin < 0) l.margin = 0;
    /* A 6-column character cell is 6 * width / 128 panel columns wide; the
       glyph is 5 of those columns plus at least one of gap. */
    l.xs = (6 * width / OLED_WIDTH - 1) / 5;
    if (l.xs < 1) l.xs = 1;
    return l;
}

/* First panel column that resamples to frame column `c`. */
static int col_x(int c, int width) { return (c * width + OLED_WIDTH - 1) / OLED_WIDTH; }

static bool page_selected(const PanelFrame *f, int page)
{
    return f->glyphs[page * OLED_WIDTH] == '>';
}

static bool page_has_glyphs(const PanelFrame *f, int page)
{
    for (int c = 0; c < OLED_WIDTH; c++) {
        if (f->glyphs[page * OLED_WIDTH + c]) return true;
    }
    return false;
}

static bool page_blank(const PanelFrame *f, int page)
{
    for (int c = 0; c < OLED_WIDTH; c++) {
        if (f->frame[page * OLED_WIDTH + c]) return false;
    }
    return true;
}

/* What AUTO means on this row. */
static uint8_t auto_role(int page)
{
    if (page == 0) return OLED_ROLE_ACCENT;               /* the title */
    if (page == OLED_PAGES - 1) return OLED_ROLE_DIM;     /* the hint footer */
    return OLED_ROLE_TEXT;
}

static uint16_t role_color(const PanelPalette *pal, uint8_t role, int page, bool selected)
{
    if (role == OLED_ROLE_AUTO || role >= OLED_ROLE_COUNT) {
        if (selected) return pal->select_fg;
        role = auto_role(page);
    }
    return pal->fg[role];
}

static void render_text(const PanelFrame *f, const PanelPalette *pal,
                        int width, int height, int y0, int rows, uint16_t *out)
{
    const Layout l = layout_for(width, height);
    /* A ruled title: a title on row 0 with nothing on row 1, which is how
       nearly every screen opens. Asked once per band, not per pixel. */
    const bool rule = page_has_glyphs(f, 0) && page_blank(f, 1);

    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *row = out + (size_t)r * width;
        const int page = y / l.pitch;
        if (page >= OLED_PAGES) {
            for (int x = 0; x < width; x++) row[x] = pal->bg;
            continue;
        }
        const bool selected = page_selected(f, page);
        const uint16_t bg = selected ? pal->select_bg : pal->bg;
        const int off = y - page * l.pitch - l.margin;

        if (off < 0 || off >= 8 * l.ys) {
            /* Leading between rows: background, the selection bar running
               through it so the bar is one block, and the title's rule in
               the leading under row 0. */
            const bool is_rule = rule && page == 0 && y == l.pitch - 2;
            for (int x = 0; x < width; x++) {
                row[x] = (is_rule && x >= 6 && x < width - 6) ? pal->rule : bg;
            }
            continue;
        }

        const int bit = off / l.ys;   /* pixel row within the page, 0..7 */
        const uint8_t *fpage = f->frame + (size_t)page * OLED_WIDTH;
        const uint16_t base_fg = role_color(pal, OLED_ROLE_AUTO, page, selected);

        /* Everything, resampled from the frame. */
        for (int x = 0; x < width; x++) {
            const int lx = x * OLED_WIDTH / width;
            row[x] = ((fpage[lx] >> bit) & 1) ? base_fg : bg;
        }

        /* Then every glyph again, crisp, over its own cell. */
        const uint8_t *g = f->glyphs + (size_t)page * OLED_WIDTH;
        const uint8_t *ro = f->roles + (size_t)page * OLED_WIDTH;
        for (int c = 0; c < OLED_WIDTH; c++) {
            if (!g[c]) continue;
            const uint8_t *cols = font_5x7_glyph((char)g[c]);
            const uint16_t fg = role_color(pal, ro[c], page, selected);
            const int x0 = col_x(c, width);
            for (int gx = 0; gx < 5; gx++) {
                const uint16_t px = ((cols[gx] >> bit) & 1) ? fg : bg;
                for (int k = 0; k < l.xs; k++) {
                    const int x = x0 + gx * l.xs + k;
                    if (x < width) row[x] = px;
                }
            }
        }
    }
}

/* ---------------------------------------------------------- viewfinder */

/* The camera's preview, 1:1, over whatever the text pass drew there. Clipped
 * to the panel and to this band, so a rectangle that hangs off either is
 * drawn as far as it fits. */
static void render_gray(const PanelFrame *f, const PanelPalette *pal,
                        int width, int height, int y0, int rows, uint16_t *out)
{
    const int x0 = f->gray_x < 0 ? 0 : f->gray_x;
    int x1 = f->gray_x + f->gray_w;
    if (x1 > width) x1 = width;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        if (y < f->gray_y || y >= f->gray_y + f->gray_h || y >= height) continue;
        const uint8_t *src = f->gray + (size_t)(y - f->gray_y) * f->gray_w;
        uint16_t *row = out + (size_t)r * width;
        for (int x = x0; x < x1; x++) {
            row[x] = pal->gray[src[x - f->gray_x] >> 2];
        }
    }
}

void panel_color_render(const PanelFrame *f, const PanelPalette *pal,
                        int width, int height, int y0, int rows, uint16_t *out)
{
    if (f->picture) {
        render_picture(f->picture, f->tint, pal, width, height, y0, rows, out);
        return;
    }
    const uint8_t scale = f->qr ? panel_color_qr_scale(f->qr->size, width, height) : 0;
    if (scale && f->qr_fill) {
        render_qr_fill(f->qr, f->qr_inverted, pal, width, height, y0, rows, out);
        return;
    }
    if (scale) {
        render_qr(f->qr, f->qr_inverted, scale, f->qr_caption, pal, width, height, y0, rows, out);
        return;
    }
    render_text(f, pal, width, height, y0, rows, out);
    if (f->gray && f->gray_w > 0 && f->gray_h > 0) {
        render_gray(f, pal, width, height, y0, rows, out);
    }
}
