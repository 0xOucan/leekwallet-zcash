/* See camera.h. */

#include "camera.h"

#include "board.h"
#include "camera-tune.h"

#include <string.h>

/*
 * LEEK_CAMERA_DRIVER is set by src/CMakeLists.txt, and only when the build was
 * configured with the camera components present. It is a separate question
 * from LEEK_HAS_CAMERA, which says what the *hardware* has: the QEMU target
 * for the CAM board has the pin map and no sensor behind it, so it wants the
 * board's pins and none of this driver. Defaulting it to 0 means a build that
 * never heard of it compiles the no-camera path rather than failing on an
 * undefined identifier.
 */
#ifndef LEEK_CAMERA_DRIVER
#  define LEEK_CAMERA_DRIVER 0
#endif

#if LEEK_CAMERA_DRIVER && !LEEK_HAS_CAMERA
#  error "camera driver requested on a board whose map has no sensor"
#endif

/* Settings > Preview on the colour kit: the raw exposure instead of the
   auto-levelled view. Not secret, kept by ui.c in NVS. */
static bool preview_true;

void camera_set_preview_true_exposure(bool on) { preview_true = on; }
bool camera_preview_true_exposure(void) { return preview_true; }

#if LEEK_CAMERA_DRIVER

#include <string.h>

#include "esp_camera.h"
#include "esp_camera_af.h"

#ifdef CONFIG_CAMERA_AF_SUPPORT
#  define CONFIG_CAMERA_AF_ENABLED 1
#else
#  define CONFIG_CAMERA_AF_ENABLED 0
#endif
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "quirc.h"

#include "viewfinder.h"
#include "scan-timing.h"

#include "freertos/FreeRTOS.h"

static const char *TAG = "camera";

/*
 * Per-stage frame timing (scan-timing.h), one line a second over USB serial.
 * Under a spinlock because the UI task adds its draw time from another task
 * than the one decoding once decode has its own task, and a 64-bit add is
 * not atomic on this core.
 */
static ScanTiming timing;
static portMUX_TYPE timing_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t timing_window_us;
static uint32_t timing_window_frames;

static void timing_add(scan_stage_t stage, int64_t t0_us)
{
    const int64_t dt = esp_timer_get_time() - t0_us;
    portENTER_CRITICAL(&timing_mux);
    scan_timing_add(&timing, stage, dt > 0 ? (uint32_t)dt : 0);
    portEXIT_CRITICAL(&timing_mux);
}

/*
 * QVGA grayscale, which is what both halves of this file want.
 *
 * Grayscale rather than JPEG or RGB565 because quirc wants exactly one byte of
 * luminance per pixel: any other format would be captured and then converted,
 * paying for the conversion in the same CPU quirc is waiting for. QVGA rather
 * than VGA because quirc's cost is dominated by the identify stage and that
 * stage grows with pixel count, and because 320x240 is enough resolution to
 * read the QR versions a phone animates from a comfortable distance. If bench
 * numbers from the board say otherwise, this is the constant to move.
 */
/*
 * VGA, not QVGA.
 *
 * The first bench frames settled this: at 320x240 a companion's request code
 * filled about 160 pixels, and with that many modules each one was barely two
 * camera pixels. quirc located nothing at all - not blur, simply not enough
 * pixels to see modules with. VGA doubles the linear resolution, so the same
 * distance gives four to six pixels per module.
 *
 * The cost is quirc's image plane and the decode, both four times larger:
 * about 1.2 MB of PSRAM against 8 MB, and fewer frames per second. Frames are
 * the cheaper thing to spend here, because a frame that cannot resolve a
 * module is worth nothing however many of them arrive.
 */
/*
 * Capture size, stepped with OK on the scan screen for bench comparison.
 * QVGA is the default: compared on the bench at exposure -4 against VGA and
 * SVGA, it read best - four times fewer pixels per frame, so several times
 * the frames a second, and the companion's medium fragments still resolve
 * with the phone held close. Applied by restarting the camera.
 */
/*
 * 2026-11-10: the Scan screen no longer steps the size. Its output is always
 * QVGA, and the push zooms on the sensor instead (camera_next_zoom(),
 * camera-tune.h): the same 320x240 decode cost with two or more times the
 * real pixels per module, where VGA and SVGA quadrupled the cost for the
 * same field of view. VGA and SVGA stay for measurement, in the bench image
 * only: pio run -e esp32s3cam-bench with -DCAMERA_BENCH_FRAME_W=640 (or 800),
 * and -DCAMERA_BENCH_ZOOM=1 (2x) or 2 (3x) for a zoom at QVGA.
 */
static const struct { uint16_t w, h; framesize_t size; } FRAME_MODES[] = {
    { 320, 240, FRAMESIZE_QVGA },
    { 640, 480, FRAMESIZE_VGA  },
    { 800, 600, FRAMESIZE_SVGA },
};
#if defined(LEEK_QR_BENCH) && LEEK_QR_BENCH && defined(CAMERA_BENCH_FRAME_W)
#  if CAMERA_BENCH_FRAME_W == 640
#    define FRAME_MODE_START 1
#  elif CAMERA_BENCH_FRAME_W == 800
#    define FRAME_MODE_START 2
#  else
#    define FRAME_MODE_START 0
#  endif
#else
#  define FRAME_MODE_START 0
#endif
#if defined(LEEK_QR_BENCH) && LEEK_QR_BENCH && defined(CAMERA_BENCH_ZOOM)
#  define ZOOM_START CAMERA_BENCH_ZOOM
#else
#  define ZOOM_START 0
#endif
static const uint8_t frame_mode = FRAME_MODE_START;
/* The zoom level (camera-tune.h), 1x every time the camera starts. */
static uint8_t zoom_level;
#define FRAME_W     (FRAME_MODES[frame_mode].w)
#define FRAME_H     (FRAME_MODES[frame_mode].h)
#define FRAME_SIZE  (FRAME_MODES[frame_mode].size)

