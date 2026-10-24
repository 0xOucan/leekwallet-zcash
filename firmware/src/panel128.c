/**
 * The 128x128 panel image. See panel128.h.
 */
#include "panel128.h"

#include <string.h>

#include "oled.h"

#define QUIET_MODULES 2

uint8_t panel128_qr_scale(uint8_t modules)
{
    for (uint8_t s = 4; s >= 1; s--) {
        if ((unsigned)(modules + 2 * QUIET_MODULES) * s <= PANEL128_SIZE) {
            return s;
        }
    }
    return 0;
}

static void set(uint8_t *out, int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= PANEL128_SIZE || y >= PANEL128_SIZE) return;
    uint8_t *b = &out[(y / 8) * PANEL128_SIZE + x];
    if (on) *b |= (uint8_t)(1u << (y % 8));
    else    *b &= (uint8_t)~(1u << (y % 8));
}

void panel128_compose(const uint8_t *frame, const QRCode *qr, bool inverted,
                      uint8_t out[PANEL128_BYTES])
{
    memset(out, 0, PANEL128_BYTES);
    const uint8_t scale = qr ? panel128_qr_scale(qr->size) : 0;
    if (!qr || scale == 0) {
        /* Panel page p holds frame rows 4p..4p+3: half a frame page, each
         * bit doubled into two adjacent rows. */
        for (int p = 0; p < PANEL128_PAGES; p++) {
            const uint8_t *src = frame + (size_t)(p / 2) * OLED_WIDTH;
            const int shift = (p & 1) * 4;
            for (int x = 0; x < PANEL128_SIZE; x++) {
                const uint8_t n = (uint8_t)((src[x] >> shift) & 0x0F);
                uint8_t b = 0;
                for (int i = 0; i < 4; i++) {
                    if (n & (1u << i)) b |= (uint8_t)(3u << (2 * i));
                }
                out[p * PANEL128_SIZE + x] = b;
            }
        }
        return;
    }

    const int px = qr->size * scale;
    const int origin = (PANEL128_SIZE - px) / 2;
    /* Normal polarity: only the code and its quiet zone are lit, the rest of
     * the panel dark (oled-core's FRAMED style, for the same camera reasons). */
    if (!inverted) {
        const int q = QUIET_MODULES * scale;
        for (int y = origin - q; y < origin + px + q; y++) {
            for (int x = origin - q; x < origin + px + q; x++) set(out, x, y, true);
        }
    }
    for (int my = 0; my < qr->size; my++) {
        for (int mx = 0; mx < qr->size; mx++) {
            if (!qrcode_getModule((QRCode *)qr, (uint8_t)mx, (uint8_t)my)) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    set(out, origin + mx * scale + sx, origin + my * scale + sy, inverted);
                }
            }
        }
    }
}
