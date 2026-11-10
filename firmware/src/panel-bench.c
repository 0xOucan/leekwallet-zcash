/* See panel-bench.h. */

#include "panel-bench.h"

#if LEEK_PANEL_BENCH

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#include "board.h"

static const char *TAG = "panel-bench";

#define PIN_SCL   PIN_DISPLAY_SCLK   /* module SCL, header "1"  */
#define PIN_SDA   PIN_DISPLAY_MOSI   /* module SDA, header "41" */
#define PIN_DC    PIN_DISPLAY_DC     /* module DC,  header "2"  */
#define PIN_CS    GPIO_NUM_48        /* optional: module CS on header "48" */

#define W 240
#define H 240

/* Half a clock period in microseconds: about 250 kHz, slow enough for any
   jumper wire. Pixel fills use HALF_FAST to keep a screen under ~1 s. */
#define HALF_SLOW 2
#define HALF_FAST 0

static int cpol, cpha;   /* the SPI mode under test */

static inline void pin(gpio_num_t p, int v) { gpio_set_level(p, v); }

static void bits(uint8_t b, int half)
{
    for (int i = 7; i >= 0; i--) {
        const int v = (b >> i) & 1;
        if (cpha == 0) {
            /* Data valid before the leading edge. */
            pin(PIN_SDA, v);
            if (half) esp_rom_delay_us(half);
            pin(PIN_SCL, !cpol);
            if (half) esp_rom_delay_us(half);
            pin(PIN_SCL, cpol);
        } else {
            /* Data changes on the leading edge, sampled on the trailing one. */
            pin(PIN_SCL, !cpol);
            pin(PIN_SDA, v);
            if (half) esp_rom_delay_us(half);
            pin(PIN_SCL, cpol);
            if (half) esp_rom_delay_us(half);
        }
    }
}

static void cs(int v)
{
    pin(PIN_CS, v);
    esp_rom_delay_us(1);
}

static void cmd(uint8_t c, const uint8_t *data, int n)
{
    cs(0);
    pin(PIN_DC, 0);
    bits(c, HALF_SLOW);
    pin(PIN_DC, 1);
    for (int i = 0; i < n; i++) bits(data[i], HALF_SLOW);
    cs(1);
}

static void fill(uint16_t rgb565)
{
    const uint8_t caset[] = { 0, 0, 0, W - 1 };
    const uint8_t raset[] = { 0, 0, 0, H - 1 };
    cmd(0x2A, caset, 4);
    cmd(0x2B, raset, 4);
    cs(0);
    pin(PIN_DC, 0);
    bits(0x2C, HALF_SLOW);   /* RAMWR */
    pin(PIN_DC, 1);
    const uint8_t hi = rgb565 >> 8, lo = rgb565 & 0xFF;
    for (int i = 0; i < W * H; i++) {
        bits(hi, HALF_FAST);
        bits(lo, HALF_FAST);
        if ((i & 0x3FFF) == 0) vTaskDelay(1);   /* keep the watchdog fed */
    }
    cs(1);
}

static void panel_init(void)
{
    cmd(0x01, NULL, 0);                 /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11, NULL, 0);                 /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(150));
    const uint8_t colmod = 0x55, madctl = 0x00;
    cmd(0x3A, &colmod, 1);              /* 16 bits a pixel */
    cmd(0x36, &madctl, 1);
    cmd(0x21, NULL, 0);                 /* INVON: IPS panels */
    cmd(0x13, NULL, 0);                 /* NORON */
    cmd(0x29, NULL, 0);                 /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void bench_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));    /* let the boot log drain */

    const gpio_num_t outs[] = { PIN_SCL, PIN_SDA, PIN_DC, PIN_CS };
    for (unsigned i = 0; i < sizeof outs / sizeof outs[0]; i++) {
        gpio_reset_pin(outs[i]);
        gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
        gpio_set_level(outs[i], 1);
    }
    ESP_LOGI(TAG, "======== colour panel bench ========");
    ESP_LOGI(TAG, "no wallet, no vault: bit-banged SCL=GPIO%d SDA=GPIO%d DC=GPIO%d, CS also on GPIO%d",
             PIN_SCL, PIN_SDA, PIN_DC, PIN_CS);

    static const int modes[] = { 3, 0, 2, 1 };
    static const struct { uint16_t c; const char *name; } colours[] = {
        { 0xF800, "RED" }, { 0x07E0, "GREEN" }, { 0x001F, "BLUE" },
    };
    for (unsigned round = 1;; round++) {
        for (unsigned m = 0; m < 4; m++) {
            cpol = modes[m] >> 1;
            cpha = modes[m] & 1;
            /* Idle the clock at this mode's level for a while, CS high. */
            cs(1);
            pin(PIN_SCL, cpol);
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGI(TAG, "round %u, SPI mode %d: init", round, modes[m]);
            panel_init();
            for (unsigned c = 0; c < 3; c++) {
                ESP_LOGI(TAG, "round %u, SPI mode %d: %s", round, modes[m], colours[c].name);
                fill(colours[c].c);
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
        }
    }
}

void panel_bench_start(void)
{
    xTaskCreate(bench_task, "panel_bench", 4096, NULL, 5, NULL);
}

#endif /* LEEK_PANEL_BENCH */