/*
 * How often a captured frame is also turned into a preview.
 *
 * No frame is ever *spent* on the viewfinder: the preview is rendered from the
 * same frame that was just handed to quirc, so the decode loop never gives up
 * a capture for it. What it does cost is the downsample itself - 2 x 7168
 * sampled reads - on one frame in four. That ratio is here rather than inline
 * so it can be named in a test and moved with one edit if the bench says the
 * preview is measurably slowing the decode rate.
 *
 * Overridable from the build, and 0 turns the preview off entirely, so the
 * bench can answer "what does the viewfinder cost?" as a measurement rather
 * than an argument:
 *
 *   pio run -e esp32s3cam-bench                                  preview on
 *   pio run -e esp32s3cam-bench -a "--build-flag=-DCAMERA_PREVIEW_EVERY=0"
 *
 * Two runs, two frames/s numbers, and the difference is the answer.
 */
#ifndef CAMERA_PREVIEW_EVERY
#  define CAMERA_PREVIEW_EVERY   4
#endif

/* The AGC ceiling, in whole x (camera-tune.h). 4x: noise lands on the module
   edges quirc thresholds, but blur from a long shutter is worse. */
#ifndef CAMERA_GAIN_CEILING_X
#  define CAMERA_GAIN_CEILING_X  4
#endif

/* Mains frequency for the banding filter: 60 (Mexico, the Americas), 50, or
   0 for the sensor's automatic detection (camera-tune.h). */
#ifndef CAMERA_MAINS_HZ
#  define CAMERA_MAINS_HZ  60
#endif

/*
 * Shutter cap in lines (camera-tune.h): 96 ~ 8-12 ms at the driver's QVGA
 * timing. 0 leaves the driver's one-frame maximum. Measure: the scan log's
 * "exp" stays at or under this, "gain" makes up the rest, and the PC stream
 * of a phone moved by hand smears less.
 */
#ifndef CAMERA_SHUTTER_CAP_LINES
#  define CAMERA_SHUTTER_CAP_LINES  96
#endif
/* The driver's VTS for every binned mode (QVGA..SVGA): 1968 / 2. */
#define CAMERA_DRIVER_VTS  984

/*
 * Centre-weighted metering, and the offset that keeps the knob's -5..0 scale
 * landing where the user tuned it (camera-tune.h). 0 restores flat metering
 * and the bias as it was. Measure: with a phone filling the middle of the
 * view, the row-5 level stays green (25-60%) whether the room is lit or not.
 */
#ifndef CAMERA_AEC_CENTRE
#  define CAMERA_AEC_CENTRE  1
#endif
#ifndef CAMERA_AEC_BIAS_OFFSET
#  define CAMERA_AEC_BIAS_OFFSET  3
#endif

static int sensor_ae_level(int8_t bias)
{
    return camera_tune_ae_level(bias, CAMERA_AEC_CENTRE != 0, CAMERA_AEC_BIAS_OFFSET);
}

static bool apply_zoom(uint8_t level);

/*
 * 240 MHz while the camera runs, 160 otherwise.
 *
 * The CAM board runs at 160 MHz because 240 got it very hot beside the
 * OV5640 (sdkconfig.s3cam). quirc is CPU-bound, so the Scan screen is the
 * one place 240 pays: up to 1.5x on identify and decode. Dynamic frequency
 * scaling (CONFIG_PM_ENABLE, sdkconfig.s3cam) between 160 and 240 only -
 * the floor is 160, never XTAL, and light sleep stays off, so the APB stays
 * at 80 MHz and no peripheral sees a clock change; only the CPU divider of
 * the 480 MHz PLL moves. The camera's XCLK comes from the fixed 160 MHz PLL
 * tap, not the CPU clock. A CPU_FREQ_MAX lock is held from camera_start()
 * to camera_stop(), so the heat is only while scanning.
 *
 * Measure: the scan log's "cpu 240" field, and `ident` falling to about
 * two-thirds of its 160 MHz value. Heat: touch the module after five
 * minutes on Scan. -DCAMERA_SCAN_CPU_MHZ=0 keeps 160 for an A/B.
 * A build whose generated sdkconfig.<env> predates this (PlatformIO does not
 * regenerate it) has no PM and logs that it stays at 160:
 * rm firmware/sdkconfig.esp32s3cam* and rebuild.
 */
#ifndef CAMERA_SCAN_CPU_MHZ
#  define CAMERA_SCAN_CPU_MHZ  240
#endif
#if CONFIG_PM_ENABLE && CAMERA_SCAN_CPU_MHZ > 0
#include "esp_pm.h"
static esp_pm_lock_handle_t cpu_lock;

