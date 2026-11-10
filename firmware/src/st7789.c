/**
 * A small ST7789 driver. See st7789.h.
 *
 * The command set is the panel's datasheet (Sitronix ST7789V, section 9); the
 * transaction pattern - D/C driven from the SPI pre-transfer callback, two
 * band buffers in flight - is the one firefly-display (MIT, Firefly) uses on
 * the Pixie, which is where this driver came from (THIRD-PARTY-LICENSES.md).
 */

#include "st7789.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

static const char *TAG = "st7789";

#define CMD_SWRESET   0x01
#define CMD_SLPOUT    0x11
#define CMD_NORON     0x13
#define CMD_INVOFF    0x20
#define CMD_INVON     0x21
#define CMD_DISPON    0x29
#define CMD_CASET     0x2A
#define CMD_RASET     0x2B
#define CMD_RAMWR     0x2C
#define CMD_MADCTL    0x36
#define CMD_COLMOD    0x3A

/* 8 rows: a factor of 240, and 3.75 KB a buffer at 240 wide. The two
   buffers must be internal DMA memory, which the Zcash worker's stack, BLE
   and the UI task's stack also need; at 24 rows (23 KB for both) the UI
   task could not be created on the board with BLE on. */
#define BAND_ROWS     8
/* Six transactions a band: CASET, its data, RASET, its data, RAMWR, pixels. */
#define TX_PER_BAND   6

static struct {
    bool                ready;
    St7789Config        cfg;
    spi_device_handle_t dev;
    uint16_t           *buf[2];
    spi_transaction_t   tx[2][TX_PER_BAND];
    bool                inflight[2];
    int                 next;   /* the buffer st7789_band() hands out */
} st;

/* Data or command, carried in each transaction's user field and applied to
   D/C just before it goes out. Runs in the SPI ISR, hence IRAM and the LL
   call rather than gpio_set_level(). */
static void IRAM_ATTR pre_transfer(spi_transaction_t *t)
{
    gpio_ll_set_level(&GPIO, st.cfg.dc, (uint32_t)(uintptr_t)t->user);
}

static esp_err_t send(uint8_t dc, const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    /* Copied, because an init table in flash is not DMA-readable. */
    static uint8_t dma_copy[32];
    if (len > sizeof dma_copy) return ESP_ERR_INVALID_SIZE;
    memcpy(dma_copy, data, len);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = dma_copy,
        .user = (void *)(uintptr_t)dc,
    };
    return spi_device_polling_transmit(st.dev, &t);
}

static esp_err_t command(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = send(0, &cmd, 1);
    return err == ESP_OK ? send(1, data, len) : err;
}

/* Collect the oldest band's six results. Bands complete in the order they
   were queued, so the oldest is the one a caller is waiting to reuse. */
static esp_err_t retire_oldest(void)
{
    spi_transaction_t *done = NULL;
    for (int i = 0; i < TX_PER_BAND; i++) {
        esp_err_t err = spi_device_get_trans_result(st.dev, &done, portMAX_DELAY);
        if (err != ESP_OK) return err;
    }
    for (int b = 0; b < 2; b++) {
        if (done == &st.tx[b][TX_PER_BAND - 1]) st.inflight[b] = false;
    }
    return ESP_OK;
}

uint16_t st7789_band_rows(void) { return BAND_ROWS; }

bool st7789_ready(void) { return st.ready; }

uint16_t *st7789_band(void)
{
    if (!st.ready) return NULL;
    if (st.inflight[st.next]) {
        /* Both buffers queued: the other was queued after this one, so the
           oldest is this one. */
        if (retire_oldest() != ESP_OK) return NULL;
    }
    return st.buf[st.next];
}

esp_err_t st7789_send_band(uint16_t y, uint16_t rows)
{
    if (!st.ready || rows == 0 || rows > BAND_ROWS || y + rows > st.cfg.height) {
        return ESP_ERR_INVALID_ARG;
    }
    const int b = st.next;
    const uint16_t x0 = st.cfg.x_offset, x1 = x0 + st.cfg.width - 1;
    const uint16_t y0 = st.cfg.y_offset + y, y1 = y0 + rows - 1;
    spi_transaction_t *t = st.tx[b];
    memset(t, 0, sizeof st.tx[b]);
    for (int i = 0; i < TX_PER_BAND - 1; i++) {
        t[i].flags = SPI_TRANS_USE_TXDATA;
        t[i].user = (void *)(uintptr_t)(i & 1);   /* command, data, command... */
    }
    t[0].length = 8;  t[0].tx_data[0] = CMD_CASET;
    t[1].length = 32;
    t[1].tx_data[0] = x0 >> 8; t[1].tx_data[1] = x0 & 0xFF;
    t[1].tx_data[2] = x1 >> 8; t[1].tx_data[3] = x1 & 0xFF;
    t[2].length = 8;  t[2].tx_data[0] = CMD_RASET;
    t[3].length = 32;
    t[3].tx_data[0] = y0 >> 8; t[3].tx_data[1] = y0 & 0xFF;
    t[3].tx_data[2] = y1 >> 8; t[3].tx_data[3] = y1 & 0xFF;
    t[4].length = 8;  t[4].tx_data[0] = CMD_RAMWR;
    t[4].user = (void *)0;
    t[5].length = (size_t)st.cfg.width * rows * 16;
    t[5].tx_buffer = st.buf[b];
    t[5].user = (void *)1;

    for (int i = 0; i < TX_PER_BAND; i++) {
        esp_err_t err = spi_device_queue_trans(st.dev, &t[i], portMAX_DELAY);
        if (err != ESP_OK) return err;
    }
    st.inflight[b] = true;
    st.next = b ^ 1;
    return ESP_OK;
}

