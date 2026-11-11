/* See viewfinder.h. */

#include "viewfinder.h"

#include <stdio.h>
#include <string.h>

void viewfinder_render(const uint8_t *frame, uint16_t fw, uint16_t fh,
                       uint8_t *out)
{
    if (frame == NULL || out == NULL || fw == 0 || fh == 0) {
        return;
    }

    memset(out, 0, VIEWFINDER_BYTES);

    /*
     * Keep the camera's aspect ratio. Stretching 320x240 across 128x56 made a
     * square QR look like a wide rectangle, which is not merely ugly: the
     * preview exists so someone can judge whether the code fills the frame,
     * and a distorted preview answers that question wrongly. The image is
     * letterboxed instead - full height, its proper width, centred - and the
     * bars beside it stay dark.
     */
    uint16_t vw = (uint16_t)((uint32_t)VIEWFINDER_H * fw / fh);
    if (vw > VIEWFINDER_W) {
        vw = VIEWFINDER_W;
    }
    const uint16_t vx0 = (uint16_t)((VIEWFINDER_W - vw) / 2);

    /*
     * Two passes over the sampled pixels rather than one pass into a temporary
     * image. The temporary would be 7 KB of internal RAM held for the life of
     * the scan screen; the second pass costs 7168 more array reads, which is
     * noise beside the quirc decode that runs on the same frame. Memory on
     * this device is the scarcer of the two.
     */
    uint8_t lo = 0xFF, hi = 0x00;
    for (uint16_t y = 0; y < VIEWFINDER_H; y++) {
        const uint32_t sy = (uint32_t)y * fh / VIEWFINDER_H;
        const uint8_t *row = frame + sy * (uint32_t)fw;
        for (uint16_t x = 0; x < vw; x++) {
            const uint8_t v = row[(uint32_t)x * fw / vw];
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
    }

    /*
     * A frame with no contrast at all - a lens cap, a white wall - has lo ==
     * hi. Leaving the panel blank there is the honest picture: there is
     * nothing in view to aim at.
     */
    if (lo == hi) {
        return;
    }
    const uint8_t threshold = (uint8_t)(((uint16_t)lo + hi) / 2);

    for (uint16_t y = 0; y < VIEWFINDER_H; y++) {
        const uint32_t sy = (uint32_t)y * fh / VIEWFINDER_H;
        const uint8_t *row = frame + sy * (uint32_t)fw;
        uint8_t *page = out + (y / 8) * VIEWFINDER_W;
        const uint8_t bit = (uint8_t)(1u << (y % 8));
        for (uint16_t x = 0; x < vw; x++) {
            if (row[(uint32_t)x * fw / vw] > threshold) {
                page[vx0 + x] |= bit;
            }
        }
    }
}

/* ------------------------------------------------------------ grayscale */

static uint8_t clamp_u8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

static uint8_t gray_rotation;   /* quarter turns counter-clockwise, 0..3 */

void viewfinder_set_rotation(uint8_t quarter_turns_ccw)
{
    gray_rotation = (uint8_t)(quarter_turns_ccw & 3);
}

void viewfinder_render_gray(const uint8_t *frame, uint16_t fw, uint16_t fh,
                            uint8_t *out, ViewfinderStats *st,
                            bool true_exposure)
{
    if (frame == NULL || out == NULL || st == NULL || fw == 0 || fh == 0) {
        return;
    }

    /* The picture as shown: the frame turned by gray_rotation. A quarter turn
       swaps its width and height, so a 4:3 frame becomes 3:4. */
    const uint8_t rot = gray_rotation;
    const uint16_t lw = (rot & 1) ? fh : fw;
    const uint16_t lh = (rot & 1) ? fw : fh;

    /* Letterboxed to that aspect ratio, in whichever axis it needs. Unrotated,
       every mode this camera has is 4:3, so the image is the whole 200x150;
       turned a quarter, it is 112x150 with bars at the sides. */
    uint16_t vw = VIEWFINDER_GRAY_W;
    uint16_t vh = (uint16_t)((uint32_t)VIEWFINDER_GRAY_W * lh / lw);
    if (vh > VIEWFINDER_GRAY_H) {
        vh = VIEWFINDER_GRAY_H;
        vw = (uint16_t)((uint32_t)VIEWFINDER_GRAY_H * lw / lh);
        if (vw > VIEWFINDER_GRAY_W) { vw = VIEWFINDER_GRAY_W; }
    }
    if (vw == 0 || vh == 0) {
        return;
    }
    const uint16_t vx0 = (uint16_t)((VIEWFINDER_GRAY_W - vw) / 2);
    const uint16_t vy0 = (uint16_t)((VIEWFINDER_GRAY_H - vh) / 2);

    const bool primed = st->primed;
    const int prev_lo = st->level_lo;
    const int prev_range = st->level_range ? st->level_range : 255;

    /*
     * Pass 1: the box average of every frame pixel under each output pixel,
     * written raw into `out`, with the frame statistics. Before each pixel is
     * overwritten, the previous preview's value there is turned back into raw
     * terms with the stretch it was drawn with, so `motion` measures the
     * scene moving and not the auto-level breathing.
     */
    uint8_t lo = 0xFF, hi = 0x00;
    uint32_t sum = 0, clipped = 0, moved = 0;
    for (uint16_t y = 0; y < VIEWFINDER_GRAY_H; y++) {
        uint8_t *orow = out + (uint32_t)y * VIEWFINDER_GRAY_W;
        if (y < vy0 || y >= vy0 + vh) {
            memset(orow, 0, VIEWFINDER_GRAY_W);
            continue;
        }
        const uint16_t iy = (uint16_t)(y - vy0);
        const uint32_t v0 = (uint32_t)iy * lh / vh;
        uint32_t v1 = (uint32_t)(iy + 1) * lh / vh;
        if (v1 <= v0) { v1 = v0 + 1; }
        memset(orow, 0, vx0);
        memset(orow + vx0 + vw, 0, (size_t)(VIEWFINDER_GRAY_W - vx0 - vw));
        for (uint16_t x = 0; x < vw; x++) {
            const uint32_t u0 = (uint32_t)x * lw / vw;
            uint32_t u1 = (uint32_t)(x + 1) * lw / vw;
            if (u1 <= u0) { u1 = u0 + 1; }
            /* The frame rectangle under the shown pixel [u0,u1) x [v0,v1).
               Counter-clockwise by one: shown (u, v) is frame (fw-1-v, u). */
            uint32_t sx0, sx1, sy0, sy1;
            switch (rot) {
            case 1:  sx0 = fw - v1; sx1 = fw - v0; sy0 = u0;      sy1 = u1;      break;
            case 2:  sx0 = fw - u1; sx1 = fw - u0; sy0 = fh - v1; sy1 = fh - v0; break;
            case 3:  sx0 = v0;      sx1 = v1;      sy0 = fh - u1; sy1 = fh - u0; break;
            default: sx0 = u0;      sx1 = u1;      sy0 = v0;      sy1 = v1;      break;
            }
            uint32_t acc = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                const uint8_t *row = frame + sy * fw;
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    acc += row[sx];
                }
            }
            const uint8_t v = (uint8_t)(acc / ((sy1 - sy0) * (sx1 - sx0)));
            if (primed) {
                const int was = prev_lo + (orow[vx0 + x] * prev_range + 127) / 255;
                const int d = (int)v - was;
                moved += (uint32_t)(d < 0 ? -d : d);
            }
            orow[vx0 + x] = v;
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
            sum += v;
            if (v >= 250) { clipped++; }
        }
    }

    const uint32_t n = (uint32_t)vw * vh;
    st->lo = lo;
    st->hi = hi;
    st->mean = (uint8_t)(sum / n);
    st->clipped_pct = (uint8_t)(clipped * 100 / n);
    st->motion = primed ? clamp_u8((int)(moved / n)) : 0;

    /*
     * The stretch window: the frame's own darkest to brightest, widened about
     * its middle to VIEWFINDER_MIN_RANGE when the frame has less contrast than
     * that, so no frame is amplified more than about 4x.
     */
    int wlo = lo, wrange = hi - lo;
    if (wrange < VIEWFINDER_MIN_RANGE) {
        wlo = ((int)lo + hi) / 2 - VIEWFINDER_MIN_RANGE / 2;
        if (wlo < 0) { wlo = 0; }
        if (wlo + VIEWFINDER_MIN_RANGE > 255) { wlo = 255 - VIEWFINDER_MIN_RANGE; }
        wrange = VIEWFINDER_MIN_RANGE;
    }
    if (true_exposure) {
        wlo = 0;
        wrange = 255;
    }

    /* Pass 2: the share of bright pixels, and the stretch, in place. */
    const uint8_t mid = (uint8_t)(((uint16_t)lo + hi) / 2);
    uint32_t bright = 0;
    for (uint16_t y = vy0; y < vy0 + vh; y++) {
        uint8_t *p = out + (uint32_t)y * VIEWFINDER_GRAY_W + vx0;
        for (uint16_t x = 0; x < vw; x++) {
            const int v = p[x];
            if (v > mid) { bright++; }
            if (!true_exposure) {
                p[x] = clamp_u8((v - wlo) * 255 / wrange);
            }
        }
    }
    st->bright_pct = (uint8_t)(bright * 100 / n);
    st->level_lo = (uint8_t)wlo;
    st->level_range = (uint8_t)wrange;
    st->primed = true;
}