static void scan_cpu_boost(bool on)
{
    static bool configured;
    if (!configured) {
        configured = true;
        const esp_pm_config_t pm = {
            .max_freq_mhz = CAMERA_SCAN_CPU_MHZ,
            .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .light_sleep_enable = false,
        };
        esp_err_t err = esp_pm_configure(&pm);
        if (err == ESP_OK) {
            err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "scan", &cpu_lock);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no scan-time CPU boost: %s", esp_err_to_name(err));
            cpu_lock = NULL;
        }
    }
    if (cpu_lock == NULL) {
        return;
    }
    if (on) {
        esp_pm_lock_acquire(cpu_lock);
    } else {
        esp_pm_lock_release(cpu_lock);
    }
}
#else
static void scan_cpu_boost(bool on)
{
    static bool said;
    if (on && !said && CAMERA_SCAN_CPU_MHZ > 0) {
        said = true;
        ESP_LOGW(TAG, "CONFIG_PM_ENABLE is off in this sdkconfig: the CPU stays at "
                      "%d MHz while scanning (rm sdkconfig.<env> and rebuild)",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }
}
#endif

static struct quirc *quirc_ctx;
static bool running;

/*
 * Static, not automatic. Together these are about 13 KB, and camera_next_qr()
 * is called from the UI task's loop; 13 KB of stack frame would overflow that
 * task and most others in this firmware. The bench in research/qr-spike puts
 * them on the stack because a host has megabytes of it.
 */
static struct quirc_code scan_code;
static struct quirc_data scan_data;

#if BOARD_HAS_SPI_PANEL
/* The colour kit's grayscale preview: 30 KB, in PSRAM (viewfinder.h). Taken
   once and kept, so the UI can point the panel at it between frames. */
static uint8_t         *gray_preview;
static ViewfinderStats  gray_stats;
#else
static uint8_t  preview[VIEWFINDER_BYTES];
#endif
static bool     preview_fresh;
static uint32_t preview_tick;

static uint32_t stat_frames;
static uint32_t stat_decodes;
/* Codes quirc LOCATED but could not read. The difference between this and
   stat_decodes is the whole diagnosis on a bench: zero located means the code
   is too small, too dim or out of frame, while located-but-not-decoded means
   it is seen and the modules are not resolved - blur, glare, or a refresh
   caught mid-frame. */
static uint32_t stat_located;
/*
 * Bit 0 hmirror, bit 1 vflip. See camera.h: one of them alone mirrors the
 * image, which no decoder can read.
 *
 * 2 (vflip) is the unmirrored image on this module, confirmed on the bench
 * twice: codes decode with it and the first air-gapped signature was read at
 * it. 1 (hmirror) should in theory be the same image turned 180 degrees, but
 * on this sensor it showed a mirror image; the driver's bits do not map onto
 * the optics the way the names suggest. Change it only against the bench.
 */
static uint8_t orientation = 2;

/* -4 by default, stepped from the scan screen. -5 read fastest against a
   phone at half brightness, but at -5 the view went black within seconds
   of opening the scanner; -4 is the user's pick after bench testing. */
static int8_t exposure_bias = -4;

/*
 * Exposure stays automatic throughout; only its target is biased. Locking it
 * on the first decode froze the device: switching this sensor to manual
 * exposure applies the manual exposure registers, not the value automatic
 * exposure had just settled on, and what those held stretched each frame
 * until capture timed out.
 */

/* Live preview to the PC viewer, off until OK on the scan screen asks. */
static bool streaming;
/* The centre of the frame at native resolution: 103 KB of base64, which
   the cable carries a couple of times a second. */
#define STREAM_W          320
#define STREAM_H          240
#define STREAM_PERIOD_US  (400 * 1000)
static void emit_frame(const uint8_t *buf, unsigned w, unsigned x0, unsigned y0,
                       unsigned ow, unsigned oh, unsigned step);

/*
 * Free the camera's control bus before probing it.
 *
 * This board has no power-down or reset pin wired, so the OV5640 stays
 * powered through a chip reset, a flash or a panic. A reset that lands while
 * the sensor is mid-reply leaves it holding SDA low, waiting for clocks that
 * never come, and every probe after that fails: "No camera" until the board
 * is unplugged. The standard I2C recovery: clock SCL until the sensor lets go
 * of SDA (at most nine bits), then a STOP. Harmless on a healthy bus.
 * Returns whether SDA ended up released.
 */
static bool sccb_recover(void)
{
    const gpio_num_t sda = PIN_CAM_SIOD, scl = PIN_CAM_SIOC;
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    const int sda_before = gpio_get_level(sda), scl_before = gpio_get_level(scl);
    for (int i = 0; i < 9 && gpio_get_level(sda) == 0; i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(10);
    }
    /* STOP: SDA rises while SCL is high. */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(10);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(10);
    const bool ok = gpio_get_level(sda) == 1 && gpio_get_level(scl) == 1;
    ESP_LOGI(TAG, "sccb bus before probe: SDA %d SCL %d -> %s", sda_before,
             scl_before, ok ? "free" : "still held");
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    return ok;
}

/*
 * What the sensor's AEC/AGC settled on, read back once a second: exposure in
 * lines (0x3500-0x3502, line x 16) and the real gain (0x350A/B, 0x10 = 1x),
 * printed as gain x10. These are the numbers that show whether the gain
 * ceiling and the shutter cap are doing what camera_start() asked.
 */
static void sensor_exposure(unsigned *lines, unsigned *gain_x10)
{
    *lines = 0;
    *gain_x10 = 0;
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL || sensor->get_reg == NULL) {
        return;
    }
    const int e0 = sensor->get_reg(sensor, 0x3500, 0xFF);
    const int e1 = sensor->get_reg(sensor, 0x3501, 0xFF);
    const int e2 = sensor->get_reg(sensor, 0x3502, 0xFF);
    const int g0 = sensor->get_reg(sensor, 0x350A, 0xFF);
    const int g1 = sensor->get_reg(sensor, 0x350B, 0xFF);
    if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
        *lines = ((unsigned)(e0 & 0x0F) << 12 | (unsigned)e1 << 4 | (unsigned)e2 >> 4);
    }
    if (g0 >= 0 && g1 >= 0) {
        *gain_x10 = (((unsigned)(g0 & 0x03) << 8) | (unsigned)g1) * 10u / 16u;
    }
}

/*
 * One line a second while scanning, so a bench session is diagnosable over
 * the cable without swapping in the benchmark image. `s` (located) is the
 * useful count: none means the code is too small, too dim or out of frame;
 * located without `r` (decodes) means it is seen and its modules are not
 * resolved. Nothing here is derived from a decoded payload.
 */
