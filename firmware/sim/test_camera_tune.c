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

static void test_shutter_cap(void)
{
    printf("Shutter cap\n");
    CHECK(camera_tune_shutter_cap(96, 984) == 96, "96 of 984");
    CHECK(camera_tune_shutter_cap(2000, 984) == 984, "a cap over one frame is not clamped");
    CHECK(camera_tune_shutter_cap(0, 984) == 1, "a zero cap is not one line");
    CHECK(camera_tune_shutter_cap(5, 0) == 1, "no VTS");
}

static void test_aec_weights(void)
{
    printf("AEC weights\n");
    uint8_t r[8];
    camera_tune_aec_pack(CAMERA_TUNE_AEC_CENTRE, r);
    static const uint8_t want[8] = { 0x11, 0x11, 0xF1, 0x1F, 0xF1, 0x1F, 0x11, 0x11 };
    CHECK(!memcmp(r, want, 8), "centre: %02x %02x %02x %02x %02x %02x %02x %02x",
          r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    /* The nibble order, on an asymmetric grid: window 00 in 0x5688[3:0],
       01 in [7:4], 02 in 0x5689[3:0], 33 in 0x568F[7:4]. */
    uint8_t g[4][4];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) g[y][x] = (uint8_t)(y * 4 + x);
    camera_tune_aec_pack((const uint8_t (*)[4])g, r);
    CHECK(r[0] == 0x10 && r[1] == 0x32 && r[2] == 0x54 && r[7] == 0xFE,
          "order: %02x %02x %02x .. %02x", r[0], r[1], r[2], r[7]);
    /* The centre grid is its own transpose. */
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        CHECK(CAMERA_TUNE_AEC_CENTRE[y][x] == CAMERA_TUNE_AEC_CENTRE[x][y], "not symmetric");
}

static void test_ae_level(void)
{
    printf("AE level\n");
    CHECK(camera_tune_ae_level(-4, false, 3) == -4, "flat metering moved the bias");
    CHECK(camera_tune_ae_level(-4, true, 3) == -1, "centre: -4 -> %d", camera_tune_ae_level(-4, true, 3));
    CHECK(camera_tune_ae_level(0, true, 3) == 3, "centre: 0 -> %d", camera_tune_ae_level(0, true, 3));
    CHECK(camera_tune_ae_level(-5, true, 3) == -2, "centre: -5");
    CHECK(camera_tune_ae_level(0, true, 9) == 5, "not clamped high");
    CHECK(camera_tune_ae_level(-9, false, 0) == -5, "not clamped low");
    /* Monotonic: the knob still means brighter one way. */
    for (int b = -5; b < 0; b++)
        CHECK(camera_tune_ae_level(b, true, 3) < camera_tune_ae_level(b + 1, true, 3), "not monotonic at %d", b);
}

int main(void)
{
    printf("=== camera tune ===\n");
    test_gain_ceiling();
    test_banding();
    test_shutter_cap();
    test_aec_weights();
    test_ae_level();
    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nAll camera tune tests passed\n");
    return 0;
}
