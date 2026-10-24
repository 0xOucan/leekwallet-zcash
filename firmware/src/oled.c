/**
 * LeekWallet OLED Driver
 * SSD1306 128x64 I2C display driver
 */

#include "oled.h"
#include "oled-core.h"
#include "board.h"
#if BOARD_PANEL_SH1107
#include "panel128.h"
#endif
#include <string.h>
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "oled";

/* I2C Configuration */
/* See board.h. On a board with no I2C panel these are not merely unused, they
   belong to something else, so nothing here may initialise them. */
#define PIN_SDA             PIN_I2C_SDA
#define PIN_SCL             PIN_I2C_SCL
#define I2C_PORT            I2C_NUM_0
#define I2C_FREQ_HZ         400000

/* Panel I2C address: 0x3C on most modules, 0x3D where the module's address
 * strap is moved. Probed in that order at init (oled_probe_address). */
static uint8_t oled_addr = 0x3C;
#define OLED_ADDR           oled_addr

/* 5x7 font for basic ASCII (32-127), stored as columns */

/* ============================================================================
 * Low-level I2C/SSD1306 functions
 * ============================================================================ */

/*
 * Whether the panel is actually there.
 *
 * `ui_init()` runs whether or not a display was found — the UI task is what
 * drives the auto-lock timer, the hold-to-lock poll and the sign-expiry
 * service, none of which need a screen — so every redraw reaches this file
 * regardless. On a board with no I2C panel that produced two
 * `i2c driver not installed` errors per frame, which is a log nobody can read
 * and, worse, a log in which a real fault would be invisible.
 *
 * Guarded here rather than at the call sites because this is the one place
 * every byte to the panel passes through. Drawing into the framebuffer stays
 * legal and free; only talking to hardware that is not there is refused.
 */
static bool panel_ready = false;

bool oled_present(void) { return panel_ready; }

static esp_err_t ssd1306_send_cmd(uint8_t cmd)
{
    if (!panel_ready) { return ESP_ERR_INVALID_STATE; }
    uint8_t buf[2] = {0x00, cmd};  /* Co=0, D/C#=0 (command) */
    return i2c_master_write_to_device(I2C_PORT, OLED_ADDR, buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t ssd1306_send_data(const uint8_t *data, size_t len)
{
    if (!panel_ready) { return ESP_ERR_INVALID_STATE; }
    uint8_t buf[129];  /* 1 control byte + up to 128 data bytes */
    buf[0] = 0x40;     /* Co=0, D/C#=1 (data) */

    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > 128) ? 128 : (len - offset);
        memcpy(&buf[1], &data[offset], chunk);
        esp_err_t err = i2c_master_write_to_device(I2C_PORT, OLED_ADDR, buf,
                                                   chunk + 1, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk;
    }
    return ESP_OK;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Does anything ACK at `addr`? A bare address write, no payload. */
static bool i2c_present(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

/* Pick the panel's address, or log every device on the bus when it is at
 * neither, so a bring-up shows what is actually wired there. */
static bool oled_probe_address(void)
{
    static const uint8_t candidates[] = { 0x3C, 0x3D };
    for (size_t i = 0; i < sizeof candidates; i++) {
        if (i2c_present(candidates[i])) {
            oled_addr = candidates[i];
            return true;
        }
    }
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_present(a)) {
            ESP_LOGW(TAG, "I2C scan: device at 0x%02X", a);
            found++;
        }
    }
    ESP_LOGE(TAG, "I2C scan on SDA=%d SCL=%d: %d device(s), no panel at 0x3C/0x3D%s",
             PIN_SDA, PIN_SCL, found,
             found ? "" : " - check power, GND, and that SDA/SCL are not swapped");
    return false;
}

esp_err_t oled_i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    esp_err_t err = i2c_param_config(I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C initialized: SDA=%d, SCL=%d, freq=%d Hz",
             PIN_SDA, PIN_SCL, I2C_FREQ_HZ);
    return ESP_OK;
}

#if BOARD_PANEL_SH1107
/* ============================================================================
 * SH1107, 128x128 (the S3CAM screen kit: a 1.5" GME128128-01 IIC module)
 *
 * Same I2C framing as the SSD1306 (0x00 command, 0x40 data) and the same
 * page format, so the frame goes out as it is composed (panel128.c): the UI
 * stretched 2x vertically over all sixteen pages, or a QR redrawn across
 * them at its own square scale. Page
 * addressing: 0xB0+page, then the column, then 128 bytes.
 *
 * The init values are the ones Adafruit's SH110X driver uses for 128x128.
 * Which way up the glass ends up depends on how the module is mounted:
 * BOARD_SH1107_FLIP turns it 180 degrees (segment remap and COM scan).
 * ============================================================================ */

