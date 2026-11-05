/**
 * A small ST7789 driver: SPI bring-up, an init sequence, and bands of RGB565.
 *
 * ---------------------------------------------------------------------------
 * Why not the vendored one
 *
 * The Pixie used to be driven by firefly-display, which is a good driver for
 * the Pixie and only the Pixie: its pins are the C3's IOMUX pins, its reset
 * pin is mandatory, its geometry is 240x240 with no offset and two rotations,
 * and its init sequence carries that panel's gamma. The S3 CAM's colour kit
 * is a different 240x240 module on GPIO-matrix pins, with RES tied high and a
 * CS that is grounded, and the next panel will differ again. Patching a
 * vendored driver per board is how two copies of it happen.
 *
 * So everything a panel module varies in is a field of St7789Config, and the
 * driver is otherwise the same few commands every ST7789 needs. The Pixie's
 * gamma and power settings survive as its profile's `extra_init`, byte for
 * byte (oled-st7789.c).
 *
 * ---------------------------------------------------------------------------
 * Bands, not frames
 *
 * A 240x240 RGB565 frame is 115 KB. The C3 has 400 KB in all and no PSRAM,
 * and DMA cannot read PSRAM on the S3 without a bounce buffer anyway. So the
 * caller renders a band of rows at a time into one of two DMA buffers owned
 * here, and while one band is on the wire the next is being rendered:
 *
 *     for (y = 0; y < height; y += st7789_band_rows()) {
 *         uint16_t *buf = st7789_band();     // waits for a free buffer
 *         render(y, buf);
 *         st7789_send_band(y, rows);         // queued, returns at once
 *     }
 *     st7789_wait();                         // the frame is on the glass
 */
#ifndef LEEK_ST7789_H
#define LEEK_ST7789_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * MADCTL for the four orientations of a 240x240 panel cut from the
 * controller's 240x320 RAM, and the RAM offset each one needs. Rotations 2
 * and 3 count from the far end of the RAM, where 80 rows are not glass; that
 * is the "picture shifted by a third" every first bring-up of these modules
 * meets.
 */
#define ST7789_MADCTL_MY    0x80
#define ST7789_MADCTL_MX    0x40
#define ST7789_MADCTL_MV    0x20
#define ST7789_MADCTL_BGR   0x08

typedef struct {
    int        spi_host;        /* SPI2_HOST on every target here */
    gpio_num_t sclk, mosi;
    gpio_num_t cs;              /* GPIO_NUM_NC when tied to ground */
    gpio_num_t dc;
    gpio_num_t reset;           /* GPIO_NUM_NC: software reset instead */
    uint32_t   clock_hz;
    uint8_t    spi_mode;        /* 3 for a module whose CS is grounded */

    uint16_t   width, height;   /* visible pixels, after rotation */
    uint16_t   x_offset, y_offset;  /* where the visible window starts in RAM */
    uint8_t    madctl;          /* orientation, mirroring, RGB/BGR order */
    bool       invert;          /* INVON: most IPS modules need it, or black is white */
    bool       little_endian;   /* extra_init set RAMCTRL to little-endian pixels */

    /* Panel-specific registers, sent after SLPOUT: {cmd, n, n bytes}...,
       ending with ST7789_INIT_END. NULL for none. */
    const uint8_t *extra_init;
} St7789Config;

#define ST7789_INIT_END 0xFF

/** Bring the bus and panel up, fill it black and switch it on. */
esp_err_t st7789_init(const St7789Config *cfg);

/** True after a successful init. */
bool st7789_ready(void);

/** Rows in one band. */
uint16_t st7789_band_rows(void);

/** A band buffer free to render into, width * st7789_band_rows() pixels. */
uint16_t *st7789_band(void);

/** Queue the buffer last returned by st7789_band() for rows [y, y + rows). */
esp_err_t st7789_send_band(uint16_t y, uint16_t rows);

/** Wait until every queued band has been sent. */
esp_err_t st7789_wait(void);

#endif /* LEEK_ST7789_H */
