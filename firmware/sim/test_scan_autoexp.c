/*
 * Host-native tests for src/scan-autoexp.c: the Scan screen's automatic
 * exposure search, driven by scripted located/decoded counters the way
 * camera.c feeds it once a frame. A small "scene" stands in for the sensor:
 * for each setting it says how many codes a second are located and what
 * fraction of them decode, and the counters grow accordingly.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scan-autoexp.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while (0)

/* Located per second (x10) and decoded per 1000 located, by setting. */
typedef struct {
    unsigned loc_x10[AUTOEXP_SETTINGS];
    unsigned rate[AUTOEXP_SETTINGS];
} Scene;

typedef struct {
    AutoExp  a;
    uint32_t now;
    uint32_t located, decoded;
    uint32_t loc_acc, dec_acc;    /* fractional accumulators */
    unsigned events[8];           /* count by event */
    uint8_t  tried[32];
    unsigned n_tried;
    autoexp_event_t last_ev;
} Run;

static void run_init(Run *r, uint32_t start_ms, int8_t user_bias)
{
    memset(r, 0, sizeof *r);
    r->now = start_ms;
    autoexp_reset(&r->a, r->now, user_bias);
}

/* `ms` of scanning in 150 ms frames (~6.7 fps, the bench's rate). */
static void run_for(Run *r, const Scene *sc, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 150) {
        r->now += 150;
        AutoExpApply ap;
        autoexp_current(&r->a, &ap);
        /* located: loc_x10/10 per second -> per 150 ms, in 1/1000 units */
        r->loc_acc += sc->loc_x10[ap.index] * 15u;   /* x10 /s * 0.15 s * 1000 / 10 */
        while (r->loc_acc >= 1000) {
            r->loc_acc -= 1000;
            r->located++;
            r->dec_acc += sc->rate[ap.index];
            if (r->dec_acc >= 1000) {
                r->dec_acc -= 1000;
                r->decoded++;
            }
        }
        const autoexp_event_t ev = autoexp_tick(&r->a, r->now, r->located, r->decoded);
        if (ev != AUTOEXP_EV_NONE) {
            r->events[ev]++;
            r->last_ev = ev;
            if (ev == AUTOEXP_EV_TRY && r->n_tried < sizeof r->tried) {
                r->tried[r->n_tried++] = r->a.current;
            }
        }
    }
}

/* Until the first trial starts (the window has to fill first). */
static void run_until_try(Run *r, const Scene *sc)
{
    for (int i = 0; i < 400 && r->events[AUTOEXP_EV_TRY] == 0; i++) {
        run_for(r, sc, 150);
    }
}

static Scene scene_flat(unsigned loc_x10, unsigned rate)
{
    Scene s;
    for (int i = 0; i < AUTOEXP_SETTINGS; i++) {
        s.loc_x10[i] = loc_x10;
        s.rate[i] = rate;
    }
    return s;
}

static void test_settings_table(void)
{
    printf("Settings table\n");
    const AutoExpSetting *s0 = autoexp_setting(0);
    CHECK(s0->bias_add == 0 && s0->shutter_mul == 1 && !s0->mains_swap,
          "setting 0 must be the user's own");
    CHECK(autoexp_setting(200) == s0, "out of range is setting 0");
    for (uint8_t i = 1; i < AUTOEXP_SETTINGS; i++) {
        const AutoExpSetting *s = autoexp_setting(i);
        CHECK(s->shutter_mul >= 1 && s->shutter_mul <= 4, "setting %u shutter x%u", i,
              s->shutter_mul);
        CHECK(s->bias_add >= 0 && s->bias_add <= 2, "setting %u bias +%d", i, s->bias_add);
    }
    CHECK(autoexp_bias_for(0, -4) == -4, "user bias passes");
    CHECK(autoexp_bias_for(4, -4) == -2, "+2 over -4: %d", autoexp_bias_for(4, -4));
    CHECK(autoexp_bias_for(4, -1) == 0, "clamped at 0: %d", autoexp_bias_for(4, -1));
    CHECK(autoexp_bias_for(0, -9) == -5, "clamped at -5");
}