static uint8_t panel_image[PANEL128_BYTES];

esp_err_t oled_init(void)
{
    if (!oled_probe_address()) {
        return ESP_ERR_NOT_FOUND;
    }
    panel_ready = true;
    const uint8_t init_cmds[] = {
        0xAE,                                   /* display off */
        0xD5, 0x51,                             /* clock divide / oscillator */
        0x20,                                   /* page addressing */
        0x81, 0x4F,                             /* contrast */
        0xAD, 0x8A,                             /* DC-DC on */
        BOARD_SH1107_FLIP ? 0xA1 : 0xA0,        /* segment remap */
        BOARD_SH1107_FLIP ? 0xC8 : 0xC0,        /* COM scan direction */
        0xDC, 0x00,                             /* display start line */
        0xD3, 0x00,                             /* display offset */
        0xD9, 0x22,                             /* pre-charge */
        0xDB, 0x35,                             /* VCOMH */
        0xA8, 0x7F,                             /* multiplex: 128 rows */
        0xA4,                                   /* follow RAM */
        0xA6,                                   /* normal, not inverted */
    };
    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        if (ssd1306_send_cmd(init_cmds[i]) != ESP_OK) {
            ESP_LOGE(TAG, "SH1107 init cmd %02X failed", init_cmds[i]);
            panel_ready = false;
            return ESP_FAIL;
        }
    }
    /* Clear the panel's RAM before lighting it: it powers up with noise. */
    oled_clear_panel_now();
    ssd1306_send_cmd(0xAF);
    ESP_LOGI(TAG, "SH1107 initialized: 128x128 @ 0x%02X", OLED_ADDR);
    return ESP_OK;
}