/* ---------------------------------------------------------------- hints */

/*
 * Thresholds from the arithmetic of a phone screen in a room, not from a
 * board: worth moving after a bench session (raw levels, 0..255).
 */
#define HINT_DARK_MEAN        32    /* the whole view is dim... */
#define HINT_DARK_HI          96    /* ...and nothing in it is bright */
#define HINT_BRIGHT_CLIPPED   30    /* % of the view saturated */
#define HINT_BRIGHT_MEAN      215
#define HINT_MOTION           14    /* mean change between previews */
#define HINT_CLOSER_CONTRAST  64    /* a bright region against the room... */
#define HINT_CLOSER_MIN_PCT   2     /* ...that is in frame... */
#define HINT_CLOSER_MAX_PCT   25    /* ...and small */

/*
 * The exposure row's colour, on the printed percentage of the raw mean
 * (0..99). Starting points from the user's first colour-kit session (a phone
 * at half brightness read best around a quarter to a half of full scale);
 * move them after a bench session. Inclusive bounds:
 *
 *   GOOD    LEVEL_GOOD_MIN_PCT .. LEVEL_GOOD_MAX_PCT        (green)
 *   MEDIUM  LEVEL_MEDIUM_MIN_PCT .. LEVEL_MEDIUM_MAX_PCT,
 *           outside GOOD                                    (amber)
 *   BAD     everything else                                 (red)
 */