static void timing_log_maybe(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (timing_window_us == 0) {
        timing_window_us = now_us;
        timing_window_frames = stat_frames;
        return;
    }
    const int64_t dt = now_us - timing_window_us;
    if (dt < 1000000) {
        return;
    }
    ScanTiming snap;
    portENTER_CRITICAL(&timing_mux);
    snap = timing;
    scan_timing_reset(&timing);
    portEXIT_CRITICAL(&timing_mux);

    char stages[128];
    scan_timing_format(&snap, stages, sizeof stages);
    const uint32_t fps_x10 = (uint32_t)((uint64_t)(stat_frames - timing_window_frames) *
                                        10000000u / (uint64_t)dt);
    unsigned lines = 0, gain_x10 = 0;
    sensor_exposure(&lines, &gain_x10);
    ESP_LOGI(TAG, "scan %s %u.%u fps | %s ms | s:%u r:%u | exp %u lines gain %u.%ux | cpu %u",
             FRAME_W == 320 ? camera_tune_zoom(zoom_level)->label
                            : (FRAME_W == 640 ? "VGA" : "SVGA"),
             (unsigned)(fps_x10 / 10), (unsigned)(fps_x10 % 10), stages,
             (unsigned)stat_located, (unsigned)stat_decodes,
             lines, gain_x10 / 10, gain_x10 % 10,
             (unsigned)esp_rom_get_cpu_ticks_per_us());
    timing_window_us = now_us;
    timing_window_frames = stat_frames;
}

#ifndef BOARD_VIEWFINDER_ROTATE
#define BOARD_VIEWFINDER_ROTATE 0
#endif

