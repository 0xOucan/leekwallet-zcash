/* See se-bench.h. */

#include "se-bench.h"

#if LEEK_SE_BENCH

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "leek-se.h"

static const char *TAG = "se-bench";

/*
 * The bus as the wallet sets it up: I2C0, the board's own pins, 400 kHz,
 * internal pull-ups. Done here rather than through oled_i2c_init() so this
 * file does not depend on the panel driver, which is being reworked; the
 * settings are copied from it on purpose, so that what works here is what
 * will work next to the panel.
 */
#define SE_I2C_PORT     I2C_NUM_0
#define SE_I2C_HZ       400000
#define REPEAT_MS       20000

static bool bus_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SE_I2C_HZ,
    };
    esp_err_t err = i2c_param_config(SE_I2C_PORT, &conf);
    if (err == ESP_OK)
        err = i2c_driver_install(SE_I2C_PORT, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "I2C0 up: SDA=GPIO%d SCL=GPIO%d %d Hz",
             PIN_I2C_SDA, PIN_I2C_SCL, SE_I2C_HZ);
    return true;
}

static bool ack_at(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)(addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(SE_I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

/* Everything on the bus, named where we know it. */
static void scan(void)
{
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (!ack_at(a))
            continue;
        const char *what = a == OPTIGA_I2C_ADDR ? "OPTIGA Trust M (default address)"
                         : (a == 0x3C || a == 0x3D) ? "OLED panel"
                         : "unknown";
        ESP_LOGI(TAG, "  0x%02X  %s", a, what);
        found++;
    }
    ESP_LOGI(TAG, "scan: %d device(s)", found);
}

static void hex(const char *label, const uint8_t *p, size_t n)
{
    char line[3 * 32 + 1];
    for (size_t off = 0; off < n; off += 32) {
        size_t k = n - off > 32 ? 32 : n - off, w = 0;
        for (size_t i = 0; i < k; i++)
            w += (size_t)snprintf(line + w, sizeof line - w, "%02X", p[off + i]);
        ESP_LOGI(TAG, "%s%s", off ? "    " : label, line);
    }
}

/*
 * Metadata of a few objects, as raw TLV. Reading metadata is read-only like
 * reading data, and it is the most useful thing a researcher can see here:
 * who may read, change or execute each object as this chip left the factory.
 * The list deliberately includes the device private key (0xE0F0) - its
 * METADATA, which says "read: never"; the key itself is not readable at all.
 */
static void dump_metadata(void)
{
    static const struct { uint16_t oid; const char *name; } objs[] = {
        { 0xE0C2, "coprocessor UID" },
        { 0xE0E0, "device certificate" },
        { 0xE0F0, "device private key" },
        { 0xE120, "monotonic counter 1" },
        { 0xE140, "platform binding secret" },
        { 0xF1D0, "data object 1" },
    };
    uint8_t m[64];
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) {
        size_t n = 0;
        char label[48];
        snprintf(label, sizeof label, "  meta %04X %-24s ", objs[i].oid, objs[i].name);
        if (leek_se_read(objs[i].oid, true, m, sizeof m, &n) == ESP_OK)
            hex(label, m, n);
        else
            ESP_LOGW(TAG, "%s(%s)", label, leek_se_last_error());
    }
}

