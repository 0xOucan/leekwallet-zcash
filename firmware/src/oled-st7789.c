/**
 * The ST7789 transport: a 240x240 colour TFT showing the UI.
 *
 * ---------------------------------------------------------------------------
 * What this is, and what it is not
 *
 * The panel-specific half of the display and nothing else, like oled.c for
 * the OLEDs: bring the panel up, flush, contrast, clear the glass. Every
 * glyph, the 8x21 grid and the QR encoder live in oled-core.c; what a frame
 * looks like in colour is panel-color.c; the SPI and the controller are
 * st7789.c. This file only joins them, and holds the one thing that differs
 * between the panels it drives: their profile.
 *
 * ---------------------------------------------------------------------------
 * Profiles
 *
 * Two boards carry an ST7789 and the panels are not the same part:
 *
 *   - the Firefly Pixie's, on the C3's IOMUX pins with a reset line, whose
 *     orientation and gamma come from firefly-display byte for byte, because
 *     that is what has been looked at on real glass;
 *   - the common 1.3"/1.54" 240x240 module (S3 CAM colour kit), on GPIO-matrix
 *     pins with RES tied high and CS grounded, set up the way nearly every
 *     driver for those modules sets them up: inversion on, no extra registers.
 *
 * The pins come from board.h. Geometry, rotation, inversion, clock and SPI
 * mode are ST7789_* macros with the module's usual values as defaults, so a
 * panel that comes up mirrored, shifted or in negative is fixed from
 * platformio.ini without touching code; docs/WIRING-S3CAM-COLOR.md says which
 * symptom is which macro.
 */

#include "board.h"

#if BOARD_HAS_SPI_PANEL

#include "oled.h"
#include "oled-core.h"
#include "panel-color.h"
#include "st7789.h"

#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "oled-st7789";

#ifndef ST7789_WIDTH
#  define ST7789_WIDTH      240
#endif
#ifndef ST7789_HEIGHT
#  define ST7789_HEIGHT     240
#endif

#if ST7789_PROFILE_PIXIE

/* firefly-display's init sequence after SLPOUT, minus what st7789.c sends
   itself (COLMOD, MADCTL, INVON, NORON, DISPON). RAMCTRL makes pixels
   little-endian, so the palette goes out unswapped. */
static const uint8_t PIXIE_INIT[] = {
    0xB0, 2, 0x00, 0xE8,                         /* RAMCTRL: little-endian */
    0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,       /* PORCTRL */
    0xB7, 1, 0x45,                               /* GCTRL */
    0xBB, 1, 0x2B,                               /* VCOMS */
    0xC0, 1, 0x2C,                               /* LCMCTRL: XBGR | XMX | XMH */
    0xC2, 2, 0x01, 0xFF,                         /* VDVVRHEN */
    0xC3, 1, 0x11,                               /* VRHS */
    0xC4, 1, 0x20,                               /* VDVS */
    0xC6, 1, 0x0F,                               /* FRCTRL2: 60 Hz */
    0xD0, 2, 0xA4, 0xA1,                         /* PWCTRL1 */
    0xE0, 14, 0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09,
              0x15, 0x12, 0x16, 0x19,            /* PVGAMCTRL */
    0xE1, 14, 0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E,
              0x1C, 0x18, 0x16, 0x19,            /* NVGAMCTRL */
    ST7789_INIT_END,
};

/* "Ribbon right", firefly-display's orientation for the Pixie. */
#  define PROFILE_MADCTL        (ST7789_MADCTL_MV | ST7789_MADCTL_MX)
#  define PROFILE_X_OFFSET      0
#  define PROFILE_Y_OFFSET      0
#  define PROFILE_INIT          PIXIE_INIT
#  define PROFILE_LITTLE_ENDIAN true
#  define PROFILE_NAME          "Pixie"

#else /* the common 240x240 module */

/*
 * Rotation 0-3, a quarter turn each, and the RAM offset each needs: the
 * controller has 320 rows of RAM and these modules show 240 of them, so the
 * two rotations that count from the far end start 80 rows in.
 */
#  ifndef ST7789_ROTATION
#    define ST7789_ROTATION     0
#  endif
#  if ST7789_ROTATION == 0
#    define PROFILE_MADCTL      0
#    define ROT_X 0
#    define ROT_Y 0
#  elif ST7789_ROTATION == 1
#    define PROFILE_MADCTL      (ST7789_MADCTL_MV | ST7789_MADCTL_MX)
#    define ROT_X 0
#    define ROT_Y 0
#  elif ST7789_ROTATION == 2
#    define PROFILE_MADCTL      (ST7789_MADCTL_MX | ST7789_MADCTL_MY)
#    define ROT_X 0
#    define ROT_Y 80
#  elif ST7789_ROTATION == 3
#    define PROFILE_MADCTL      (ST7789_MADCTL_MV | ST7789_MADCTL_MY)
#    define ROT_X 80
#    define ROT_Y 0
#  else
#    error "ST7789_ROTATION is 0, 1, 2 or 3"
#  endif
#  ifndef ST7789_X_OFFSET
#    define ST7789_X_OFFSET     ROT_X
#  endif
#  ifndef ST7789_Y_OFFSET
#    define ST7789_Y_OFFSET     ROT_Y
#  endif
#  define PROFILE_X_OFFSET      ST7789_X_OFFSET
#  define PROFILE_Y_OFFSET      ST7789_Y_OFFSET
#  define PROFILE_INIT          NULL
#  define PROFILE_LITTLE_ENDIAN false
#  define PROFILE_NAME          "240x240 module"