static esp_err_t send_panel(const uint8_t *img)
{
    for (int page = 0; page < PANEL128_PAGES; page++) {
        ssd1306_send_cmd(0xB0 + page);
        ssd1306_send_cmd(0x10);     /* column high nibble: 0 */
        ssd1306_send_cmd(0x00);     /* column low nibble: 0 */
        esp_err_t err = ssd1306_send_data(img + page * PANEL128_SIZE, PANEL128_SIZE);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t oled_flush(void)
{
    bool inverted = false;
    const QRCode *qr = oled_core_last_qr(&inverted);
    panel128_compose(oled_core_framebuffer(), oled_core_panel_picture(), qr, inverted,
                     panel_image);
    return send_panel(panel_image);
}

esp_err_t oled_refresh(void)
{
    return oled_flush();
}

esp_err_t oled_clear_panel_now(void)
{
    memset(panel_image, 0, sizeof panel_image);
    return send_panel(panel_image);
}

esp_err_t oled_set_contrast(uint8_t level)
{
    ssd1306_send_cmd(0x81);
    ssd1306_send_cmd(level);
    return ESP_OK;
}

esp_err_t oled_set_fast_refresh(bool fast)
{
    /* As on the SSD1306: fastest oscillator and a short pre-charge for a
     * camera, the init values otherwise. */
    ssd1306_send_cmd(0xD5);
    ssd1306_send_cmd(fast ? 0xF1 : 0x51);
    ssd1306_send_cmd(0xD9);
    ssd1306_send_cmd(0x22);
    return ESP_OK;
}

esp_err_t oled_draw_raw(const uint8_t *data, size_t len)
{
    return ssd1306_send_data(data, len);
}

uint8_t oled_panel_rows(void) { return PANEL128_SIZE; }

#else /* SSD1306, 128x64 */

uint8_t oled_panel_rows(void) { return OLED_HEIGHT; }

esp_err_t oled_init(void)
{
    if (!oled_probe_address()) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Opened before the sequence below, because that sequence goes through the
       same guarded path. Closed again on any failure, so a panel that did not
       answer cannot leave the driver believing it is there. */
    panel_ready = true;

    /* SSD1306 initialization sequence for 128x64 */
    const uint8_t init_cmds[] = {
        0xAE,       /* Display OFF */
        0xD5, 0x80, /* Set display clock divide ratio/oscillator frequency */
        0xA8, 0x3F, /* Set multiplex ratio (1 to 64) */
        0xD3, 0x00, /* Set display offset */
        0x40,       /* Set start line address */
        0x8D, 0x14, /* Enable charge pump */
        0x20, 0x00, /* Set memory addressing mode: horizontal */
        0xA1,       /* Set segment re-map (column address 127 mapped to SEG0) */
        0xC8,       /* Set COM output scan direction (remapped) */
        0xDA, 0x12, /* Set COM pins hardware configuration */
        0x81, 0xCF, /* Set contrast control */
        0xD9, 0xF1, /* Set pre-charge period */
        0xDB, 0x40, /* Set VCOMH deselect level */
        0xA4,       /* Entire display ON (resume to RAM content) */
        0xA6,       /* Set normal display (not inverted) */
        0xAF,       /* Display ON */
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        esp_err_t err = ssd1306_send_cmd(init_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SSD1306 init cmd %02X failed", init_cmds[i]);
            panel_ready = false;
            return err;
        }
    }

    ESP_LOGI(TAG, "SSD1306 initialized: %dx%d @ 0x%02X", OLED_WIDTH, OLED_HEIGHT, OLED_ADDR);
    return ESP_OK;
}

/* Single frame buffer, pushed to the panel once per render.
 *
 * Text used to be written straight to the display: oled_clear() blanked the
 * panel over I2C and then every character was its own transaction. At 400 kHz
 * that is a visible blank-then-refill on each keypress, which reads as the
 * screen flickering. Nothing wrong with the hardware - the frame was simply
 * being composed in front of the user. */
/* Owned by oled-core.c now; this file only sends it. */
#define framebuffer (oled_core_framebuffer())
static uint8_t cursor_page = 0;
static uint8_t cursor_col = 0;

esp_err_t oled_flush(void)
{
    ssd1306_send_cmd(0x21);
    ssd1306_send_cmd(0x00);
    ssd1306_send_cmd(OLED_WIDTH - 1);
    ssd1306_send_cmd(0x22);
    ssd1306_send_cmd(0x00);
    ssd1306_send_cmd(OLED_PAGES - 1);
    /* The explicit size, never sizeof.
     *
     * `framebuffer` here is a macro that calls oled_core_framebuffer(), so
     * sizeof() measures a POINTER -- four bytes -- and this sent four bytes of
     * a 1024-byte frame. The panel kept whatever was in the rest of its GDDRAM,
     * which is what a screen full of stale pixels looks like. The array lives
     * in oled-core.c now and this file cannot see its extent, so the extent has
     * to be named. */
    return ssd1306_send_data(framebuffer, (size_t)OLED_WIDTH * OLED_PAGES);
}

esp_err_t oled_set_contrast(uint8_t level)
{
    ssd1306_send_cmd(0x81);      /* Set Contrast Control */
    ssd1306_send_cmd(level);
    return ESP_OK;
}

esp_err_t oled_set_fast_refresh(bool fast)
{
    /* The init values are the normal state: 0xD5 0x80, 0xD9 0xF1. Frame rate
       is Fosc / (D * (phase1 + phase2 + 50) * 64): fastest oscillator (0xF0)
       and 2+2 DCLK pre-charge (0x22) instead of 1+15. */
    ssd1306_send_cmd(0xD5);
    ssd1306_send_cmd(fast ? 0xF0 : 0x80);
    ssd1306_send_cmd(0xD9);
    ssd1306_send_cmd(fast ? 0x22 : 0xF1);
    return ESP_OK;
}


esp_err_t oled_clear_panel_now(void)
{
    /* Set column and page address to cover entire display */
    ssd1306_send_cmd(0x21);  /* Set column address */
    ssd1306_send_cmd(0x00);  /* Start column 0 */
    ssd1306_send_cmd(0x7F);  /* End column 127 */
    ssd1306_send_cmd(0x22);  /* Set page address */
    ssd1306_send_cmd(0x00);  /* Start page 0 */
    ssd1306_send_cmd(0x07);  /* End page 7 */

    /* Send zeros to clear all pixels */
    uint8_t zeros[128];
    memset(zeros, 0x00, sizeof(zeros));

    for (int page = 0; page < OLED_PAGES; page++) {
        esp_err_t err = ssd1306_send_data(zeros, sizeof(zeros));
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}







esp_err_t oled_draw_raw(const uint8_t *data, size_t len)
{
    return ssd1306_send_data(data, len);
}



esp_err_t oled_refresh(void)
{
    ssd1306_send_cmd(0x21);  /* Set column address */
    ssd1306_send_cmd(0x00);  /* Start column 0 */
    ssd1306_send_cmd(0x7F);  /* End column 127 */
    ssd1306_send_cmd(0x22);  /* Set page address */
    ssd1306_send_cmd(0x00);  /* Start page 0 */
    ssd1306_send_cmd(0x07);  /* End page 7 */

    /* Explicit, for the reason given in oled_flush(). */
    return ssd1306_send_data(framebuffer, (size_t)OLED_WIDTH * OLED_PAGES);
}

#endif /* BOARD_PANEL_SH1107 */