static void report(void)
{
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "======== OPTIGA Trust M bring-up ========");
    ESP_LOGI(TAG, "read-only: no keys, data objects, metadata or lifecycle state are written");
    scan();

    if (!leek_se_probe()) {
        ESP_LOGE(TAG, "nothing at 0x%02X. Check: VCC to 3V3, GND, SDA->GPIO%d, "
                 "SCL->GPIO%d, RST tied to 3V3 (a floating RST holds the chip in reset)",
                 OPTIGA_I2C_ADDR, PIN_I2C_SDA, PIN_I2C_SCL);
        return;
    }
    ESP_LOGI(TAG, "probe: ACK at 0x%02X", OPTIGA_I2C_ADDR);

    if (leek_se_open() != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", leek_se_last_error());
        return;
    }

    static leek_se_info_t info;
    if (leek_se_info(&info) != ESP_OK) {
        ESP_LOGE(TAG, "info failed: %s", leek_se_last_error());
        return;
    }
    ESP_LOGI(TAG, "link: DATA_REG_LEN %u", info.data_reg_len);
    hex("UID ", info.uid, sizeof info.uid);
    ESP_LOGI(TAG, "  maker      0x%02X %s", info.id.cim,
             info.infineon ? "(Infineon)" : "(NOT the Infineon CIM 0xCD - suspect part)");
    ESP_LOGI(TAG, "  platform   0x%02X  model 0x%02X  ROM mask 0x%04X",
             info.id.platform, info.id.model, info.id.rom_mask);
    ESP_LOGI(TAG, "  firmware   id 0x%08lX  build 0x%04X",
             (unsigned long)info.id.fw_id, info.id.esw_build);
    ESP_LOGI(TAG, "  wafer      x %u  y %u", info.id.x, info.id.y);
    ESP_LOGI(TAG, "LCS global   0x%02X %s", info.lcs_global, optiga_lcs_name(info.lcs_global));
    if (info.have_lcs_app)
        ESP_LOGI(TAG, "LCS app      0x%02X %s", info.lcs_app, optiga_lcs_name(info.lcs_app));
    if (info.have_sec)
        ESP_LOGI(TAG, "SEC          %u (security event counter; 0 on a fresh chip)", info.sec);
    if (info.have_max_com)
        ESP_LOGI(TAG, "max com buf  %u", info.max_com_buf);
    if (info.have_cert) {
        ESP_LOGI(TAG, "cert 0xE0E0  object %u B, X.509 DER %u B",
                 (unsigned)info.cert_object_len, (unsigned)info.cert_len);
        hex("  sha256 ", info.cert_sha256, sizeof info.cert_sha256);
    } else {
        ESP_LOGW(TAG, "cert 0xE0E0  %u B, not recognised as a certificate",
                 (unsigned)info.cert_object_len);
    }

    dump_metadata();

    static leek_se_selftest_t st;
    if (leek_se_selftest(&st) != ESP_OK) {
        ESP_LOGE(TAG, "self-test could not run: %s", leek_se_last_error());
        return;
    }
    ESP_LOGI(TAG, "self-test: UID stable %s, TRNG %s (%u/%u ones, run %u, max byte x%u%s%s), "
             "draws differ %s",
             st.uid_stable ? "yes" : "NO",
             st.rng_ok ? "ok" : "FAILED",
             (unsigned)st.rng.ones, (unsigned)st.rng.bits,
             (unsigned)st.rng.longest_run, (unsigned)st.rng.max_byte_count,
             st.rng.why ? ": " : "", st.rng.why ? st.rng.why : "",
             st.rng_distinct ? "yes" : "NO");
    ESP_LOGI(TAG, "RESULT: %s (%lld ms)", st.passed ? "PASS" : "FAIL",
             (long long)((esp_timer_get_time() - t0) / 1000));

    char lines[8][LEEK_SE_LINE_LEN];
    size_t nl = leek_se_summary_lines(&info, lines, 8);
    ESP_LOGI(TAG, "screen preview:");
    for (size_t i = 0; i < nl; i++)
        ESP_LOGI(TAG, "  |%-21s|", lines[i]);
}

static void bench_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));    /* let the boot log drain */
    if (!bus_init() || leek_se_attach(SE_I2C_PORT) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        report();
        vTaskDelay(pdMS_TO_TICKS(REPEAT_MS));
    }
}

void se_bench_start(void)
{
    xTaskCreate(bench_task, "se_bench", 6144, NULL, 5, NULL);
}

#endif /* LEEK_SE_BENCH */