#endif

/* Options every profile takes from the build. */
#ifndef ST7789_INVERT
#  define ST7789_INVERT         1    /* IPS: without it black shows white */
#endif
#ifndef ST7789_BGR
#  define ST7789_BGR            0    /* 1 if red and blue are swapped */
#endif
#ifndef ST7789_SPI_HZ
/* 40 MHz: a frame in ~23 ms, and within what the GPIO matrix carries
   cleanly over a few centimetres of jumper wire. */
#  define ST7789_SPI_HZ         40000000
#endif
#ifndef ST7789_SPI_MODE
   /* With a CS pin, mode 0, the controller's own; with CS grounded, mode 3. */
#  define ST7789_SPI_MODE       (PIN_DISPLAY_CS == GPIO_NUM_NC ? 3 : 0)
#endif

/* The palette as the bus wants it: swapped to big-endian unless the profile
   told the controller to take little-endian pixels. */
static PanelPalette wire_palette;

static uint16_t to_wire(uint16_t c)
{
    return PROFILE_LITTLE_ENDIAN ? c : (uint16_t)((c >> 8) | (c << 8));
}

static void build_wire_palette(void)
{
    const PanelPalette *p = panel_color_palette();
    wire_palette.bg = to_wire(p->bg);
    for (int i = 0; i < OLED_ROLE_COUNT; i++) wire_palette.fg[i] = to_wire(p->fg[i]);
    wire_palette.select_bg = to_wire(p->select_bg);
    wire_palette.select_fg = to_wire(p->select_fg);
    wire_palette.rule = to_wire(p->rule);
    wire_palette.qr_dark = to_wire(p->qr_dark);
    wire_palette.qr_light = to_wire(p->qr_light);
    wire_palette.picture = to_wire(p->picture);
    wire_palette.picture_key = to_wire(p->picture_key);
    wire_palette.picture_spark = to_wire(p->picture_spark);
    wire_palette.picture_halo = to_wire(p->picture_halo);
    for (int i = 0; i < 64; i++) wire_palette.gray[i] = to_wire(p->gray[i]);
}

/* ------------------------------------------------------------ backlight */

/*
 * LEDC PWM on BLK, as the user's ST7735S driver does it: low-speed mode,
 * 13-bit duty at 5 kHz (far above anything an eye or a phone camera's
 * shutter beats against), starting at 100%. Its own timer and channel:
 * esp32-camera drives the sensor's XCLK from LEDC timer 0, channel 0
 * (camera.c), and sharing either would retune the camera's clock.
 */
#define BLK_TIMER       LEDC_TIMER_1
#define BLK_CHANNEL     LEDC_CHANNEL_1
#define BLK_RES         LEDC_TIMER_13_BIT
#define BLK_DUTY_MAX    8191
#define BLK_HZ          5000

static bool backlight_ready;

static void backlight_init(void)
{
    if (PIN_DISPLAY_BLK == GPIO_NUM_NC) {
        return;
    }
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BLK_RES,
        .timer_num = BLK_TIMER,
        .freq_hz = BLK_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = PIN_DISPLAY_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BLK_CHANNEL,
        .timer_sel = BLK_TIMER,
        .duty = BLK_DUTY_MAX,
        .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) {
        err = ledc_channel_config(&channel);
    }
    if (err != ESP_OK) {
        /* Not fatal: the panel works at whatever BLK floats to, and a
           screen at full brightness is no reason to stop a wallet. */
        ESP_LOGW(TAG, "backlight PWM on GPIO%d: %s", PIN_DISPLAY_BLK, esp_err_to_name(err));
        return;
    }
    backlight_ready = true;
}

esp_err_t oled_set_backlight(uint8_t pct)
{
    if (pct < OLED_BACKLIGHT_MIN) pct = OLED_BACKLIGHT_MIN;
    if (pct > OLED_BACKLIGHT_MAX) pct = OLED_BACKLIGHT_MAX;
    if (!backlight_ready) {
        return ESP_OK;   /* BLK on 3V3: nothing to dim, and not an error */
    }
    const uint32_t duty = (uint32_t)pct * BLK_DUTY_MAX / 100;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, BLK_CHANNEL, duty);
    return err == ESP_OK ? ledc_update_duty(LEDC_LOW_SPEED_MODE, BLK_CHANNEL) : err;
}