#define LEVEL_GOOD_MIN_PCT    25
#define LEVEL_GOOD_MAX_PCT    60
#define LEVEL_MEDIUM_MIN_PCT  15
#define LEVEL_MEDIUM_MAX_PCT  85

static unsigned level_pct(uint8_t mean)
{
    unsigned pct = (unsigned)mean * 100u / 255u;
    return pct > 99 ? 99 : pct;
}

viewfinder_level_t viewfinder_level_grade(uint8_t mean)
{
    const unsigned pct = level_pct(mean);
    if (pct >= LEVEL_GOOD_MIN_PCT && pct <= LEVEL_GOOD_MAX_PCT) {
        return VIEWFINDER_LEVEL_GOOD;
    }
    if (pct >= LEVEL_MEDIUM_MIN_PCT && pct <= LEVEL_MEDIUM_MAX_PCT) {
        return VIEWFINDER_LEVEL_MEDIUM;
    }
    return VIEWFINDER_LEVEL_BAD;
}

const char *viewfinder_hint(const ViewfinderStats *st, bool located)
{
    if (st == NULL || !st->primed) {
        return NULL;
    }
    if (st->mean < HINT_DARK_MEAN && st->hi < HINT_DARK_HI) {
        return "Too dark";
    }
    if (st->clipped_pct >= HINT_BRIGHT_CLIPPED || st->mean >= HINT_BRIGHT_MEAN) {
        return "Too bright";
    }
    if (st->motion >= HINT_MOTION) {
        return "Hold steady";
    }
    if (!located && st->hi - st->lo >= HINT_CLOSER_CONTRAST &&
        st->bright_pct >= HINT_CLOSER_MIN_PCT && st->bright_pct <= HINT_CLOSER_MAX_PCT) {
        return "Move closer";
    }
    return NULL;
}

/* ---------------------------------------------------------------- lines */

void viewfinder_exposure_line(int8_t bias, uint8_t mean, char out[VIEWFINDER_LINE])
{
    if (bias < -5) { bias = -5; }
    if (bias > 0)  { bias = 0; }
    /* Six slots, -5 at the left: filled up to the knob, empty after it. */
    char bar[9];
    bar[0] = '<';
    for (int i = 0; i < 6; i++) {
        const int at = bias + 5;
        bar[1 + i] = i < at ? '=' : (i == at ? 'O' : '-');
    }
    bar[7] = '>';
    bar[8] = '\0';
    snprintf(out, VIEWFINDER_LINE, "Light %s %2d %2u%%", bar, (int)bias, level_pct(mean));
}

void viewfinder_stats_line(const char *zoom, uint16_t fps, uint32_t seen,
                           uint32_t read, char out[VIEWFINDER_LINE])
{
    /* The label cut to four, and each number clamped to two digits in a
       buffer of its own, so the line provably fits: "2.7x 99fps s:99 r:99"
       is 20. */
    char size[5];
    snprintf(size, sizeof size, "%s", zoom != NULL ? zoom : "?");
    char rate[3], s[3], r[3];
    if (fps == 0xFFFF) {
        memcpy(rate, "--", 3);
    } else {
        snprintf(rate, sizeof rate, "%u", (unsigned)(fps > 99 ? 99 : fps) % 100u);
    }
    snprintf(s, sizeof s, "%u", (unsigned)(seen > 99 ? 99 : seen) % 100u);
    snprintf(r, sizeof r, "%u", (unsigned)(read > 99 ? 99 : read) % 100u);
    snprintf(out, VIEWFINDER_LINE, "%s %sfps s:%s r:%s", size, rate, s, r);
}

void viewfinder_progress_line(uint32_t total, uint32_t remaining,
                              char out[VIEWFINDER_LINE])
{
    if (remaining > total) { remaining = total; }
    const uint32_t done = total - remaining;
    const uint32_t fill = total ? done * 10 / total : 0;
    char bar[13];
    bar[0] = '[';
    for (uint32_t i = 0; i < 10; i++) {
        bar[1 + i] = i < fill ? '#' : '-';
    }
    bar[11] = ']';
    bar[12] = '\0';
    snprintf(out, VIEWFINDER_LINE, "%s %u left", bar,
             (unsigned)(remaining > 999 ? 999 : remaining));
}