bool camera_start(void)
{
    viewfinder_set_rotation(BOARD_VIEWFINDER_ROTATE);
    if (running) {
        return true;
    }

    /*
     * Pins come from board.h and are not retyped here. The CAM board's map was
     * continuity-checked once (docs/BOARD-S3CAM-PINOUT.md); a second copy of
     * fifteen GPIO numbers is a second thing to get wrong, and the one that
     * disagreed would be this one.
     */
    const camera_config_t cfg = {
        .pin_pwdn       = PIN_CAM_PWDN,
        .pin_reset      = PIN_CAM_RESET,
        .pin_xclk       = PIN_CAM_XCLK,
        .pin_sccb_sda   = PIN_CAM_SIOD,
        .pin_sccb_scl   = PIN_CAM_SIOC,
        .pin_d7         = PIN_CAM_D7,
        .pin_d6         = PIN_CAM_D6,
        .pin_d5         = PIN_CAM_D5,
        .pin_d4         = PIN_CAM_D4,
        .pin_d3         = PIN_CAM_D3,
        .pin_d2         = PIN_CAM_D2,
        .pin_d1         = PIN_CAM_D1,
        .pin_d0         = PIN_CAM_D0,
        .pin_vsync      = PIN_CAM_VSYNC,
        .pin_href       = PIN_CAM_HREF,
        .pin_pclk       = PIN_CAM_PCLK,

        .xclk_freq_hz   = 20000000,
        .ledc_timer     = LEDC_TIMER_0,
        .ledc_channel   = LEDC_CHANNEL_0,

        .pixel_format   = PIXFORMAT_GRAYSCALE,
        .frame_size     = FRAME_SIZE,

        /* Two buffers and LATEST: the decoder wants the newest view of the
           companion's screen, not a queue of stale ones. A backlog would make
           every decoded fragment older than the frame being animated, which is
           exactly the wrong thing when the sender has already moved on. */
        .fb_count       = 2,
        .fb_location    = CAMERA_FB_IN_PSRAM,
        .grab_mode      = CAMERA_GRAB_LATEST,
    };

    sccb_recover();
    /* No retry. The bench case this was for had the bus free and a sensor
       that answered nothing at all, which only a power cycle clears; a
       second probe only doubled a 17 s freeze. */
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        /* Not fatal, and deliberately not an abort. A board with no ribbon
           seated, or none fitted at all, must still show a Scan screen that
           says so - see camera.h. */
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        return false;
    }

    /*
     * Mirror and flip, set rather than inherited.
     *
     * We took whatever the sensor powered up with, and OV5640 modules commonly
     * come up mirrored. A 180-degree rotation is harmless - the finder patterns
     * tell a decoder which way is up - but a MIRRORED code is not a QR code at
     * all, and quirc will never read one however sharp it is. That is a fault
     * with no symptom except silence, which is what the bench saw: a clear
     * picture on the panel and nothing ever decoded.
     *
     * Both off. What matters is that the count is even: both on is a rotation,
     * one on is a mirror. If the module is physically mounted upside down the
     * picture will be too, and that costs nothing.
     */
    camera_set_orientation(orientation);
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        /*
         * Expose for the screen, not for the room.
         *
         * The first VGA frames off this board were smeared into streaks: a
         * dark room, so automatic exposure held the shutter open, and a
         * handheld device turned that into motion blur across every frame.
         * quirc located nothing, and no amount of resolution fixes a smear.
         *
         * What this camera is always pointed at is a phone or a monitor, which
         * is far brighter than the room around it. Biasing exposure down lets
         * the screen land correctly exposed with a much shorter shutter, which
         * is what stops the blur; the room going dark around it costs nothing,
         * since nothing there needs reading. Gain control stays on to make up
         * the difference on dimmer screens. How far down is exposure_bias,
         * stepped from the scan screen.
         */
        if (sensor->set_gain_ctrl != NULL)     { sensor->set_gain_ctrl(sensor, 1); }
        if (sensor->set_exposure_ctrl != NULL) { sensor->set_exposure_ctrl(sensor, 1); }
        if (sensor->set_ae_level != NULL)      { sensor->set_ae_level(sensor, sensor_ae_level(exposure_bias)); }

        /*
         * A QR profile, not a photo profile: nothing here is looked at by a
         * person except through a 128x64 preview. Night mode off, because it
         * exists to choose long exposures. Gain capped at 4x (below), because gain
         * noise lands exactly on the module edges quirc thresholds. Contrast
         * up to separate black from white modules, a little sharpening and a
         * little denoise - more of either softens or haloes the edges. White
         * balance off: the frame is grayscale and one fewer automatic loop
         * moving the picture between frames is one fewer cause of a miss.
         */
        if (sensor->set_aec2 != NULL)          { sensor->set_aec2(sensor, 0); }
        /*
         * Not set_gainceiling(): the vendored driver writes its enum value
         * raw into 0x3A18/19, and GAINCEILING_4X is 1, which in the
         * register's real-gain format (0x10 = 1x) is a 1/16x ceiling. That
         * pinned AGC at its minimum from the first bench session on, so all
         * brightness came from shutter time - the blur the exposure bias was
         * then fighting. A real CAMERA_GAIN_CEILING_X, through set_reg()
         * (camera-tune.h). Measure: the scan log's "gain" now climbs above
         * 1.0x on a dim screen and "exp" falls; try 8 with
         * -DCAMERA_GAIN_CEILING_X=8 for dim phones.
         */
        if (sensor->set_reg != NULL) {
            sensor->set_reg(sensor, 0x3A18, 0x3FF,
                            camera_tune_gain_ceiling_reg(CAMERA_GAIN_CEILING_X));
        }
        /* Banding at the local mains frequency, not the driver's fixed
           50 Hz. Measure: a printed code under room LED light shows no
           rolling dark bands in the PC stream. */
        if (sensor->set_reg != NULL) {
            sensor->set_reg(sensor, 0x3C01, CAMERA_TUNE_BAND_3C01_MASK,
                            camera_tune_band_3c01(CAMERA_MAINS_HZ));
            sensor->set_reg(sensor, 0x3C00, CAMERA_TUNE_BAND_3C00_MASK,
                            camera_tune_band_3c00(CAMERA_MAINS_HZ));
        }
        /* Cap the shutter, both banding variants, and weight the metering
           to the middle of the view. Exposure stays automatic: no manual
           value that could stretch a frame (see above). */
        if (sensor->set_reg != NULL && CAMERA_SHUTTER_CAP_LINES > 0) {
            const uint16_t cap = camera_tune_shutter_cap(CAMERA_SHUTTER_CAP_LINES,
                                                         CAMERA_DRIVER_VTS);
            sensor->set_reg(sensor, 0x3A02, 0xFFFF, cap);
            sensor->set_reg(sensor, 0x3A14, 0x0FFF, cap);
        }
        if (sensor->set_reg != NULL && CAMERA_AEC_CENTRE) {
            uint8_t w[8];
            camera_tune_aec_pack(CAMERA_TUNE_AEC_CENTRE, w);
            for (int i = 0; i < 8; i++) {
                sensor->set_reg(sensor, 0x5688 + i, 0xFF, w[i]);
            }
        }
        if (sensor->set_contrast != NULL)      { sensor->set_contrast(sensor, 2); }
        if (sensor->set_sharpness != NULL)     { sensor->set_sharpness(sensor, 1); }
        if (sensor->set_denoise != NULL)       { sensor->set_denoise(sensor, 1); }
        if (sensor->set_bpc != NULL)           { sensor->set_bpc(sensor, 1); }
        if (sensor->set_wpc != NULL)           { sensor->set_wpc(sensor, 1); }
        if (sensor->set_raw_gma != NULL)       { sensor->set_raw_gma(sensor, 1); }
        if (sensor->set_lenc != NULL)          { sensor->set_lenc(sensor, 1); }
        if (sensor->set_whitebal != NULL)      { sensor->set_whitebal(sensor, 0); }
        if (sensor->set_awb_gain != NULL)      { sensor->set_awb_gain(sensor, 0); }

        /*
         * Autofocus, if this module has the motor for it.
         *
         * The lens otherwise stays wherever it powered up, which is not where
         * a QR held at arm's length is: the first sharp frames off this board
         * still had soft modules, greys where blacks should be. Continuous
         * mode, because the user moves the device until it reads and the lens
         * should follow rather than wait to be asked.
         */
        /* Guarded twice over: the call is compiled out unless the build asks
           for AF, because on a module with no focus motor it waited for a
           reply that never came and took camera start down with it. */
        if (CONFIG_CAMERA_AF_ENABLED && esp_camera_af_is_supported(sensor)) {
            const esp_camera_af_config_t af = {
                .mode = ESP_CAMERA_AF_MODE_AUTO,
                .step_size = 1,
                .range_min = 0,
                .range_max = 1023,
                .timeout_ms = 2000,
            };
            const esp_err_t af_err = esp_camera_af_init(sensor, &af);
            ESP_LOGI(TAG, "autofocus init: %s", esp_err_to_name(af_err));
        } else {
            ESP_LOGW(TAG, "this module has no autofocus; focus is fixed");
        }
        ESP_LOGI(TAG, "sensor 0x%04x, hmirror %d, vflip %d",
                 (unsigned)sensor->id.PID, sensor->status.hmirror,
                 sensor->status.vflip);
    }

    /* 1x on every start (the Scan screen opens at the full field); the
       bench image may start zoomed. */
    zoom_level = 0;
    if (ZOOM_START > 0 && ZOOM_START < CAMERA_ZOOM_COUNT && apply_zoom(ZOOM_START)) {
        zoom_level = ZOOM_START;
    }

    quirc_ctx = quirc_new();
    if (quirc_ctx == NULL || quirc_resize(quirc_ctx, FRAME_W, FRAME_H) < 0) {
        /* quirc's image and pixel planes are 77 KB each and land in PSRAM.
           Failing here means PSRAM did not come up, which is worth a log of
           its own because everything else on this board still works. */
        ESP_LOGE(TAG, "quirc_resize failed; is PSRAM up?");
        quirc_destroy(quirc_ctx);
        quirc_ctx = NULL;
        esp_camera_deinit();
        return false;
    }

    preview_fresh = false;
    preview_tick = 0;