/* -------------------------------------------------------------------- API */

esp_err_t oled_i2c_init(void)
{
    /* No I2C panel. Succeeding without touching anything keeps main() one
       sequence for every board; on the CAM colour kit the I2C pins are left
       to the secure element. */
    return ESP_OK;
}

esp_err_t oled_init(void)
{
    const St7789Config cfg = {
        .spi_host = SPI2_HOST,
        .sclk = PIN_DISPLAY_SCLK,
        .mosi = PIN_DISPLAY_MOSI,
        .cs = PIN_DISPLAY_CS,
        .dc = PIN_DISPLAY_DC,
        .reset = PIN_DISPLAY_RESET,
        .clock_hz = ST7789_SPI_HZ,
        .spi_mode = ST7789_SPI_MODE,
        .width = ST7789_WIDTH,
        .height = ST7789_HEIGHT,
        .x_offset = PROFILE_X_OFFSET,
        .y_offset = PROFILE_Y_OFFSET,
        .madctl = (uint8_t)(PROFILE_MADCTL | (ST7789_BGR ? ST7789_MADCTL_BGR : 0)),
        .invert = ST7789_INVERT,
        .little_endian = PROFILE_LITTLE_ENDIAN,
        .extra_init = PROFILE_INIT,
    };
    build_wire_palette();
    backlight_init();
    esp_err_t err = st7789_init(&cfg);
    if (err != ESP_OK) {
        /* Said out loud rather than swallowed: main() checks this, and a
           wallet with no screen must not reach the PIN prompt believing it
           has one. */
        ESP_LOGE(TAG, "ST7789 (%s) init failed: %s", PROFILE_NAME, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ST7789 %s: SCLK=%d MOSI=%d DC=%d CS=%d RES=%d", PROFILE_NAME,
             PIN_DISPLAY_SCLK, PIN_DISPLAY_MOSI, PIN_DISPLAY_DC,
             PIN_DISPLAY_CS, PIN_DISPLAY_RESET);
    return ESP_OK;
}

esp_err_t oled_flush(void)
{
    if (!st7789_ready()) { return ESP_ERR_INVALID_STATE; }
    PanelFrame f = {
        .frame = oled_core_framebuffer(),
        .glyphs = oled_core_glyphs(),
        .roles = oled_core_roles(),
        .picture = oled_core_panel_picture(),
        .tint = oled_core_panel_tint(),
    };
    f.qr = oled_core_last_qr(&f.qr_inverted);
    const OledPanelGray g = oled_core_panel_gray();
    f.gray = g.px;
    f.gray_x = g.x; f.gray_y = g.y; f.gray_w = g.w; f.gray_h = g.h;

    /* Render one band while the previous one is on the wire; flush returns
       once the whole frame is on the glass, which is what it means on the
       OLEDs too. */
    const uint16_t band = st7789_band_rows();
    for (uint16_t y = 0; y < ST7789_HEIGHT; y += band) {
        const uint16_t rows = (ST7789_HEIGHT - y < band) ? ST7789_HEIGHT - y : band;
        uint16_t *buf = st7789_band();
        if (buf == NULL) { return ESP_FAIL; }
        panel_color_render(&f, &wire_palette, ST7789_WIDTH, ST7789_HEIGHT, y, rows, buf);
        esp_err_t err = st7789_send_band(y, rows);
        if (err != ESP_OK) { return err; }
    }
    return st7789_wait();
}

esp_err_t oled_refresh(void) { return oled_flush(); }

esp_err_t oled_set_fast_refresh(bool fast)
{
    /* An LCD holds its image; there is no row scan to beat against. */
    (void)fast;
    return ESP_OK;
}

esp_err_t oled_set_contrast(uint8_t level)
{
    /* The ST7789 has no contrast register. The backlight is
       oled_set_backlight(), a setting of its own (Settings > Screen): the
       OLED levels this takes, Dim for every QR among them, would make a TFT
       unreadable. The setting is still accepted
       and kept by ui.c, so a user's choice survives a move between boards. */
    (void)level;
    return ESP_OK;
}

esp_err_t oled_clear_panel_now(void)
{
    oled_clear();
    return oled_flush();
}

esp_err_t oled_draw_raw(const uint8_t *data, size_t len)
{
    /* Raw SSD1306 bytes mean nothing to an ST7789, and guessing a
       translation would be worse than refusing. */
    (void)data; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

bool oled_present(void) { return st7789_ready(); }

/* Tall enough for the full-panel splash (ui.c asks for >= 128). */
uint8_t oled_panel_rows(void) { return ST7789_HEIGHT > 255 ? 255 : ST7789_HEIGHT; }

#endif /* BOARD_HAS_SPI_PANEL */