static void test_texts(void)
{
    printf("Status texts\n");
    char t[32];
    autoexp_event_text(AUTOEXP_EV_TRY, 1, t, sizeof t);
    CHECK(!strcmp(t, "Auto exposure +1"), "try: \"%s\"", t);
    autoexp_event_text(AUTOEXP_EV_KEEP, 5, t, sizeof t);
    CHECK(!strcmp(t, "Auto exposure +5 kept"), "keep: \"%s\"", t);
    CHECK(strlen(t) <= 21, "keep text %zu chars, the row holds 21", strlen(t));
    autoexp_event_text(AUTOEXP_EV_REVERT, 0, t, sizeof t);
    CHECK(!strcmp(t, "Auto exposure off"), "revert: \"%s\"", t);
    autoexp_event_text(AUTOEXP_EV_REVERT, 3, t, sizeof t);
    CHECK(!strcmp(t, "Auto exposure +3 kept"), "revert to kept: \"%s\"", t);
    autoexp_event_text(AUTOEXP_EV_KEEP, 0, t, sizeof t);
    CHECK(!strcmp(t, "Auto exposure off"), "keep 0: \"%s\"", t);
    autoexp_event_text(AUTOEXP_EV_STOP, 2, t, sizeof t);
    CHECK(t[0] == '\0', "stop is silent");
    char small[8];
    autoexp_event_text(AUTOEXP_EV_KEEP, 5, small, sizeof small);
    CHECK(strlen(small) == 7, "truncated, terminated");
}

/* An LCD: ~5 located a second, 80 % read. Never touched. */
static void test_lcd_untouched(void)
{
    printf("LCD reads well: no search\n");
    Run r;
    run_init(&r, 1000, -4);
    const Scene sc = scene_flat(50, 800);
    run_for(&r, &sc, 60000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 0, "%u trials on a good screen", r.events[AUTOEXP_EV_TRY]);
    CHECK(r.a.current == 0 && autoexp_phase(&r.a) == AUTOEXP_IDLE, "still idle at 0");
}

/* Borderline but fine: 20 % read is well above the 10 % trigger, even with
   one decode either way in a window. */
static void test_borderline_untouched(void)
{
    printf("20%% read: no search\n");
    Run r;
    run_init(&r, 0, -4);
    const Scene sc = scene_flat(40, 200);
    run_for(&r, &sc, 30000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 0, "%u trials", r.events[AUTOEXP_EV_TRY]);
}

/* No code in view, or only now and then: nothing to judge, no search. */
static void test_nothing_located(void)
{
    printf("Too few located: no search\n");
    Run r;
    run_init(&r, 0, -4);
    Scene sc = scene_flat(0, 0);
    run_for(&r, &sc, 20000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 0, "nothing located, %u trials", r.events[AUTOEXP_EV_TRY]);
    sc = scene_flat(15, 0);   /* 1.5 a second: 3-4 in a window, under the 5 needed */
    run_for(&r, &sc, 20000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 0, "2/s located, %u trials", r.events[AUTOEXP_EV_TRY]);
}

/* The bench's bad case: located ~5/s, read ~1 %. Setting 2 (x2, +1) reads
   60 %: the search tries 1, then 2, and keeps 2 at once. */
static void test_oled_early_keep(void)
{
    printf("OLED, setting 2 fixes it: kept early\n");
    Run r;
    run_init(&r, 5000, -4);
    Scene sc = scene_flat(50, 10);
    sc.rate[1] = 50;
    sc.rate[2] = 600;
    run_for(&r, &sc, 2000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 0, "not before a full window");
    run_for(&r, &sc, 600);
    CHECK(r.events[AUTOEXP_EV_TRY] == 1 && r.a.current == 1,
          "triggered after ~2 s: %u trials, at %u", r.events[AUTOEXP_EV_TRY], r.a.current);
    AutoExpApply ap;
    autoexp_current(&r.a, &ap);
    CHECK(ap.shutter_mul == 2 && ap.bias == -4 && !ap.mains_swap, "trial 1 applies x2 at -4");
    run_for(&r, &sc, 20000);
    CHECK(r.n_tried == 2 && r.tried[0] == 1 && r.tried[1] == 2, "tried %u settings", r.n_tried);
    CHECK(r.events[AUTOEXP_EV_KEEP] == 1 && r.last_ev == AUTOEXP_EV_KEEP, "kept");
    CHECK(r.a.current == 2 && autoexp_phase(&r.a) == AUTOEXP_IDLE, "holding 2, watching");
    autoexp_current(&r.a, &ap);
    CHECK(ap.bias == -3 && ap.shutter_mul == 2, "kept x2 at -3 (bias %d)", ap.bias);
    /* It keeps reading at 60 %: no second search. */
    run_for(&r, &sc, 30000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 2, "no new search: %u trials", r.events[AUTOEXP_EV_TRY]);
}

