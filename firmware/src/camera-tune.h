/**
 * The OV5640 register values the Scan screen's camera profile writes, as pure
 * functions and tables so the host suite can check them (sim/test_camera_tune.c).
 *
 * camera.c applies them through the driver's set_reg()/set_res_raw(); nothing
 * here touches hardware. Every value is derived from the OV5640 datasheet's
 * register descriptions (see docs/JOURNAL.md, "Camera tuning") and still has
 * to be confirmed on a board; each section says how.
 */
#ifndef LEEK_CAMERA_TUNE_H
#define LEEK_CAMERA_TUNE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * AGC gain ceiling, 0x3A18[1:0]/0x3A19 (10 bits, big-endian).
 *
 * The register holds "real gain" with four fractional bits: 0x10 is 1x, 0x40
 * 4x, 0x3FF the 63.9x maximum. The stock driver's set_gainceiling() writes
 * its gainceiling_t enum straight in, and GAINCEILING_4X is the enum value 1:
 * a ceiling of 1/16x, so AGC was pinned at its minimum and every bit of
 * brightness came from shutter time, which is motion blur. camera.c writes
 * this value instead.
 *
 * Expected on the board: the once-a-second "gain" figure in the scan log
 * rises above 1.0x on a dim screen (it never did before), "exp" lines fall,
 * and decodes/s at phone brightness 50% go up.
 */
#define CAMERA_TUNE_GAIN_REG_MAX  0x3FF
uint16_t camera_tune_gain_ceiling_reg(unsigned times);

/*
 * Banding filter: which mains frequency the exposure steps avoid beating with.
 *
 * 0x3C01 bit 7 selects manual (1) or automatic (0) detection; the driver's
 * defaults leave it manual (0x3C01 = 0xA4). In manual mode 0x3C00 bit 2
 * picks the frequency: 1 for 50 Hz, 0 for 60 Hz. Confirmed against the
 * driver's own "//50Hz {0x3c00, 0x04}" default and Linux's ov5640.c
 * (ov5640_set_ctrl_light_freq sets CTRL00 BIT(2) only for 50 Hz). The
 * driver therefore ran at 50 Hz; Mexico's mains is 60 Hz.
 *
 * `hz` is 50, 60 or 0 for automatic detection; anything else is 60.
 * Values to OR in under the masks CAMERA_TUNE_BAND_3C00_MASK /
 * CAMERA_TUNE_BAND_3C01_MASK.
 *
 * Expected on the board: no rolling dark bands on a printed code under room
 * LED or fluorescent light. A phone or monitor is self-lit and barely
 * changes. The band step registers (0x3A08/09 for 50 Hz, 0x3A0A/0B for 60 Hz)
 * are left at the driver's values, which were computed for its reference
 * timing, not for our QVGA PLL; with the shutter capped under one band
 * (below) they hardly matter. TODO(board): once the sensor's real line time
 * is measured, set 0x3A0A/0B = 8333 us / line time.
 */
#define CAMERA_TUNE_BAND_3C00_MASK  0x04
#define CAMERA_TUNE_BAND_3C01_MASK  0x80
uint8_t camera_tune_band_3c00(unsigned hz);
uint8_t camera_tune_band_3c01(unsigned hz);

#endif /* LEEK_CAMERA_TUNE_H */
