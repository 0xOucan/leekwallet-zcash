/* The EC11 decoder (src/encoder.c): a detent each way, nothing from bounce
 * or a half turn that comes back, and every step of a long spin counted. */
#include <stdio.h>
#include <stdlib.h>

#include "encoder.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* (a, b) samples; 1 = open. One clockwise detent per the table: B falls
 * first. */
static const uint8_t CW[][2]  = { {1, 0}, {0, 0}, {0, 1}, {1, 1} };
static const uint8_t CCW[][2] = { {0, 1}, {0, 0}, {1, 0}, {1, 1} };

static int feed(Encoder *e, const uint8_t seq[][2], int n, int repeat)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        for (int r = 0; r < repeat; r++) sum += encoder_feed(e, seq[i][0], seq[i][1]);
    }
    return sum;
}

int main(void)
{
    printf("== test_encoder\n");
    Encoder e;
    encoder_init(&e);
    CHECK(feed(&e, CW, 4, 1) == 1, "one clockwise detent");
    CHECK(feed(&e, CCW, 4, 1) == -1, "one anticlockwise detent");
    /* Sampled 1 ms apart, a slow turn repeats each state many times. */
    CHECK(feed(&e, CW, 4, 7) == 1, "a slow detent counts once");

    int sum = 0;
    for (int i = 0; i < 100; i++) sum += feed(&e, CW, 4, 1);
    CHECK(sum == 100, "100 detents gave %d", sum);

    /* Bounce: the first contact chatters before settling. */
    const uint8_t bouncy[][2] = { {1, 0}, {1, 1}, {1, 0}, {1, 1}, {1, 0}, {0, 0}, {1, 0}, {0, 0},
                                  {0, 1}, {0, 0}, {0, 1}, {1, 1} };
    CHECK(feed(&e, bouncy, 12, 1) == 1, "bounce counted as more than one detent");

    /* Half way and back: nothing. */
    const uint8_t back[][2] = { {1, 0}, {0, 0}, {1, 0}, {1, 1} };
    CHECK(feed(&e, back, 4, 1) == 0, "a turn that came back produced a step");

    /* Noise on the resting lines alone: nothing. */
    const uint8_t noise[][2] = { {1, 0}, {1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, 1} };
    CHECK(feed(&e, noise, 6, 1) == 0, "single-line glitches produced a step");

    /* Random garbage never produces more steps than full cycles could. */
    srand(7);
    int steps = 0;
    for (int i = 0; i < 100000; i++) steps += abs(encoder_feed(&e, rand() & 1, rand() & 1));
    CHECK(steps <= 100000 / 4, "garbage gave %d steps", steps);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
