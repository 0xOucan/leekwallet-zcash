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

#endif /* LEEK_CAMERA_TUNE_H */
