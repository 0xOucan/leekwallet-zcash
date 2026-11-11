/*
 * Host-native tests for src/scan-timing.c: the per-stage frame timing and
 * the time-to-complete-UR clock the camera tuning is measured with. If these
 * numbers are wrong, every bench comparison built on them is too.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "scan-timing.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while (0)

static void test_stages(void)
{
    printf("Stage timing\n");
    ScanTiming t;
    scan_timing_reset(&t);
    char line[160];
    scan_timing_format(&t, line, sizeof line);
    CHECK(!strcmp(line, "wait - copy - ident - dec - prev - draw -"),
          "empty: \"%s\"", line);

    scan_timing_add(&t, SCAN_STAGE_WAIT, 40000);
    scan_timing_add(&t, SCAN_STAGE_WAIT, 42500);
    scan_timing_add(&t, SCAN_STAGE_IDENTIFY, 38049);
    scan_timing_add(&t, SCAN_STAGE_DRAW, 999);
    CHECK(scan_timing_mean_us(&t, SCAN_STAGE_WAIT) == 41250, "mean %u",
          (unsigned)scan_timing_mean_us(&t, SCAN_STAGE_WAIT));
    CHECK(t.max_us[SCAN_STAGE_WAIT] == 42500, "max %u", (unsigned)t.max_us[SCAN_STAGE_WAIT]);
    CHECK(scan_timing_mean_us(&t, SCAN_STAGE_COPY) == 0, "a stage with no samples has a mean");
    scan_timing_format(&t, line, sizeof line);
    CHECK(!strcmp(line, "wait 41.2/43 copy - ident 38.0/38 dec - prev - draw 0.9/1"),
          "formatted: \"%s\"", line);

    /* Out-of-range stages and NULLs are ignored, not written past the end. */
    scan_timing_add(&t, SCAN_STAGE_COUNT, 5);
    scan_timing_add(NULL, SCAN_STAGE_WAIT, 5);
    CHECK(t.n[SCAN_STAGE_WAIT] == 2, "a bad add counted");

    /* Short buffers truncate and stay terminated. */
    char tiny[12];
    memset(tiny, 'x', sizeof tiny);
    const size_t n = scan_timing_format(&t, tiny, sizeof tiny);
    CHECK(n < sizeof tiny && tiny[n] == '\0' && strlen(tiny) == n, "truncation: %zu \"%s\"", n, tiny);

    scan_timing_reset(&t);
    CHECK(t.n[SCAN_STAGE_WAIT] == 0 && t.max_us[SCAN_STAGE_WAIT] == 0, "reset kept samples");
}

static void test_ur_clock(void)
{
    printf("Time to a complete UR\n");
    ScanUrClock c;
    scan_ur_reset(&c);
    ScanUrResult r;
    CHECK(!scan_ur_done(&c, 1000, 0, 5, &r), "done with no part seen");

    /* First part at 2.0 s, frame 100; 7 parts, 2 of them redundant; done
       at 5.5 s, frame 140, for a 5-part message. */
    scan_ur_part(&c, 2000000, 100, false);
    for (int i = 0; i < 4; i++) scan_ur_part(&c, 2500000 + i * 500000, 110 + i, i == 1);
    scan_ur_part(&c, 5000000, 135, true);
    scan_ur_part(&c, 5500000, 140, false);
    CHECK(scan_ur_done(&c, 5500000, 140, 5, &r), "not done");
    CHECK(r.ms == 3500 && r.frames == 40 && r.parts == 7 && r.redundant == 2 && r.seq_len == 5,
          "result %u ms %u frames %u parts %u redundant %u seq",
          (unsigned)r.ms, (unsigned)r.frames, (unsigned)r.parts, (unsigned)r.redundant,
          (unsigned)r.seq_len);
    char line[120];
    scan_ur_format(&r, line, sizeof line);
    CHECK(!strcmp(line, "UR complete: 3500 ms, 40 frames, 7 parts for seqLen 5 (2 redundant, 140%)"),
          "\"%s\"", line);
    CHECK(!scan_ur_done(&c, 6000000, 150, 5, &r), "done twice");

    /* The next message starts its own clock. */
    scan_ur_part(&c, 9000000, 200, false);
    CHECK(scan_ur_done(&c, 9000000, 200, 1, &r) && r.ms == 0 && r.parts == 1,
          "a single-frame UR: %u ms %u parts", (unsigned)r.ms, (unsigned)r.parts);

    /* seqLen 0 cannot divide by zero. */
    r.seq_len = 0;
    scan_ur_format(&r, line, sizeof line);
    CHECK(strstr(line, "0%)") != NULL, "\"%s\"", line);
}

int main(void)
{
    printf("=== scan timing ===\n");
    test_stages();
    test_ur_clock();
    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nAll scan timing tests passed\n");
    return 0;
}
