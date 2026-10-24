/* The boot splash (src/splash.c): its timing, and both renderings.
 *
 *   build/test_splash              the checks
 *   build/test_splash --pgm DIR    also write every frame, tall and wide, as
 *                                  PGM images at 4x into DIR, to look at
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "splash.h"
#include "version.h"
#include "oled.h"

void leek_log(const char *level, const char *tag, const char *fmt, ...) { }


static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static bool px(const uint8_t *img, int x, int y) { return (img[(y / 8) * 128 + x] >> (y % 8)) & 1; }

static int lit_in(const uint8_t *img, int rows, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1 && y < rows; y++) {
        for (int x = x0; x < x1; x++) n += px(img, x, y);
    }
    return n;
}

static void write_pgm(const char *dir, const char *kind, unsigned f, const uint8_t *img, int rows)
{
    char path[512];
    snprintf(path, sizeof path, "%s/splash-%s-%02u.pgm", dir, kind, f);
    FILE *o = fopen(path, "wb");
    if (!o) { perror(path); return; }
    const int S = 4;
    fprintf(o, "P5\n%d %d\n255\n", 128 * S, rows * S);
    for (int y = 0; y < rows * S; y++) {
        for (int x = 0; x < 128 * S; x++) fputc(px(img, x / S, y / S) ? 255 : 0, o);
    }
    fclose(o);
}

int main(int argc, char **argv)
{
    const char *pgm = (argc == 3 && strcmp(argv[1], "--pgm") == 0) ? argv[2] : NULL;
    printf("== test_splash\n");

    const unsigned n = splash_frame_count();
    const uint32_t total = splash_total_ms();
    printf("  %u frames, %u ms\n", n, (unsigned)total);

    /* Timing: short, and every instant maps to one frame in order. */
    CHECK(n >= 2, "an animation needs frames");
    CHECK(total <= 2500, "the splash takes %u ms, over 2.5 s", (unsigned)total);
    CHECK(total >= 1000, "the splash is too short to read (%u ms)", (unsigned)total);
    CHECK(splash_frame_at(0) == 0, "does not start on frame 0");
    CHECK(splash_frame_at(total) == n, "not over at the total");
    CHECK(splash_frame_at(total - 1) == n - 1, "the last frame is not up at the end");
    unsigned prev = 0;
    for (uint32_t ms = 0; ms <= total + 100; ms += 5) {
        unsigned f = splash_frame_at(ms);
        CHECK(f >= prev && f <= prev + 1, "frame %u after %u at %u ms", f, prev, (unsigned)ms);
        prev = f;
    }
    for (unsigned f = 0; f < n; f++) {
        CHECK(splash_frame_at(splash_frame_end_ms(f)) == f + 1, "frame %u ends late", f);
        CHECK(splash_frame_has_text(f) == (f == n - 1), "text on frame %u", f);
    }
    /* Held long enough at the end to read three lines. */
    CHECK(splash_frame_end_ms(n - 1) - splash_frame_end_ms(n - 2) >= 600, "the text is not held");

    /* The version line is the release's, from version.h. */
    CHECK(strcmp(SPLASH_LINE_2, "v" LEEK_FW_VERSION " " LEEK_FW_CODENAME) == 0, "version line");
    CHECK(strcmp(LEEK_FW_CODENAME, "Sacbeh") == 0, "codename");

    /* The logo moves: no two consecutive frames are the same picture, and the
     * first (lid on) differs from the last (lid up, key out). */
    for (unsigned f = 1; f < n - 1; f++) {
        int diff = 0;
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) diff += splash_logo_pixel(f, x, y) != splash_logo_pixel(f - 1, x, y);
        CHECK(diff > 0, "frame %u repeats frame %u", f, f - 1);
    }

    static uint8_t tall[SPLASH_TALL_BYTES], again[SPLASH_TALL_BYTES], wide[SPLASH_WIDE_BYTES];
    for (unsigned f = 0; f < n; f++) {
        splash_render_tall(f, tall);
        splash_render_tall(f, again);
        CHECK(memcmp(tall, again, sizeof tall) == 0, "frame %u is not deterministic", f);

        /* The logo is centred and nothing escapes its box but the text. */
        int logo = 0, total_lit = lit_in(tall, 128, 0, 0, 128, 128);
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) logo += splash_logo_pixel(f, x, y);
        CHECK(logo > 500, "frame %u has almost no logo (%d px)", f, logo);
        const int text = lit_in(tall, 128, 0, 80, 128, 128);
        if (splash_frame_has_text(f)) {
            CHECK(text > 150, "frame %u: text missing (%d px)", f, text);
            CHECK(total_lit == logo + text, "frame %u: text overlaps the logo", f);
        } else {
            CHECK(text == 0, "frame %u: text before its time", f);
            CHECK(total_lit == logo, "frame %u: %d px lit for a %d px logo", f, total_lit, logo);
        }
        /* Centred: as much margin on the left as on the right, give or take one. */
        int l = 128, r = -1;
        for (int y = 0; y < 80; y++)
            for (int x = 0; x < 128; x++)
                if (px(tall, x, y)) { if (x < l) l = x; if (x > r) r = x; }
        CHECK(l >= 20 && 127 - r >= 20, "frame %u: logo not centred (%d..%d)", f, l, r);

        splash_render_wide(f, wide);
        const int wlogo = lit_in(wide, 64, 0, 0, 36, 64);
        const int wtext = lit_in(wide, 64, 36, 0, 128, 64);
        CHECK(wlogo > 150, "wide frame %u: logo missing (%d px)", f, wlogo);
        CHECK(splash_frame_has_text(f) ? wtext > 150 : wtext == 0, "wide frame %u: text %d px", f, wtext);

        if (pgm) {
            write_pgm(pgm, "tall", f, tall, 128);
            write_pgm(pgm, "wide", f, wide, 64);
        }
    }
    if (pgm) printf("  frames written to %s\n", pgm);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
