/**
 * Host-native tests for src/viewfinder.c - the Scan screen's aiming aid.
 *
 * A preview that is mirrored, rotated, or off by a factor of the downsample
 * ratio is worse than none: the user corrects their aim in the wrong
 * direction and concludes the camera is broken. On hardware that is a
 * bring-up day; here it is a synthetic frame with a bright rectangle in a
 * known corner and an assertion about which corner it lands in.
 *
 * The other thing checked here is the per-frame threshold. A fixed one is
 * wrong under the next light source, so a dim frame and a bright frame that
 * differ only by a constant offset must produce the identical panel.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viewfinder.h"

#define FW 320
#define FH 240

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        printf("  FAIL: "); printf(__VA_ARGS__);     \
        printf("\n"); failures++;                    \
    }                                                \
} while (0)

static uint8_t frame[FW * FH];
static uint8_t panel[VIEWFINDER_BYTES];

static void frame_fill(uint8_t v)
{
    memset(frame, v, sizeof frame);
}

/* Bright block over [x0,x1) x [y0,y1) in frame coordinates. */
static void frame_rect(int x0, int y0, int x1, int y1, uint8_t v)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            frame[y * FW + x] = v;
        }
    }
}

static bool panel_pixel(int x, int y)
{
    return (panel[(y / 8) * VIEWFINDER_W + x] >> (y % 8)) & 1u;
}