#if BOARD_HAS_SPI_PANEL
    if (gray_preview == NULL) {
        gray_preview = heap_caps_malloc(VIEWFINDER_GRAY_BYTES, MALLOC_CAP_SPIRAM);
    }
    memset(&gray_stats, 0, sizeof gray_stats);
#endif
    stat_frames = 0;
    stat_decodes = 0;
    stat_located = 0;
    scan_timing_reset(&timing);
    timing_window_us = 0;
    scan_cpu_boost(true);
    running = true;
    ESP_LOGI(TAG, "camera up: %dx%d grayscale", FRAME_W, FRAME_H);
    return true;
}

void camera_stop(void)
{
    if (!running) {
        return;
    }
    running = false;
    streaming = false;
    /*
     * No software standby here, though the sensor runs warm. This board has
     * no power-down or reset pin wired, so the OV5640 stays powered through
     * a chip reset, and a sensor left in standby answered the next probe
     * with 0xffff: "No camera" until the board was unplugged.
     */
    esp_camera_deinit();
    quirc_destroy(quirc_ctx);
    quirc_ctx = NULL;
    scan_cpu_boost(false);
    preview_fresh = false;
#if BOARD_HAS_SPI_PANEL
    gray_stats.primed = false;
#else
    memset(preview, 0, sizeof preview);
#endif
}

bool camera_next_qr(char *out, size_t out_size, size_t *out_len)
{
    if (!running || out == NULL || out_size == 0) {
        return false;
    }

    const int64_t t_wait = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        return false;
    }
    timing_add(SCAN_STAGE_WAIT, t_wait);
    stat_frames++;

    /*
     * Optional: a frame every so often while scanning, without anyone pressing
     * anything. Off by default; built with -DCAMERA_AUTODUMP_MS=5000 for a
     * bench session, because pressing OK at the moment somebody on the other
     * end of the cable happens to be listening is a coordination problem that
     * wasted several attempts.
     */
#if defined(CAMERA_AUTODUMP_MS) && CAMERA_AUTODUMP_MS > 0
    {
        static int64_t last_dump_us;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_dump_us > (int64_t)CAMERA_AUTODUMP_MS * 1000) {
            last_dump_us = now_us;
            esp_camera_fb_return(fb);
            camera_dump_frame();
            return false;
        }
    }
#endif

    timing_log_maybe();

    bool got = false;

    if (fb->format == PIXFORMAT_GRAYSCALE &&
        fb->width == FRAME_W && fb->height == FRAME_H) {

        if (streaming) {
            static int64_t last_stream_us;
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_stream_us > STREAM_PERIOD_US) {
                last_stream_us = now_us;
                emit_frame(fb->buf, FRAME_W, (FRAME_W - STREAM_W) / 2,
                           (FRAME_H - STREAM_H) / 2, STREAM_W, STREAM_H, 1);
            }
        }

        if (CAMERA_PREVIEW_EVERY > 0 &&
            ++preview_tick % CAMERA_PREVIEW_EVERY == 0) {
            const int64_t t_prev = esp_timer_get_time();
#if BOARD_HAS_SPI_PANEL
            /* A millisecond or two at QVGA: one pass over the frame for the
               box average, one over the 30 KB preview for the stretch. */
            if (gray_preview != NULL) {
                viewfinder_render_gray(fb->buf, FRAME_W, FRAME_H, gray_preview,
                                       &gray_stats, preview_true);
                preview_fresh = true;
            }
#else
            viewfinder_render(fb->buf, FRAME_W, FRAME_H, preview);
            preview_fresh = true;
#endif
            timing_add(SCAN_STAGE_PREVIEW, t_prev);
        }

        /*
         * One copy, and it is not avoidable through quirc's public API: the
         * library owns its image plane, hands it out through quirc_begin() and
         * then overwrites it in place with the thresholded image during
         * identify. Pointing it at the driver's framebuffer would mean reaching
         * into quirc_internal.h and would also corrupt the buffer the driver is
         * about to reuse. 77 KB PSRAM to PSRAM is a small fraction of what the
         * identify stage on the very next line costs.
         */
        int w = 0, h = 0;
        const int64_t t_copy = esp_timer_get_time();
        uint8_t *img = quirc_begin(quirc_ctx, &w, &h);
        memcpy(img, fb->buf, (size_t)w * (size_t)h);
        timing_add(SCAN_STAGE_COPY, t_copy);
        const int64_t t_ident = esp_timer_get_time();
        quirc_end(quirc_ctx);
        timing_add(SCAN_STAGE_IDENTIFY, t_ident);

        const int n = quirc_count(quirc_ctx);
        stat_located += (uint32_t)n;
        const int64_t t_dec = esp_timer_get_time();
        for (int i = 0; i < n && !got; i++) {
            quirc_extract(quirc_ctx, i, &scan_code);
            if (quirc_decode(&scan_code, &scan_data) != QUIRC_SUCCESS) {
                continue;   /* blur, glare, a half-refreshed screen */
            }
            stat_decodes++;

            const size_t len = (size_t)scan_data.payload_len;

            /*
             * Anything that is not a UR is dropped here without a word. A
             * wallet pointed at the world sees Wi-Fi codes, URLs and product
             * labels, and none of them is a failure of anything - reporting
             * them upward would fill the status line with complaints about
             * whatever happened to be in frame. The decoder above is
             * case-insensitive about the scheme, so both spellings pass.
             */
            if (len < 3 || len + 1 > out_size ||
                (scan_data.payload[0] != 'u' && scan_data.payload[0] != 'U') ||
                (scan_data.payload[1] != 'r' && scan_data.payload[1] != 'R') ||
                scan_data.payload[2] != ':') {
                continue;
            }

            memcpy(out, scan_data.payload, len);
            out[len] = '\0';
            if (out_len != NULL) {
                *out_len = len;
            }
            got = true;
        }
        if (n > 0) {
            timing_add(SCAN_STAGE_DECODE, t_dec);
        }
    }

    esp_camera_fb_return(fb);
    return got;
}

