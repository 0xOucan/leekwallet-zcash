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

/* Both kits' prompts; the longest is what has to fit. */
#define PROMPT "Press any key"

static splash_view_t view_at(splash_loop_t *s, int64_t ms) { return splash_loop_view(s, ms); }

/* The loop: plays past one pass, blinks the prompt, takes input as each kit
 * wants it, blanks when left alone and wakes without dismissing. */
static void test_loop(void)
{
    printf("== the loop\n");
    const unsigned n = splash_frame_count();
    const uint32_t anim = splash_frame_end_ms(n - 2);
    const uint32_t cycle = splash_loop_cycle_ms();
    const int64_t t0 = 1000000;   /* an arbitrary boot time */
    splash_loop_t s;
    splash_loop_start(&s, t0);

    CHECK(cycle == anim + SPLASH_LOOP_HOLD_MS, "cycle %u ms", (unsigned)cycle);
    CHECK(SPLASH_LOOP_HOLD_MS >= 2000, "the text is not held long enough to read");

    /* Every frame comes up in order in the first pass, and again in the
     * second and the tenth: it loops rather than stopping on the text. */
    for (int pass = 0; pass < 10; pass += (pass == 1 ? 8 : 1)) {
        const int64_t base = t0 + (int64_t)pass * cycle;
        for (unsigned f = 0; f + 1 < n; f++) {
            const uint32_t at = f ? splash_frame_end_ms(f - 1) : 0;
            splash_view_t v = view_at(&s, base + at);
            CHECK(!v.blank && v.frame == f && !v.prompt, "pass %d: frame %u not up at %u ms (got %u)",
                  pass, f, (unsigned)at, v.frame);
        }
        splash_view_t v = view_at(&s, base + anim);
        CHECK(v.frame == n - 1 && v.prompt, "pass %d: the text and prompt are not up", pass);
        v = view_at(&s, base + anim + SPLASH_PROMPT_BLINK_MS);
        CHECK(v.frame == n - 1 && !v.prompt, "pass %d: the prompt does not blink off", pass);
        v = view_at(&s, base + anim + 2 * SPLASH_PROMPT_BLINK_MS);
        CHECK(v.frame == n - 1 && v.prompt, "pass %d: the prompt does not blink back", pass);
        v = view_at(&s, base + cycle - 1);
        CHECK(v.frame == n - 1, "pass %d: the hold ends early", pass);
    }

    /* next_change_ms lands exactly on the next change, never past it. */
    for (int64_t t = t0; t < t0 + 2 * (int64_t)cycle; t += 7) {
        const uint32_t d = splash_loop_next_change_ms(&s, t);
        CHECK(d > 0 && d <= SPLASH_LOOP_HOLD_MS, "next change in %u ms at %lld", (unsigned)d, (long long)t);
        splash_view_t now = view_at(&s, t), before = view_at(&s, t + d - 1), after = view_at(&s, t + d);
        CHECK(splash_view_equal(now, before), "the view changed before next_change at %lld", (long long)t);
        CHECK(!splash_view_equal(now, after), "nothing changed at next_change at %lld", (long long)t);
    }

    /* Knob kit (ok_only): a turn or Cancel does not dismiss; it shows the
     * prompt at once. The press does. */
    splash_loop_start(&s, t0);
    int64_t t = t0 + 200;
    CHECK(splash_loop_input(&s, t, false, true) == SPLASH_INPUT_PROMPT, "knob: a turn dismissed");
    splash_view_t v = view_at(&s, t);
    CHECK(v.frame == n - 1 && v.prompt, "knob: a turn did not bring up the prompt");
    v = view_at(&s, t + SPLASH_LOOP_HOLD_MS);
    CHECK(v.frame == 0, "knob: the loop did not carry on after a turn");
    CHECK(splash_loop_input(&s, t + 5, true, true) == SPLASH_INPUT_DISMISS, "knob: the press did not dismiss");

    /* Four buttons: any key dismisses. */
    splash_loop_start(&s, t0);
    CHECK(splash_loop_input(&s, t0 + 10, false, false) == SPLASH_INPUT_DISMISS, "4 keys: a key did not dismiss");
    CHECK(splash_loop_input(&s, t0 + 20, true, false) == SPLASH_INPUT_DISMISS, "4 keys: OK did not dismiss");

    /* Blanking: lit until 10 minutes without input, black from then on, and
     * a turn in between restarts the count. */
    splash_loop_start(&s, t0);
    v = view_at(&s, t0 + SPLASH_BLANK_AFTER_MS - 1);
    CHECK(!v.blank, "blank before the timeout");
    CHECK(splash_loop_next_change_ms(&s, t0 + SPLASH_BLANK_AFTER_MS - 1) == 1, "the wait overshoots the blanking");
    splash_loop_input(&s, t0 + SPLASH_BLANK_AFTER_MS - 1, false, true);   /* a turn */
    v = view_at(&s, t0 + SPLASH_BLANK_AFTER_MS + 1000);
    CHECK(!v.blank, "a turn did not restart the blanking timer");
    const int64_t quiet = t0 + 2 * (int64_t)SPLASH_BLANK_AFTER_MS;
    v = view_at(&s, quiet);
    CHECK(v.blank, "not blank after 10 minutes untouched");
    CHECK(view_at(&s, quiet + 3600000LL).blank, "came back on by itself");
    CHECK(splash_loop_next_change_ms(&s, quiet) == UINT32_MAX, "a blank panel wants repainting");

    /* Waking: OK and the other inputs alike only wake it, from frame 0. */
    for (int k = 0; k < 2; k++) {
        const bool ok = k == 0;
        splash_loop_start(&s, t0);
        (void)view_at(&s, quiet);
        CHECK(splash_loop_input(&s, quiet + 5, ok, true) == SPLASH_INPUT_WOKE,
              "%s on a blank panel did more than wake it", ok ? "OK" : "a turn");
        v = view_at(&s, quiet + 5);
        CHECK(!v.blank && v.frame == 0, "%s: did not wake into the loop", ok ? "OK" : "a turn");
        CHECK(splash_loop_input(&s, quiet + 50, true, true) == SPLASH_INPUT_DISMISS,
              "%s: the next press does not dismiss", ok ? "OK" : "a turn");
    }
    /* Also on the four-button kit, where every key otherwise dismisses. */
    splash_loop_start(&s, t0);
    CHECK(splash_loop_input(&s, quiet, false, false) == SPLASH_INPUT_WOKE, "4 keys: a key dismissed a blank panel");
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
        splash_render_tall(f, NULL, tall);
        splash_render_tall(f, NULL, again);
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

        splash_render_wide(f, NULL, wide);
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

    /* The prompt: under the text, on the text frame only, inside the panel,
     * and clear of everything else. */
    {
        static uint8_t with[SPLASH_TALL_BYTES], wwith[SPLASH_WIDE_BYTES];
        splash_render_tall(n - 1, NULL, tall);
        splash_render_tall(n - 1, PROMPT, with);
        int extra = 0, lost = 0, low = 128;
        for (int y = 0; y < 128; y++)
            for (int x = 0; x < 128; x++) {
                if (px(with, x, y) && !px(tall, x, y)) { extra++; if (y < low) low = y; }
                if (!px(with, x, y) && px(tall, x, y)) lost++;
            }
        CHECK(extra > 40, "tall: the prompt is missing (%d px)", extra);
        CHECK(lost == 0, "tall: the prompt erased %d px", lost);
        CHECK(low >= 112, "tall: the prompt starts at row %d, among the text", low);
        if (pgm) write_pgm(pgm, "tall-prompt", n - 1, with, 128);
        splash_render_tall(0, PROMPT, with);
        splash_render_tall(0, NULL, tall);
        CHECK(memcmp(with, tall, sizeof tall) == 0, "tall: a prompt during the animation");

        splash_render_wide(n - 1, NULL, wide);
        splash_render_wide(n - 1, PROMPT, wwith);
        extra = 0; lost = 0; low = 64;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 128; x++) {
                if (px(wwith, x, y) && !px(wide, x, y)) { extra++; if (y < low) low = y; if (x < 36) lost++; }
                if (!px(wwith, x, y) && px(wide, x, y)) lost++;
            }
        CHECK(extra > 40, "wide: the prompt is missing (%d px)", extra);
        CHECK(lost == 0, "wide: the prompt hit the logo or the text (%d px)", lost);
        CHECK(low >= 40, "wide: the prompt starts at row %d, among the text", low);
        if (pgm) write_pgm(pgm, "wide-prompt", n - 1, wwith, 64);
    }

    test_loop();

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