/* Each trial takes settle + hold; the whole search is bounded. */
static void test_search_time(void)
{
    printf("Search time is bounded\n");
    Run r;
    run_init(&r, 0, -4);
    const Scene sc = scene_flat(50, 0);
    run_until_try(&r, &sc);
    CHECK(r.events[AUTOEXP_EV_TRY] == 1, "started");
    CHECK(r.now <= 2700, "triggered at %u ms", r.now);
    const uint32_t t0 = r.now;
    while (r.last_ev != AUTOEXP_EV_REVERT && r.now - t0 < 60000) {
        run_for(&r, &sc, 150);
    }
    const uint32_t took = r.now - t0;
    CHECK(r.last_ev == AUTOEXP_EV_REVERT, "ended");
    /* 5 trials x (700 + 2000) ms, give or take a frame each. */
    CHECK(took >= 5 * 2700 && took <= 5 * 2700 + 5 * 300, "search took %u ms", took);
    printf("  five trials: %u ms\n", took);
}

/* Nothing helps: all five tried, back to the user's, a cool-down, one more
   search, then no more for this scan. */
static void test_nothing_helps(void)
{
    printf("Nothing helps: revert, cool down, at most two searches\n");
    Run r;
    run_init(&r, 0, -4);
    const Scene sc = scene_flat(50, 10);
    run_for(&r, &sc, 20000);
    CHECK(r.n_tried == 5, "tried %u", r.n_tried);
    for (unsigned i = 0; i < r.n_tried && i < 5; i++) {
        CHECK(r.tried[i] == i + 1, "order: trial %u was %u", i, r.tried[i]);
    }
    CHECK(r.events[AUTOEXP_EV_REVERT] == 1 && r.a.current == 0, "back to the user's");
    CHECK(autoexp_phase(&r.a) == AUTOEXP_COOLDOWN, "cooling down");
    run_for(&r, &sc, 60000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 10, "two searches, %u trials", r.events[AUTOEXP_EV_TRY]);
    CHECK(r.events[AUTOEXP_EV_REVERT] == 2, "two reverts");
    run_for(&r, &sc, 60000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 10, "no third search");
    CHECK(autoexp_phase(&r.a) == AUTOEXP_DONE && r.a.current == 0, "done at the user's");
}

/* Modest gains only: the best of all five is kept even under the "good"
   mark, if it beats the trigger by the margin. */
static void test_best_of_all(void)
{
    printf("Best of all kept\n");
    Run r;
    run_init(&r, 0, -4);
    Scene sc = scene_flat(50, 10);
    sc.rate[1] = 100;
    sc.rate[3] = 200;
    sc.rate[4] = 300;
    sc.rate[5] = 150;
    run_for(&r, &sc, 20000);
    CHECK(r.n_tried == 5, "tried %u", r.n_tried);
    CHECK(r.events[AUTOEXP_EV_KEEP] == 1 && r.a.current == 4, "kept %u", r.a.current);
    run_for(&r, &sc, 30000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 5, "30 %% holds: no new search");
}

/* A trial that loses the code (too bright to locate) reads as nothing,
   after one extension, and the search goes on. */
static void test_trial_loses_code(void)
{
    printf("A trial that locates nothing scores zero\n");
    Run r;
    run_init(&r, 0, -4);
    Scene sc = scene_flat(50, 10);
    sc.loc_x10[1] = 0;
    sc.rate[3] = 500;
    run_until_try(&r, &sc);
    CHECK(r.a.current == 1 && autoexp_phase(&r.a) == AUTOEXP_SETTLING, "trial 1");
    run_for(&r, &sc, 700 + 2000 + 150);
    CHECK(r.a.current == 1 && autoexp_phase(&r.a) == AUTOEXP_JUDGING, "extended, not judged yet");
    run_for(&r, &sc, 2000);
    CHECK(r.a.current == 2, "then on to 2 (at %u)", r.a.current);
    run_for(&r, &sc, 20000);
    CHECK(r.a.current == 3 && r.events[AUTOEXP_EV_KEEP] == 1, "kept 3 (at %u)", r.a.current);
}

