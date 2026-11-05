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
 */
static const PanelPalette LEEK_PALETTE = {
    .bg        = PANEL_RGB565(0x08, 0x0B, 0x08),
    .fg = {
        [OLED_ROLE_AUTO]   = PANEL_RGB565(0xEE, 0xE8, 0xD6),
        [OLED_ROLE_TEXT]   = PANEL_RGB565(0xEE, 0xE8, 0xD6),   /* logo cream, lifted */
        [OLED_ROLE_DIM]    = PANEL_RGB565(0x8E, 0x9B, 0x88),
        [OLED_ROLE_ACCENT] = PANEL_RGB565(0x7A, 0xBF, 0x6A),   /* leek green */
        [OLED_ROLE_WARN]   = PANEL_RGB565(0xFF, 0xB3, 0x40),
        [OLED_ROLE_OK]     = PANEL_RGB565(0x9C, 0xE0, 0x7A),
        [OLED_ROLE_DANGER] = PANEL_RGB565(0xF2, 0x4C, 0x3D),
    },
    .select_bg = PANEL_RGB565(0x3F, 0x6E, 0x31),               /* the logo's dark leek */
    .select_fg = PANEL_RGB565(0xFF, 0xFF, 0xFF),
    .rule      = PANEL_RGB565(0x3F, 0x6E, 0x31),
    .qr_dark   = PANEL_RGB565(0x00, 0x00, 0x00),
    .qr_light  = PANEL_RGB565(0xFF, 0xFF, 0xFF),
    .picture   = PANEL_RGB565(0x7A, 0xBF, 0x6A),
};

const PanelPalette *panel_color_palette(void) { return &LEEK_PALETTE; }

/* ------------------------------------------------------------------ QR */

/* ISO 18004 asks for four modules of quiet zone. Two is the fallback for a
 * code that would otherwise not fit at all, which on 240 rows means nothing
 * this firmware draws. */
uint8_t panel_color_qr_scale(uint8_t modules, int width, int height)
{
    const int side = width < height ? width : height;
    for (int quiet = 4; quiet >= 2; quiet -= 2) {
        const int s = side / (modules + 2 * quiet);
        if (s >= 1) {
            return (uint8_t)(s > 255 ? 255 : s);
        }
    }
    return 0;
}

static void render_qr(const QRCode *qr, bool inverted, uint8_t scale,
                      const PanelPalette *pal, int width, int height,
                      int y0, int rows, uint16_t *out)
{
    const int px = qr->size * scale;
    const int ox = (width - px) / 2;
    const int oy = (height - px) / 2;
    const int quiet = 4 * scale;
    /*
     * Normal polarity: dark modules on a lit square that runs to the edge of
     * the quiet zone, the rest of the panel dark - the same FRAMED look as
     * the OLEDs (oled-core.c), which costs an LCD nothing and keeps one
     * picture of "the QR screen" across boards. Inverted: lit modules on dark,
     * as the user chose it.
     */
    const uint16_t lit = inverted ? pal->qr_light : pal->qr_dark;
    const uint16_t field = inverted ? pal->qr_dark : pal->qr_light;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *row = out + (size_t)r * width;
        const bool in_frame = !inverted && y >= oy - quiet && y < oy + px + quiet;
        const int my = (y - oy) / scale;
        const bool in_code_y = y >= oy && my < qr->size;
        for (int x = 0; x < width; x++) {
            uint16_t c = pal->qr_dark;
            if (in_frame && x >= ox - quiet && x < ox + px + quiet) {
                c = field;
            }
            if (in_code_y && x >= ox && x < ox + px &&
                qrcode_getModule((QRCode *)qr, (uint8_t)((x - ox) / scale), (uint8_t)my)) {
                c = lit;
            }
            row[x] = c;
        }
    }
}

/* ------------------------------------------------------------- picture */

static void render_picture(const uint8_t *pic, const PanelPalette *pal,
                           int width, int height, int y0, int rows, uint16_t *out)
{
    /* Nearest-neighbour from 128x128. A picture is the one thing with no
       glyphs to redraw, and the splash is a logo, not text. */
    for (int r = 0; r < rows; r++) {
        const int ly = (y0 + r) * 128 / height;
        uint16_t *row = out + (size_t)r * width;
        for (int x = 0; x < width; x++) {
            const int lx = x * 128 / width;
            const bool on = (pic[(ly / 8) * 128 + lx] >> (ly % 8)) & 1;
            row[x] = on ? pal->picture : pal->bg;
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

void panel_color_render(const PanelFrame *f, const PanelPalette *pal,
                        int width, int height, int y0, int rows, uint16_t *out)
{
    if (f->picture) {
        render_picture(f->picture, pal, width, height, y0, rows, out);
        return;
    }
    const uint8_t scale = f->qr ? panel_color_qr_scale(f->qr->size, width, height) : 0;
    if (scale) {
        render_qr(f->qr, f->qr_inverted, scale, pal, width, height, y0, rows, out);
        return;
    }
    render_text(f, pal, width, height, y0, rows, out);
}