void camera_note_draw_us(uint32_t us)
{
    if (!running) {
        return;
    }
    portENTER_CRITICAL(&timing_mux);
    scan_timing_add(&timing, SCAN_STAGE_DRAW, us);
    portEXIT_CRITICAL(&timing_mux);
}

void camera_set_orientation(uint8_t mode)
{
    orientation = (uint8_t)(mode & 0x03);
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        return;
    }
    if (sensor->set_hmirror != NULL) {
        sensor->set_hmirror(sensor, (orientation & 1) ? 1 : 0);
    }
    if (sensor->set_vflip != NULL) {
        sensor->set_vflip(sensor, (orientation & 2) ? 1 : 0);
    }
}

uint8_t camera_orientation(void) { return orientation; }

void camera_set_exposure_bias(int8_t level)
{
    exposure_bias = (int8_t)(level < -5 ? -5 : (level > 0 ? 0 : level));
    sensor_t *sensor = esp_camera_sensor_get();
    if (running && sensor != NULL && sensor->set_ae_level != NULL) {
        sensor->set_ae_level(sensor, sensor_ae_level(exposure_bias));
    }
}

int8_t camera_exposure_bias(void) { return exposure_bias; }

uint16_t camera_frame_width(void) { return FRAME_W; }

/*
 * Point the sensor's window at a zoom level, live: no restart, the
 * framebuffers stay 320x240. set_res_raw() also re-applies mirror and flip
 * (set_image_options()), so the orientation survives. A frame or two around
 * the switch may be mixed; quirc simply misses them.
 */
static bool apply_zoom(uint8_t level)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL || sensor->set_res_raw == NULL || FRAME_W != 320) {
        return level == 0;
    }
    const CameraZoom *z = camera_tune_zoom(level);
    const int err = sensor->set_res_raw(sensor, z->sx, z->sy, z->ex, z->ey, z->ox, z->oy,
                                        z->tx, z->ty, z->ow, z->oh, z->scale, z->binning);
    ESP_LOGI(TAG, "zoom %s: window (%u,%u)-(%u,%u) VTS %u -> %s", z->label,
             z->sx, z->sy, z->ex, z->ey, z->ty, err ? "FAILED" : "ok");
    return err == 0;
}

bool camera_next_zoom(void)
{
    if (!running) {
        return false;
    }
    const uint8_t next = camera_tune_zoom_next(zoom_level);
    if (apply_zoom(next)) {
        zoom_level = next;
        return true;
    }
    /* Back to the full field, which is the driver's own QVGA window. */
    (void)apply_zoom(0);
    zoom_level = 0;
    return false;
}

uint8_t camera_zoom(void) { return zoom_level; }
const char *camera_zoom_label(void) { return camera_tune_zoom(zoom_level)->label; }



/*
 * One grayscale image over the console as base64 between FRAME markers: an
 * ow x oh window at (x0, y0) of a frame `w` pixels wide, taking every
 * `step`-th pixel. The full dump is the whole frame at step 1; the live
 * stream is the centre 320x240 at step 1, the decoder's own pixels, because a
 * downscaled stream looked blurred for reasons that were the stream's and not
 * the camera's.
 */
static void emit_frame(const uint8_t *buf, unsigned w, unsigned x0, unsigned y0,
                       unsigned ow, unsigned oh, unsigned step)
{
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t total = (size_t)ow * oh;

    printf("FRAME_BEGIN %u %u\n", ow, oh);
    /* 57 input bytes per line keeps each printed line under 80 characters,
       which is what a serial monitor and a log capture both handle without
       wrapping surprises. */
    const size_t CHUNK = 57;
    #define PX(k) buf[((size_t)y0 + ((k) / ow) * step) * w + x0 + ((k) % ow) * step]
    for (size_t i = 0; i < total; i += CHUNK) {
        char line[80];
        size_t o = 0;
        for (size_t j = i; j < i + CHUNK && j < total; j += 3) {
            const uint32_t a = PX(j);
            const uint32_t b = (j + 1 < total) ? PX(j + 1) : 0;
            const uint32_t c = (j + 2 < total) ? PX(j + 2) : 0;
            const uint32_t v = (a << 16) | (b << 8) | c;
            line[o++] = B64[(v >> 18) & 0x3F];
            line[o++] = B64[(v >> 12) & 0x3F];
            line[o++] = (j + 1 < total) ? B64[(v >> 6) & 0x3F] : '=';
            line[o++] = (j + 2 < total) ? B64[v & 0x3F] : '=';
        }
        line[o] = '\0';
        printf("%s\n", line);
    }
    #undef PX
    printf("FRAME_END\n");
}

void camera_dump_frame(void)
{
    if (!running) {
        return;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL || fb->format != PIXFORMAT_GRAYSCALE) {
        if (fb != NULL) { esp_camera_fb_return(fb); }
        return;
    }
    emit_frame(fb->buf, fb->width, 0, 0, fb->width, fb->height, 1);
    esp_camera_fb_return(fb);
}

void camera_set_stream(bool on) { streaming = on; }
bool camera_streaming(void) { return streaming; }

#if BOARD_HAS_SPI_PANEL
const uint8_t *camera_preview_take(void) { return NULL; }

const uint8_t *camera_preview_gray_take(ViewfinderStats *stats)
{
    if (!preview_fresh || gray_preview == NULL) {
        return NULL;
    }
    preview_fresh = false;
    if (stats != NULL) { *stats = gray_stats; }
    return gray_preview;
}
#else
const uint8_t *camera_preview_take(void)
{
    if (!preview_fresh) {
        return NULL;
    }
    preview_fresh = false;
    return preview;
}

