/**
 * The boot splash. See splash.h.
 */
#include "splash.h"

#include <string.h>

#include "oled-core.h"
#include "splash-frames.h"

/* Tall panel: the logo centred near the top, the three lines beneath it. */
#define TALL_LOGO_X   ((128 - SPLASH_LOGO_W) / 2)
#define TALL_LOGO_Y   4
#define TALL_TEXT_Y   (TALL_LOGO_Y + SPLASH_LOGO_H + 8)
#define TALL_LINE_GAP 14

/* Wide panel: the logo at half size on the left, the text to its right. */
#define HALF_W        ((SPLASH_LOGO_W + 1) / 2)
#define HALF_H        ((SPLASH_LOGO_H + 1) / 2)
#define WIDE_LOGO_X   2
#define WIDE_LOGO_Y   ((64 - HALF_H) / 2)
#define WIDE_TEXT_X   (WIDE_LOGO_X + HALF_W + 4)
#define WIDE_TEXT_Y   14
#define WIDE_LINE_GAP 14

_Static_assert(TALL_TEXT_Y + 2 * TALL_LINE_GAP + 7 <= 128, "splash text runs off the tall panel");
_Static_assert(WIDE_TEXT_Y + 2 * WIDE_LINE_GAP + 7 <= 64, "splash text runs off the wide panel");
_Static_assert(HALF_H <= 64, "half-size logo taller than the frame");

static const char *const LINES[3] = { SPLASH_LINE_1, SPLASH_LINE_2, SPLASH_LINE_3 };

unsigned splash_frame_count(void)
{
    return SPLASH_FRAMES;
}

uint32_t splash_frame_end_ms(unsigned frame)
{
    uint32_t t = 0;
    for (unsigned i = 0; i < SPLASH_FRAMES && i <= frame; i++) {
        t += SPLASH_FRAME_MS[i];
    }
    return t;
}

uint32_t splash_total_ms(void)
{
    return splash_frame_end_ms(SPLASH_FRAMES - 1);
}

unsigned splash_frame_at(uint32_t ms)
{
    for (unsigned i = 0; i < SPLASH_FRAMES; i++) {
        if (ms < splash_frame_end_ms(i)) {
            return i;
        }
    }
    return SPLASH_FRAMES;
}

bool splash_frame_has_text(unsigned frame)
{
    return frame == SPLASH_FRAMES - 1;
}

bool splash_logo_pixel(unsigned frame, int x, int y)
{
    if (frame >= SPLASH_FRAMES || x < 0 || y < 0 || x >= SPLASH_LOGO_W || y >= SPLASH_LOGO_H) {
        return false;
    }
    const uint8_t b = SPLASH_LOGO[frame][y * SPLASH_LOGO_STRIDE + x / 8];
    return (b & (0x80u >> (x % 8))) != 0;
}

/* Half size: a 2x2 block is lit when at least two of its pixels are, which
 * keeps the rim and the fret band without thickening the thin strokes. */
static bool half_pixel(unsigned frame, int x, int y)
{
    const int n = splash_logo_pixel(frame, 2 * x, 2 * y) + splash_logo_pixel(frame, 2 * x + 1, 2 * y) +
                  splash_logo_pixel(frame, 2 * x, 2 * y + 1) + splash_logo_pixel(frame, 2 * x + 1, 2 * y + 1);
    return n >= 2;
}

static void set(uint8_t *out, int rows, int x, int y)
{
    if (x < 0 || y < 0 || x >= 128 || y >= rows) {
        return;
    }
    out[(y / 8) * 128 + x] |= (uint8_t)(1u << (y % 8));
}

static void text(uint8_t *out, int rows, int x, int y, const char *s)
{
    for (; *s; s++, x += 6) {
        const uint8_t *g = font_5x7_glyph(*s);
        for (int c = 0; c < 5; c++) {
            for (int r = 0; r < 7; r++) {
                if (g[c] & (1u << r)) {
                    set(out, rows, x + c, y + r);
                }
            }
        }
    }
}

static int text_width(const char *s)
{
    const int n = (int)strlen(s);
    return n ? n * 6 - 1 : 0;
}

void splash_render_tall(unsigned frame, uint8_t out[SPLASH_TALL_BYTES])
{
    memset(out, 0, SPLASH_TALL_BYTES);
    for (int y = 0; y < SPLASH_LOGO_H; y++) {
        for (int x = 0; x < SPLASH_LOGO_W; x++) {
            if (splash_logo_pixel(frame, x, y)) {
                set(out, 128, TALL_LOGO_X + x, TALL_LOGO_Y + y);
            }
        }
    }
    if (splash_frame_has_text(frame)) {
        for (int i = 0; i < 3; i++) {
            text(out, 128, (128 - text_width(LINES[i])) / 2, TALL_TEXT_Y + i * TALL_LINE_GAP, LINES[i]);
        }
    }
}

void splash_render_wide(unsigned frame, uint8_t out[SPLASH_WIDE_BYTES])
{
    memset(out, 0, SPLASH_WIDE_BYTES);
    for (int y = 0; y < HALF_H; y++) {
        for (int x = 0; x < HALF_W; x++) {
            if (half_pixel(frame, x, y)) {
                set(out, 64, WIDE_LOGO_X + x, WIDE_LOGO_Y + y);
            }
        }
    }
    if (splash_frame_has_text(frame)) {
        const int room = 128 - WIDE_TEXT_X;
        for (int i = 0; i < 3; i++) {
            text(out, 64, WIDE_TEXT_X + (room - text_width(LINES[i])) / 2,
                 WIDE_TEXT_Y + i * WIDE_LINE_GAP, LINES[i]);
        }
    }
}