/* A knob turn mid-search: stopped for the scan, the user's bias applies. */
static void test_user_override(void)
{
    printf("Knob turn stops the search\n");
    Run r;
    run_init(&r, 0, -4);
    const Scene sc = scene_flat(50, 10);
    run_for(&r, &sc, 6000);
    CHECK(r.a.current != 0, "mid-search");
    CHECK(autoexp_user_override(&r.a, -3) == AUTOEXP_EV_STOP, "stop event");
    CHECK(autoexp_user_override(&r.a, -2) == AUTOEXP_EV_NONE, "once");
    AutoExpApply ap;
    autoexp_current(&r.a, &ap);
    CHECK(ap.index == 0 && ap.bias == -2 && ap.shutter_mul == 1 && !ap.mains_swap,
          "the user's own: bias %d x%u", ap.bias, ap.shutter_mul);
    const unsigned tries = r.events[AUTOEXP_EV_TRY];
    run_for(&r, &sc, 60000);
    CHECK(r.events[AUTOEXP_EV_TRY] == tries, "no more trials");
    CHECK(autoexp_phase(&r.a) == AUTOEXP_STOPPED, "stopped");

    /* And before any search: nothing ever starts. */
    run_init(&r, 0, -4);
    autoexp_user_override(&r.a, -5);
    run_for(&r, &sc, 30000);
    CHECK(r.events[AUTOEXP_EV_TRY] == 0, "no search after an early turn");
}

/* At bias 0 the "+1" steps clamp to the same thing; each is tried once. */
static void test_duplicates_skipped(void)
{
    printf("Clamped duplicates are skipped\n");
    Run r;
    run_init(&r, 0, 0);
    const Scene sc = scene_flat(50, 10);
    run_for(&r, &sc, 15000);
    CHECK(r.n_tried == 3, "tried %u", r.n_tried);
    CHECK(r.n_tried >= 3 && r.tried[0] == 1 && r.tried[1] == 3 && r.tried[2] == 5,
          "order %u %u %u", r.tried[0], r.tried[1], r.tried[2]);
}

/* A second search from a kept setting may go back to the user's own. */
static void test_second_search_reaches_user(void)
{
    printf("Second search can return to the user's setting\n");
    Run r;
    run_init(&r, 0, -4);
    Scene sc = scene_flat(50, 10);
    sc.rate[2] = 600;
    run_for(&r, &sc, 12000);
    CHECK(r.a.current == 2 && r.events[AUTOEXP_EV_KEEP] == 1, "kept 2");
    /* The scene changes: now only the user's own reads. */
    sc = scene_flat(50, 10);
    sc.rate[0] = 700;
    run_for(&r, &sc, 30000);
    CHECK(r.a.current == 0, "back at 0 (at %u)", r.a.current);
    CHECK(r.tried[r.n_tried - 1] == 0, "0 tried last");
    CHECK(r.events[AUTOEXP_EV_KEEP] == 2, "kept twice");
}

/* A zoom push mid-trial restarts its settle. */
static void test_disturbed(void)
{
    printf("A zoom push restarts the trial's settle\n");
    Run r;
    run_init(&r, 0, -4);
    const Scene sc = scene_flat(50, 10);
    run_until_try(&r, &sc);
    run_for(&r, &sc, 700 + 900);
    CHECK(autoexp_phase(&r.a) == AUTOEXP_JUDGING && r.a.current == 1, "judging 1");
    autoexp_disturbed(&r.a, r.now);
    CHECK(autoexp_phase(&r.a) == AUTOEXP_SETTLING, "settling again");
    run_for(&r, &sc, 2000);
    CHECK(r.a.current == 1, "still on 1 after 2 s");
    run_for(&r, &sc, 1000);
    CHECK(r.a.current == 2, "on to 2 after settle + hold");
}

/* The millisecond clock and the counters wrap without a hiccup. */
static void test_wrap(void)
{
    printf("Clock and counter wrap\n");
    Run r;
    run_init(&r, 0xFFFFFFFFu - 1000, -4);
    r.located = 0xFFFFFFF0u;
    r.decoded = 0xFFFFFFF0u;
    Scene sc = scene_flat(50, 10);
    sc.rate[1] = 600;
    run_for(&r, &sc, 10000);
    CHECK(r.events[AUTOEXP_EV_KEEP] == 1 && r.a.current == 1, "kept 1 across the wrap (at %u)",
          r.a.current);
}

int main(void)
{
    test_settings_table();
    test_texts();
    test_lcd_untouched();
    test_borderline_untouched();
    test_nothing_located();
    test_oled_early_keep();
    test_search_time();
    test_nothing_helps();
    test_best_of_all();
    test_trial_loses_code();
    test_user_override();
    test_duplicates_skipped();
    test_second_search_reaches_user();
    test_disturbed();
    test_wrap();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("All scan auto-exposure tests passed.\n");
    return 0;
}