esp_err_t st7789_wait(void)
{
    while (st.inflight[0] || st.inflight[1]) {
        esp_err_t err = retire_oldest();
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }

esp_err_t st7789_init(const St7789Config *cfg)
{
    if (st.ready) return ESP_OK;
    st.cfg = *cfg;

    const size_t band_bytes = (size_t)cfg->width * BAND_ROWS * sizeof(uint16_t);
    for (int b = 0; b < 2; b++) {
        st.buf[b] = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (st.buf[b] == NULL) {
            ESP_LOGE(TAG, "no DMA memory for a %u-byte band", (unsigned)band_bytes);
            return ESP_ERR_NO_MEM;
        }
    }

    /* D/C (and reset) are plain outputs. Reset first: a pin that is part of
       a native SPI function keeps it otherwise, even with no signal routed. */
    gpio_reset_pin(cfg->dc);
    gpio_set_direction(cfg->dc, GPIO_MODE_OUTPUT);
    if (cfg->reset != GPIO_NUM_NC) {
        gpio_reset_pin(cfg->reset);
        gpio_set_direction(cfg->reset, GPIO_MODE_OUTPUT);
    }

    const spi_bus_config_t bus = {
        .mosi_io_num = cfg->mosi,
        .miso_io_num = -1,
        .sclk_io_num = cfg->sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)band_bytes,
    };
    esp_err_t err = spi_bus_initialize(cfg->spi_host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus: %s", esp_err_to_name(err));
        return err;
    }
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = (int)cfg->clock_hz,
        .mode = cfg->spi_mode,
        .spics_io_num = cfg->cs,
        .queue_size = 2 * TX_PER_BAND,
        .pre_cb = pre_transfer,
    };
    err = spi_bus_add_device(cfg->spi_host, &dev, &st.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Reset. With a reset line, pulse it. Without one - a module whose RES is
     * tied high - SWRESET puts every register back to its default just the
     * same; what it cannot do is recover a controller wedged mid-transfer,
     * which a power cycle does instead. 120 ms is the datasheet's wait before
     * SLPOUT after either.
     */
    if (cfg->reset != GPIO_NUM_NC) {
        gpio_set_level(cfg->reset, 0);
        delay_ms(10);
        gpio_set_level(cfg->reset, 1);
    } else {
        command(CMD_SWRESET, NULL, 0);
    }
    delay_ms(150);

    err = command(CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) {
        /* A write-only bus cannot tell a missing panel from a present one;
           this fails only when the SPI peripheral itself does. */
        ESP_LOGE(TAG, "SLPOUT: %s", esp_err_to_name(err));
        return err;
    }
    delay_ms(120);

    for (const uint8_t *p = cfg->extra_init; p && p[0] != ST7789_INIT_END; p += 2 + p[1]) {
        command(p[0], p + 2, p[1]);
    }
    const uint8_t colmod = 0x55;   /* 16 bits a pixel, RGB565 */
    command(CMD_COLMOD, &colmod, 1);
    command(CMD_MADCTL, &cfg->madctl, 1);
    command(cfg->invert ? CMD_INVON : CMD_INVOFF, NULL, 0);
    command(CMD_NORON, NULL, 0);

    /* Black before the panel is switched on: its RAM powers up as noise. */
    st.ready = true;
    for (uint16_t y = 0; y < cfg->height; y += BAND_ROWS) {
        uint16_t *buf = st7789_band();
        if (buf == NULL) break;
        memset(buf, 0, band_bytes);
        const uint16_t rows = (cfg->height - y < BAND_ROWS) ? cfg->height - y : BAND_ROWS;
        st7789_send_band(y, rows);
    }
    st7789_wait();

    command(CMD_DISPON, NULL, 0);
    delay_ms(20);
    ESP_LOGI(TAG, "%ux%u at (%u,%u), MADCTL %02X, %s, %u MHz, SPI mode %u",
             cfg->width, cfg->height, cfg->x_offset, cfg->y_offset, cfg->madctl,
             cfg->invert ? "inverted" : "not inverted",
             (unsigned)(cfg->clock_hz / 1000000), cfg->spi_mode);
    return ESP_OK;
}
