/*
 * Host-native tests for src/camera-tune.c: the OV5640 register values the
 * Scan screen's camera profile writes. A wrong value here is a camera that
 * "works" and scans badly, which no other suite would notice.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "camera-tune.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while (0)

static void test_gain_ceiling(void)
{
    printf("Gain ceiling\n");
    /* Real-gain format: 0x10 is 1x. The bug this replaces wrote 0x001. */
    CHECK(camera_tune_gain_ceiling_reg(1) == 0x010, "1x is 0x%03x", camera_tune_gain_ceiling_reg(1));
    CHECK(camera_tune_gain_ceiling_reg(4) == 0x040, "4x is 0x%03x", camera_tune_gain_ceiling_reg(4));
    CHECK(camera_tune_gain_ceiling_reg(8) == 0x080, "8x is 0x%03x", camera_tune_gain_ceiling_reg(8));
    CHECK(camera_tune_gain_ceiling_reg(16) == 0x100, "16x is 0x%03x", camera_tune_gain_ceiling_reg(16));
    CHECK(camera_tune_gain_ceiling_reg(0) == 0x010, "0x is not clamped to 1x");
    CHECK(camera_tune_gain_ceiling_reg(64) == 0x3FF, "64x does not clamp to the ten bits");
    CHECK(camera_tune_gain_ceiling_reg(1000) == 0x3FF, "huge does not clamp");
    for (unsigned x = 1; x < 64; x++) {
        CHECK(camera_tune_gain_ceiling_reg(x) > 0x00F, "%ux is below 1x", x);
    }
}

static void test_banding(void)
{
    printf("Banding\n");
    /* 60 Hz: manual mode, bit 2 clear. */
    CHECK(camera_tune_band_3c00(60) == 0x00, "60 Hz sets 0x3C00 bit 2");
    CHECK(camera_tune_band_3c01(60) == 0x80, "60 Hz is not manual");
    /* 50 Hz: manual, bit 2 set (the driver's own default). */
    CHECK(camera_tune_band_3c00(50) == 0x04, "50 Hz does not set bit 2");
    CHECK(camera_tune_band_3c01(50) == 0x80, "50 Hz is not manual");
    /* 0: automatic detection. */
    CHECK(camera_tune_band_3c01(0) == 0x00, "auto is manual");
    /* Anything unknown is the 60 Hz the board is used under. */
    CHECK(camera_tune_band_3c00(55) == 0x00 && camera_tune_band_3c01(55) == 0x80,
          "an unknown frequency is not 60 Hz");
    /* Never outside the masks. */
    for (unsigned hz = 0; hz < 100; hz++) {
        CHECK((camera_tune_band_3c00(hz) & ~CAMERA_TUNE_BAND_3C00_MASK) == 0, "3C00 spills");
        CHECK((camera_tune_band_3c01(hz) & ~CAMERA_TUNE_BAND_3C01_MASK) == 0, "3C01 spills");
    }
}

int main(void)
{
    printf("=== camera tune ===\n");
    test_gain_ceiling();
    test_banding();
    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nAll camera tune tests passed\n");
    return 0;
}
