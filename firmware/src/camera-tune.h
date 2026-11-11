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

/*
 * Shutter cap: the AEC's maximum exposure, so it reaches for gain (now that
 * gain has a real ceiling) before it reaches for blur.
 *
 * {0x3A02, 0x3A03} is the 60 Hz maximum and {0x3A14[3:0], 0x3A15} the 50 Hz
 * one (16 bits, big-endian); camera.c writes both, so the cap holds under
 * either banding setting. In lines: the driver and OmniVision's own init
 * tables write 0x03D8 = 984 = VTS there, "one frame". (The datasheet's reset
 * default 0x3D80 is 984 x 16, which would mean 1/16-line units; if the
 * scan log's "exp" never rises above cap/16 lines, that is the reading, and
 * the cap should be multiplied by 16. TODO(board): confirm.)
 *
 * Line time is 1 / (sensor fps x VTS): at the ~8-12 fps the driver's QVGA PLL
 * gives (to be measured: bench fps with no code in view), 85-128 us, so 96
 * lines is about 8-12 ms, under one 60 Hz band (8.3 ms at the faster
 * estimate) and over the 2-4 ms a PWM-dimmed phone OLED needs to not band.
 * Clamped to 1..vts: more than a frame stretches the frame.
 */
uint16_t camera_tune_shutter_cap(unsigned lines, unsigned vts);

/*
 * Centre-weighted AEC: 0x5688-0x568F hold a 4x4 grid of 4-bit zone weights,
 * two zones a register, "Window r(c+1) weight" in bits 7:4 and "Window rc"
 * in bits 3:0 (datasheet, AEC weight table; reset 0x11 everywhere, i.e.
 * flat). camera_tune_aec_pack() turns grid[row][col] into the eight
 * register values. CAMERA_TUNE_AEC_CENTRE is 15 on the middle 2x2 and 1
 * elsewhere: the phone in the middle of the view sets the exposure and the
 * dark room around it barely counts. Symmetric, so it does not matter
 * whether the datasheet's "rc" is row-column or column-row.
 */
void camera_tune_aec_pack(const uint8_t grid[4][4], uint8_t out[8]);
extern const uint8_t CAMERA_TUNE_AEC_CENTRE[4][4];

/*
 * The knob's exposure bias (-5..0, as the Scan screen shows it) to the
 * sensor's AE level (-5..5, ov5640.c set_ae_level(): target (level+5)*10+5).
 *
 * The bias was tuned with whole-frame metering, where a screen filling a
 * quarter of the view at target 15 (bias -4) landed at about 60/255. Centre-
 * weighted, the screen IS most of the metered average, and the same target
 * would leave it at about 18: black. So with centre weighting the sensor
 * gets bias + offset (3 by default: -4 -> -1, target 45, the screen ~54/255,
 * close to what the user tuned), and the knob keeps its scale and its
 * default. Clamped to -5..5.
 */
int camera_tune_ae_level(int bias, bool centre_weighted, int offset);

/*
 * Digital zoom on the sensor: the Scan screen's knob push.
 *
 * The output is always QVGA 320x240 grayscale; what changes is how much of
 * the 2624x1952 pixel array feeds it. The OV5640 reads a window of the array
 * (X/Y_ADDR_ST/END, 0x3800-0x3807), bins it 2x2 (as the driver does for
 * every mode up to 1280x960), drops an ISP offset from each edge
 * (0x3810-0x3813) and scales what is left down to the output (0x3808-0x380B,
 * ISP_CONTROL_01 bit 5). So each zoom level is real sensor pixels, always
 * DOWNscaled - never an interpolated upscale - and quirc's cost is the same
 * 320x240 at every level.
 *
 * The windows, centred on the array (1312, 976), binned, offsets 16/8:
 *
 *   level  window (sx,sy)-(ex,ey)   binned - offsets   to 320x240   field
 *   1x     (0,0)-(2623,1951)        1280 x 960         4:1          full
 *   2x     (640,480)-(1983,1471)    640 x 480          2:1          1/2
 *   3x     (800,600)-(1823,1351)    480 x 360          3:2          1/2.67
 *
 * 1x is exactly the driver's own QVGA (ratio_table[4:3], binned). "3x" is
 * 2.67x of the field: the nearest window whose binned size scales to
 * 320x240 by a simple ratio (3 sensor pixels per output pixel, 2x2 binned
 * then 3:2), where a true 3x (853.3 x 640) has none.
 *
 * HTS stays the driver's 2060. VTS (ty) is the driver's 984 at every level
 * unless built with -DCAMERA_ZOOM_VTS=N for 2x and 3x (item 8 of the tuning
 * list): a zoomed window reads half or fewer of the rows, so it can run at a
 * shorter frame - try 700, then 600 - for more frames a second. Even at 984
 * the zoomed rolling readout is half as long (fewer rows each line time),
 * which already tears fewer animated frames. TODO(board): measure fps with
 * no code in view at each VTS; back off on any capture timeout.
 *
 * Expected on the board: at 2x a phone code about twice as wide on row 6's
 * viewfinder, "s" (located) at a distance where 1x found nothing, and
 * sender fragments of 120-150 bytes decoding where 1x managed 50-90.
 */
typedef struct {
    const char *label;          /* row 6: "1x", "2x", "3x" */
    uint16_t sx, sy, ex, ey;    /* array window, inclusive */
    uint16_t ox, oy;            /* ISP offset, binned pixels, each edge */
    uint16_t tx, ty;            /* HTS, VTS */
    uint16_t ow, oh;            /* output: always 320 x 240 */
    bool     binning, scale;
} CameraZoom;

#define CAMERA_ZOOM_COUNT  3
#define CAMERA_ARRAY_W     2624
#define CAMERA_ARRAY_H     1952

/** Level 0..CAMERA_ZOOM_COUNT-1; out of range is level 0 (1x). */
const CameraZoom *camera_tune_zoom(uint8_t level);
/** The push: 1x -> 2x -> 3x -> 1x. */
uint8_t camera_tune_zoom_next(uint8_t level);

#endif /* LEEK_CAMERA_TUNE_H */