static int panel_lit(void)
{
    int n = 0;
    for (int y = 0; y < VIEWFINDER_H; y++) {
        for (int x = 0; x < VIEWFINDER_W; x++) {
            n += panel_pixel(x, y) ? 1 : 0;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ tests */

/* The corner test. Each quadrant of the frame, one at a time, and only the
   matching quadrant of the panel may light: that pins orientation in both
   axes at once, which a single centred rectangle cannot do. */
static void test_quadrants(void)
{
    printf("Quadrant mapping\n");

    const struct { int x0, y0, x1, y1; const char *name; } q[4] = {
        {   0,   0, FW / 2, FH / 2, "top-left"     },
        { FW/2,   0, FW,     FH / 2, "top-right"   },
        {   0, FH/2, FW / 2, FH,     "bottom-left" },
        { FW/2, FH/2, FW,    FH,     "bottom-right"},
    };

    /* The picture is letterboxed to the camera's aspect ratio, so a quadrant
       fills half of that image rather than half of the panel, and the bars
       either side stay dark. */
    const int vw = VIEWFINDER_H * FW / FH;
    const int vx0 = (VIEWFINDER_W - vw) / 2;

    for (int i = 0; i < 4; i++) {
        frame_fill(0x20);
        frame_rect(q[i].x0, q[i].y0, q[i].x1, q[i].y1, 0xF0);
        viewfinder_render(frame, FW, FH, panel);

        int inside = 0, outside = 0;
        for (int y = 0; y < VIEWFINDER_H; y++) {
            for (int x = 0; x < VIEWFINDER_W; x++) {
                const bool want_x = (q[i].x0 == 0)
                        ? (x >= vx0 && x < vx0 + vw / 2)
                        : (x >= vx0 + vw / 2 && x < vx0 + vw);
                const bool want_y = (q[i].y0 == 0) ? (y < VIEWFINDER_H / 2)
                                                   : (y >= VIEWFINDER_H / 2);
                if (panel_pixel(x, y)) {
                    if (want_x && want_y) { inside++; } else { outside++; }
                }
            }
        }
        CHECK(outside == 0, "%s: %d lit pixels outside the quadrant",
              q[i].name, outside);
        CHECK(inside == (vw / 2) * (VIEWFINDER_H / 2),
              "%s: %d lit inside, expected %d", q[i].name, inside,
              (vw / 2) * (VIEWFINDER_H / 2));
        /* And the letterbox bars are dark, which is what makes a square code
           look square. */
        int bars = 0;
        for (int y = 0; y < VIEWFINDER_H; y++) {
            for (int x = 0; x < VIEWFINDER_W; x++) {
                if ((x < vx0 || x >= vx0 + vw) && panel_pixel(x, y)) { bars++; }
            }
        }
        CHECK(bars == 0, "%s: %d lit pixels in the letterbox bars",
              q[i].name, bars);
    }
}

/* A QR sitting in the middle of the view is the ordinary case: it must appear
   in the middle, at roughly the same fraction of the panel it occupies of the
   frame. */
static void test_centred_block(void)
{
    printf("Centred block\n");

    frame_fill(0x10);
    frame_rect(FW / 4, FH / 4, FW * 3 / 4, FH * 3 / 4, 0xE0);
    viewfinder_render(frame, FW, FH, panel);

    CHECK(panel_pixel(VIEWFINDER_W / 2, VIEWFINDER_H / 2),
          "centre of the panel is dark");
    CHECK(!panel_pixel(1, 1), "top-left corner is lit");
    CHECK(!panel_pixel(VIEWFINDER_W - 2, VIEWFINDER_H - 2),
          "bottom-right corner is lit");

    const int lit = panel_lit();
    /* Half the letterboxed image, not half the panel. */
    const int want = ((VIEWFINDER_H * FW / FH) / 2) * (VIEWFINDER_H / 2);
    CHECK(lit > want * 9 / 10 && lit < want * 11 / 10,
          "lit area %d, expected about %d", lit, want);
}

/* The threshold comes from the frame, so the same scene under more light is
   the same picture. Two frames differing by a constant must render alike. */
static void test_threshold_is_per_frame(void)
{
    printf("Per-frame threshold\n");

    frame_fill(0x08);
    frame_rect(20, 20, 120, 120, 0x40);
    viewfinder_render(frame, FW, FH, panel);
    uint8_t dim[VIEWFINDER_BYTES];
    memcpy(dim, panel, sizeof dim);

    frame_fill(0x8F);
    frame_rect(20, 20, 120, 120, 0xC7);   /* same scene, +0x87 everywhere */
    viewfinder_render(frame, FW, FH, panel);

    CHECK(memcmp(dim, panel, sizeof dim) == 0,
          "the same scene under different light rendered differently");
    CHECK(panel_lit() > 0, "nothing lit at all");
}

/* A lens cap, a white wall, a frame the sensor has not exposed yet: no
   contrast means nothing to aim at, and a blank panel says that honestly
   rather than showing amplified noise. */
static void test_flat_frame_is_blank(void)
{
    printf("Flat frame\n");

    frame_fill(0x77);
    memset(panel, 0xAA, sizeof panel);
    viewfinder_render(frame, FW, FH, panel);
    CHECK(panel_lit() == 0, "%d pixels lit for a frame with no contrast",
          panel_lit());
}

/* Nothing here may write past the bitmap it was handed, and a null argument
   is a no-op rather than a crash: camera.c calls this on whatever the driver
   returned. */
static void test_guards(void)
{
    printf("Guards\n");

    viewfinder_render(NULL, FW, FH, panel);
    viewfinder_render(frame, 0, FH, panel);
    viewfinder_render(frame, FW, FH, NULL);
    printf("  (no crash)\n");
}

/* ============================================================ grayscale */

#define GW VIEWFINDER_GRAY_W
#define GH VIEWFINDER_GRAY_H

/* Canaries either side: nothing may be written outside the preview. */
static uint8_t gray_pad[16 + VIEWFINDER_GRAY_BYTES + 16];
static uint8_t *const gray = gray_pad + 16;
static uint8_t big[800 * 600];

static ViewfinderStats fresh_stats(void)
{
    ViewfinderStats st;
    memset(&st, 0, sizeof st);
    return st;
}

static void big_scene(int w, int h, uint8_t bg, uint8_t fg)
{
    /* The same scene at any size: a block over x 5%..15%, y 10%..30%. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const bool in = x >= w * 5 / 100 && x < w * 15 / 100 &&
                            y >= h * 10 / 100 && y < h * 30 / 100;
            big[y * w + x] = in ? fg : bg;
        }
    }
}

static uint8_t gpx(int x, int y) { return gray[y * GW + x]; }

/* A bright block near the top-left of the frame is near the top-left of the
   preview: not mirrored, not flipped, at the same fraction of the picture. */
static void test_gray_orientation(void)
{
    printf("Grayscale orientation and size\n");
    memset(gray_pad, 0xA5, sizeof gray_pad);

    const struct { int w, h; } modes[] = { { 320, 240 }, { 640, 480 }, { 800, 600 } };
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        const int w = modes[m].w, h = modes[m].h;
        big_scene(w, h, 0x20, 0xE0);
        ViewfinderStats st = fresh_stats();
        viewfinder_render_gray(big, (uint16_t)w, (uint16_t)h, gray, &st, false);

        /* Block: x 10..30, y 15..45 of 200x150. Its middle is lit... */
        CHECK(gpx(20, 30) == 255, "%dx%d: block centre is %u, not 255", w, h, gpx(20, 30));
        /* ...its mirror images and the far corner are dark. */
        CHECK(gpx(GW - 1 - 20, 30) == 0, "%dx%d: mirrored left-right (%u)", w, h, gpx(GW - 21, 30));
        CHECK(gpx(20, GH - 1 - 30) == 0, "%dx%d: flipped top-bottom (%u)", w, h, gpx(20, GH - 31));
        CHECK(gpx(GW - 2, GH - 2) == 0, "%dx%d: far corner lit", w, h);
        /* Edges where they belong, within the one pixel a box straddles. */
        CHECK(gpx(8, 30) == 0 && gpx(32, 30) == 0, "%dx%d: block too wide", w, h);
        CHECK(gpx(20, 13) == 0 && gpx(20, 47) == 0, "%dx%d: block too tall", w, h);
        CHECK(gpx(11, 16) == 255 && gpx(28, 43) == 255, "%dx%d: block too small", w, h);
    }

    bool canaries = true;
    for (int i = 0; i < 16; i++) {
        canaries &= gray_pad[i] == 0xA5 && gray_pad[16 + VIEWFINDER_GRAY_BYTES + i] == 0xA5;
    }
    CHECK(canaries, "wrote outside the %d-byte preview", VIEWFINDER_GRAY_BYTES);
    CHECK(VIEWFINDER_GRAY_W * 3 == VIEWFINDER_GRAY_H * 4, "preview is not 4:3");
}

/* Every quadrant, one at a time: only that quadrant of the preview lights. */
static void test_gray_quadrants(void)
{
    printf("Grayscale quadrants\n");
    const struct { int x0, y0; const char *name; } q[4] = {
        { 0, 0, "top-left" }, { FW / 2, 0, "top-right" },
        { 0, FH / 2, "bottom-left" }, { FW / 2, FH / 2, "bottom-right" },
    };
    for (int i = 0; i < 4; i++) {
        frame_fill(0x20);
        frame_rect(q[i].x0, q[i].y0, q[i].x0 + FW / 2, q[i].y0 + FH / 2, 0xF0);
        ViewfinderStats st = fresh_stats();
        viewfinder_render_gray(frame, FW, FH, gray, &st, false);
        int wrong = 0;
        for (int y = 0; y < GH; y++) {
            for (int x = 0; x < GW; x++) {
                const bool want = ((x >= GW / 2) == (q[i].x0 != 0)) &&
                                  ((y >= GH / 2) == (q[i].y0 != 0));
                if (gpx(x, y) != (want ? 255 : 0)) wrong++;
            }
        }
        CHECK(wrong == 0, "%s: %d pixels wrong", q[i].name, wrong);
    }
}

/* Auto-level: a dim scene fills the range, and the same scene under more
   light is the same picture. */
static void test_gray_autolevel(void)
{
    printf("Grayscale auto-level\n");
    static uint8_t dim[VIEWFINDER_GRAY_BYTES];

    frame_fill(0x08);
    frame_rect(40, 40, 200, 160, 0x68);
    ViewfinderStats st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(st.lo == 0x08 && st.hi == 0x68, "raw range %02x..%02x", st.lo, st.hi);
    uint8_t lo = 255, hi = 0;
    for (int i = 0; i < VIEWFINDER_GRAY_BYTES; i++) {
        if (gray[i] < lo) lo = gray[i];
        if (gray[i] > hi) hi = gray[i];
    }
    CHECK(lo == 0 && hi == 255, "a dim scene was shown %u..%u, not 0..255", lo, hi);
    memcpy(dim, gray, sizeof dim);

    frame_fill(0x08 + 0x80);
    frame_rect(40, 40, 200, 160, 0x68 + 0x80);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(memcmp(dim, gray, sizeof dim) == 0, "the same scene in more light looked different");

    /* True exposure: no stretch, the raw levels as the decoder sees them. */
    frame_fill(0x08);
    frame_rect(40, 40, 200, 160, 0x68);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, true);
    CHECK(gpx(2, 2) == 0x08 && gpx(GW / 2, GH / 2) == 0x68,
          "true exposure showed %02x/%02x, not 08/68", gpx(2, 2), gpx(GW / 2, GH / 2));
    CHECK(st.lo == 0x08 && st.hi == 0x68 && st.mean > 0x08 && st.mean < 0x68,
          "statistics differ with true exposure");
}

/* The gain cap: no contrast is a flat gray at its own level, not amplified
   noise, and a faint edge stays faint. */
static void test_gray_gain_cap(void)
{
    printf("Grayscale gain cap\n");
    frame_fill(0x77);
    ViewfinderStats st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    bool flat = true;
    for (int i = 1; i < VIEWFINDER_GRAY_BYTES; i++) flat &= gray[i] == gray[0];
    CHECK(flat, "a flat frame did not render flat");
    CHECK(gray[0] > 64 && gray[0] < 192, "a mid-gray wall shown as %u", gray[0]);

    frame_fill(0x70);
    frame_rect(0, 0, FW / 2, FH, 0x78);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    const int step = gpx(10, 10) - gpx(GW - 10, 10);
    CHECK(step > 0 && step <= 8 * 255 / VIEWFINDER_MIN_RANGE + 1,
          "an 8-level edge was stretched to %d", step);

    /* Black stays black and white stays white at the ends of the range. */
    frame_fill(0x00);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(gray[0] == 0, "black lens cap shown as %u", gray[0]);
    frame_fill(0xFF);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(gray[0] == 255, "white wall shown as %u", gray[0]);
    CHECK(st.clipped_pct == 100, "white wall %u%% clipped", st.clipped_pct);
}

/* Motion: the same frame twice is still; a moved block is not. */
static void test_gray_motion(void)
{
    printf("Grayscale motion\n");
    frame_fill(0x20);
    frame_rect(40, 40, 120, 120, 0xE0);
    ViewfinderStats st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(st.motion == 0 && st.primed, "first frame: motion %u", st.motion);
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(st.motion <= 1, "a still scene moved by %u", st.motion);

    frame_fill(0x20);
    frame_rect(160, 100, 280, 220, 0xE0);
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(st.motion >= 14, "a block jumping across the view moved only %u", st.motion);
}

static void test_hints(void)
{
    printf("Hints\n");
    ViewfinderStats st = fresh_stats();
    CHECK(viewfinder_hint(&st, false) == NULL, "a hint before any frame");
    CHECK(viewfinder_hint(NULL, false) == NULL, "a hint for no statistics");

    st.primed = true;
    st.lo = 2; st.hi = 40; st.mean = 12;
    CHECK(viewfinder_hint(&st, false) && !strcmp(viewfinder_hint(&st, false), "Too dark"), "dark");

    st = fresh_stats(); st.primed = true;
    st.lo = 200; st.hi = 255; st.mean = 240; st.clipped_pct = 60;
    CHECK(viewfinder_hint(&st, false) && !strcmp(viewfinder_hint(&st, false), "Too bright"), "bright");

    st = fresh_stats(); st.primed = true;
    st.lo = 10; st.hi = 200; st.mean = 90; st.bright_pct = 40; st.motion = 30;
    CHECK(viewfinder_hint(&st, true) && !strcmp(viewfinder_hint(&st, true), "Hold steady"), "steady");

    /* A small bright screen in a dim room, and nothing located: closer. */
    st = fresh_stats(); st.primed = true;
    st.lo = 10; st.hi = 220; st.mean = 40; st.bright_pct = 8;
    CHECK(viewfinder_hint(&st, false) && !strcmp(viewfinder_hint(&st, false), "Move closer"), "closer");
    CHECK(viewfinder_hint(&st, true) == NULL, "closer, with a code already located");

    /* An ordinary, well-lit, located view says nothing. */
    st = fresh_stats(); st.primed = true;
    st.lo = 10; st.hi = 230; st.mean = 120; st.bright_pct = 50;
    CHECK(viewfinder_hint(&st, true) == NULL, "a hint for a good view");

    /* And end to end, from frames. */
    frame_fill(0x04);
    frame_rect(100, 80, 140, 120, 0x30);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(viewfinder_hint(&st, false) && !strcmp(viewfinder_hint(&st, false), "Too dark"),
          "a dark frame gave %s", viewfinder_hint(&st, false) ? viewfinder_hint(&st, false) : "nothing");
    frame_fill(0x10);
    frame_rect(140, 100, 180, 140, 0xF0);
    st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(viewfinder_hint(&st, false) && !strcmp(viewfinder_hint(&st, false), "Move closer"),
          "a small bright screen gave %s", viewfinder_hint(&st, false) ? viewfinder_hint(&st, false) : "nothing");
}

static void test_lines(void)
{
    printf("Feedback lines\n");
    char line[VIEWFINDER_LINE];

    viewfinder_exposure_line(-4, 59, line);
    CHECK(!strcmp(line, "Light <=O----> -4 23%"), "exposure: \"%s\"", line);
    viewfinder_exposure_line(0, 255, line);
    CHECK(!strcmp(line, "Light <=====O>  0 99%"), "exposure at 0: \"%s\"", line);
    viewfinder_exposure_line(-5, 0, line);
    CHECK(!strcmp(line, "Light <O-----> -5  0%"), "exposure at -5: \"%s\"", line);
    viewfinder_exposure_line(-9, 0, line);
    CHECK(!strcmp(line, "Light <O-----> -5  0%"), "exposure clamps: \"%s\"", line);
    for (int b = -5; b <= 0; b++) {
        for (int m = 0; m < 256; m += 17) {
            viewfinder_exposure_line((int8_t)b, (uint8_t)m, line);
            CHECK(strlen(line) <= 21, "exposure %d/%d is %zu wide", b, m, strlen(line));
        }
    }

    viewfinder_stats_line("1x", 12, 3, 1, line);
    CHECK(!strcmp(line, "1x 12fps s:3 r:1"), "stats: \"%s\"", line);
    viewfinder_stats_line("3x", 0xFFFF, 12345, 999, line);
    CHECK(!strcmp(line, "3x --fps s:99 r:99"), "stats clamp: \"%s\"", line);
    viewfinder_stats_line("2x", 500, 0, 0, line);
    CHECK(!strcmp(line, "2x 99fps s:0 r:0"), "stats 2x: \"%s\"", line);
    /* A long label is cut so the row still fits; no label is "?". */
    viewfinder_stats_line("ZOOMZOOM", 99, 99, 99, line);
    CHECK(!strcmp(line, "ZOOM 99fps s:99 r:99") && strlen(line) <= 21, "stats long: \"%s\"", line);
    viewfinder_stats_line(NULL, 1, 0, 0, line);
    CHECK(!strcmp(line, "? 1fps s:0 r:0"), "stats no label: \"%s\"", line);

    viewfinder_progress_line(10, 6, line);
    CHECK(!strcmp(line, "[####------] 6 left"), "progress: \"%s\"", line);
    viewfinder_progress_line(3, 0, line);
    CHECK(!strcmp(line, "[##########] 0 left"), "progress done: \"%s\"", line);
    viewfinder_progress_line(5000, 4999, line);
    CHECK(strlen(line) <= 21 && !strncmp(line, "[----------] 999", 16), "progress clamp: \"%s\"", line);
    viewfinder_progress_line(0, 0, line);
    CHECK(!strcmp(line, "[----------] 0 left"), "progress none: \"%s\"", line);
}

/* The exposure row's colour: graded on the printed percentage, so the
   colour and the number can never disagree. */
static void test_level_grade(void)
{
    printf("Level grade\n");
    char line[VIEWFINDER_LINE];
    for (int m = 0; m < 256; m++) {
        viewfinder_exposure_line(-4, (uint8_t)m, line);
        const unsigned pct = (unsigned)atoi(line + strlen(line) - VIEWFINDER_EXPOSURE_PCT_CHARS);
        const viewfinder_level_t want =
            (pct >= 25 && pct <= 60) ? VIEWFINDER_LEVEL_GOOD :
            (pct >= 15 && pct <= 85) ? VIEWFINDER_LEVEL_MEDIUM : VIEWFINDER_LEVEL_BAD;
        CHECK(viewfinder_level_grade((uint8_t)m) == want,
              "mean %d (%u%%) graded %d, not %d", m, pct,
              (int)viewfinder_level_grade((uint8_t)m), (int)want);
        CHECK(line[strlen(line) - 1] == '%', "row does not end in its percentage: \"%s\"", line);
    }
    /* The edges, by name. 59 is 23%: medium. */
    CHECK(viewfinder_level_grade(0)   == VIEWFINDER_LEVEL_BAD,    "black is not bad");
    CHECK(viewfinder_level_grade(39)  == VIEWFINDER_LEVEL_MEDIUM, "15%% is not medium");
    CHECK(viewfinder_level_grade(38)  == VIEWFINDER_LEVEL_BAD,    "14%% is not bad");
    CHECK(viewfinder_level_grade(59)  == VIEWFINDER_LEVEL_MEDIUM, "23%% is not medium");
    CHECK(viewfinder_level_grade(64)  == VIEWFINDER_LEVEL_GOOD,   "25%% is not good");
    CHECK(viewfinder_level_grade(155) == VIEWFINDER_LEVEL_GOOD,   "60%% is not good");
    CHECK(viewfinder_level_grade(156) == VIEWFINDER_LEVEL_MEDIUM, "61%% is not medium");
    CHECK(viewfinder_level_grade(217) == VIEWFINDER_LEVEL_MEDIUM, "85%% is not medium");
    CHECK(viewfinder_level_grade(220) == VIEWFINDER_LEVEL_BAD,    "86%% is not bad");
    CHECK(viewfinder_level_grade(255) == VIEWFINDER_LEVEL_BAD,    "white is not bad");
}

static void test_gray_guards(void)
{
    printf("Grayscale guards\n");
    ViewfinderStats st = fresh_stats();
    viewfinder_render_gray(NULL, FW, FH, gray, &st, false);
    viewfinder_render_gray(frame, 0, FH, gray, &st, false);
    viewfinder_render_gray(frame, FW, 0, gray, &st, false);
    viewfinder_render_gray(frame, FW, FH, NULL, &st, false);
    viewfinder_render_gray(frame, FW, FH, gray, NULL, false);
    CHECK(!st.primed, "a refused call primed the statistics");
    printf("  (no crash)\n");
}

/* A quarter turn: the preview shows the frame turned, never mirrored, and
   letterboxed to 3:4 (112x150 in the middle of 200x150). Each turn is checked
   pixel by pixel against the frame point under the pixel's centre; pixels
   whose box straddles the bright quadrant's edge may be any gray. */
static void test_gray_rotation(void)
{
    printf("Grayscale rotation\n");
    for (int rot = 0; rot < 4; rot++) {
        viewfinder_set_rotation((uint8_t)rot);
        const int lw = (rot & 1) ? FH : FW, lh = (rot & 1) ? FW : FH;
        int vw = GW, vh = GW * lh / lw;
        if (vh > GH) { vh = GH; vw = GH * lw / lh; }
        const int vx0 = (GW - vw) / 2, vy0 = (GH - vh) / 2;
        frame_fill(0x20);
        frame_rect(FW / 2, 0, FW, FH / 2, 0xF0);   /* top-right of the frame */
        ViewfinderStats st = fresh_stats();
        viewfinder_render_gray(frame, FW, FH, gray, &st, false);
        int wrong = 0, mixed = 0, bars = 0;
        for (int y = 0; y < GH; y++) {
            for (int x = 0; x < GW; x++) {
                const uint8_t v = gpx(x, y);
                if (x < vx0 || x >= vx0 + vw || y < vy0 || y >= vy0 + vh) {
                    if (v != 0) bars++;
                    continue;
                }
                const double u = (x - vx0 + 0.5) * lw / vw;
                const double w = (y - vy0 + 0.5) * lh / vh;
                double fx, fy;
                switch (rot) {
                case 1:  fx = FW - w; fy = u;      break;
                case 2:  fx = FW - u; fy = FH - w; break;
                case 3:  fx = w;      fy = FH - u; break;
                default: fx = u;      fy = w;      break;
                }
                const bool want = fx >= FW / 2 && fy < FH / 2;
                if (v != 0 && v != 255) { mixed++; continue; }
                if ((v == 255) != want) wrong++;
            }
        }
        CHECK(wrong == 0, "turn %d: %d pixels wrong", rot, wrong);
        CHECK(mixed <= 2 * (vw + vh), "turn %d: %d edge pixels", rot, mixed);
        CHECK(bars == 0, "turn %d: %d bar pixels lit", rot, bars);
        CHECK(vw == ((rot & 1) ? 112 : 200) && vh == 150,
              "turn %d: view %dx%d", rot, vw, vh);
    }
    /* Counter-clockwise by one: the frame's top-right lands top-left. */
    viewfinder_set_rotation(1);
    frame_fill(0x20);
    frame_rect(FW / 2, 0, FW, FH / 2, 0xF0);
    ViewfinderStats st = fresh_stats();
    viewfinder_render_gray(frame, FW, FH, gray, &st, false);
    CHECK(gpx(44 + 10, 10) == 255 && gpx(44 + 100, 10) == 0 &&
          gpx(44 + 10, 140) == 0, "turn 1: top-right is not shown top-left");
    viewfinder_set_rotation(0);
}

int main(void)
{
    printf("=== viewfinder ===\n");
    test_quadrants();
    test_centred_block();
    test_threshold_is_per_frame();
    test_flat_frame_is_blank();
    test_guards();
    test_gray_orientation();
    test_gray_quadrants();
    test_gray_rotation();
    test_gray_autolevel();
    test_gray_gain_cap();
    test_gray_motion();
    test_hints();
    test_lines();
    test_level_grade();
    test_gray_guards();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nAll viewfinder tests passed\n");
    return 0;
}