const uint8_t *camera_preview_gray_take(ViewfinderStats *stats)
{
    (void)stats;
    return NULL;
}
#endif

void camera_stats(uint32_t *frames, uint32_t *decodes, uint32_t *located)
{
    if (frames != NULL)  { *frames = stat_frames; }
    if (decodes != NULL) { *decodes = stat_decodes; }
    if (located != NULL) { *located = stat_located; }
}

#else /* !LEEK_CAMERA_DRIVER */

/*
 * No sensor on this board, or no driver in this build.
 *
 * The reference board and the Pixie have no camera at all. The QEMU target for
 * the CAM board has the pins and no silicon behind them - QEMU emulates no
 * image sensor - so it lands here too and its Scan screen says the camera did
 * not start, which is the truth there.
 */

#ifdef LEEK_HOST_TEST
/* A stand-in sensor for the UI suites (camera.h). */
static const uint8_t *test_frame;
static uint16_t       test_w, test_h;
static bool           test_running;
static uint32_t       test_frames, test_located, test_decodes;
static uint8_t        test_mono[VIEWFINDER_BYTES];
static uint8_t        test_gray[VIEWFINDER_GRAY_BYTES];
static ViewfinderStats test_stats;
static bool           test_mono_fresh, test_gray_fresh;

void camera__set_frame_for_test(const uint8_t *frame, uint16_t w, uint16_t h)
{
    test_frame = frame;
    test_w = w;
    test_h = h;
    if (frame == NULL) {
        test_running = false;
    }
}

void camera__set_located_for_test(uint32_t located, uint32_t decodes)
{
    test_located = located;
    test_decodes = decodes;
}

static uint8_t test_zoom;
bool camera_start(void)
{
    test_zoom = 0;
    test_running = test_frame != NULL;
    test_frames = 0;
    test_mono_fresh = test_gray_fresh = false;
    memset(&test_stats, 0, sizeof test_stats);
    return test_running;
}

void camera_stop(void) { test_running = false; }

bool camera_next_qr(char *out, size_t out_size, size_t *out_len)
{
    (void)out;
    (void)out_size;
    (void)out_len;
    if (!test_running || test_frame == NULL) {
        return false;
    }
    test_frames++;
    viewfinder_render(test_frame, test_w, test_h, test_mono);
    viewfinder_render_gray(test_frame, test_w, test_h, test_gray, &test_stats,
                           preview_true);
    test_mono_fresh = test_gray_fresh = true;
    return false;
}

const uint8_t *camera_preview_take(void)
{
    if (!test_mono_fresh) { return NULL; }
    test_mono_fresh = false;
    return test_mono;
}

const uint8_t *camera_preview_gray_take(ViewfinderStats *stats)
{
    if (!test_gray_fresh) { return NULL; }
    test_gray_fresh = false;
    if (stats != NULL) { *stats = test_stats; }
    return test_gray;
}

uint16_t camera_frame_width(void) { return test_running ? test_w : 0; }

void camera_stats(uint32_t *frames, uint32_t *decodes, uint32_t *located)
{
    if (frames != NULL)  { *frames = test_frames; }
    if (decodes != NULL) { *decodes = test_decodes; }
    if (located != NULL) { *located = test_located; }
}
#else
bool camera_start(void) { return false; }
void camera_stop(void) { }

bool camera_next_qr(char *out, size_t out_size, size_t *out_len)
{
    (void)out;
    (void)out_size;
    (void)out_len;
    return false;
}

const uint8_t *camera_preview_take(void) { return NULL; }

const uint8_t *camera_preview_gray_take(ViewfinderStats *stats)
{
    (void)stats;
    return NULL;
}

uint16_t camera_frame_width(void) { return 0; }

void camera_stats(uint32_t *frames, uint32_t *decodes, uint32_t *located)
{
    if (frames != NULL)  { *frames = 0; }
    if (decodes != NULL) { *decodes = 0; }
    if (located != NULL) { *located = 0; }
}
#endif

void camera_set_orientation(uint8_t mode) { (void)mode; }
void camera_note_draw_us(uint32_t us) { (void)us; }
void camera_dump_frame(void) { }
uint8_t camera_orientation(void) { return 0; }
#ifdef LEEK_HOST_TEST
static int8_t test_bias = -4;
void camera_set_exposure_bias(int8_t level)
{
    test_bias = (int8_t)(level < -5 ? -5 : (level > 0 ? 0 : level));
}
int8_t camera_exposure_bias(void) { return test_bias; }
#else
void camera_set_exposure_bias(int8_t level) { (void)level; }
int8_t camera_exposure_bias(void) { return 0; }
#endif
#ifdef LEEK_HOST_TEST
/* The host stand-in's zoom: the real table and cycle, and a switch for a
   level that fails to apply. */
static bool    test_zoom_fails;
void camera__set_zoom_fails_for_test(bool fails) { test_zoom_fails = fails; }
bool camera_next_zoom(void)
{
    if (!test_running) {
        return false;
    }
    if (test_zoom_fails) {
        test_zoom = 0;
        return false;
    }
    test_zoom = camera_tune_zoom_next(test_zoom);
    return true;
}
uint8_t camera_zoom(void) { return test_zoom; }
const char *camera_zoom_label(void) { return camera_tune_zoom(test_zoom)->label; }
#else
bool camera_next_zoom(void) { return false; }
uint8_t camera_zoom(void) { return 0; }
const char *camera_zoom_label(void) { return camera_tune_zoom(0)->label; }
#endif
void camera_set_stream(bool on) { (void)on; }
bool camera_streaming(void) { return false; }

#endif
