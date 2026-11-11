/**
 * The secure element hook (se.h) for an OPTIGA Trust M, and the container
 * backend se-backup.c stores its copy through. Empty unless the build names
 * the part (-DLEEK_SE=LEEK_SE_OPTIGA, as esp32s3cam-color-se does).
 *
 * The bus. The Trust M sits on I2C0 (PIN_I2C_SDA / PIN_I2C_SCL, GPIO47/21
 * on the CAM board), on the LEGACY driver like everything else in this image
 * (sdkconfig.s3cam: ESP-IDF 5.5 refuses both drivers in one image). On a kit
 * with an I2C panel, oled_i2c_init() has installed that driver before main.c
 * calls se_probe(), and the panel's install is reused; never a second one.
 * On the colour kit the panel is SPI, oled_i2c_init() touches nothing, and the
 * bus is the chip's alone, so it is installed here with the settings
 * se-bench.c and oled.c use (400 kHz, internal pull-ups). The camera's SCCB
 * is on I2C1 and is not affected either way.
 */

#include "se.h"
#include "board.h"

#if LEEK_SE == LEEK_SE_OPTIGA

#include <stdio.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_log.h"

#include "leek-se.h"
#include "se-backup.h"

static const char *TAG = "se";

#define SE_I2C_PORT  I2C_NUM_0
#define SE_I2C_HZ    400000

static bool s_attached;

static bool bus_init(void)
{
#if BOARD_HAS_I2C_PANEL
    /* oled_i2c_init() owns the install on this kit. */
    return true;
#else
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SE_I2C_HZ,
    };
    esp_err_t err = i2c_param_config(SE_I2C_PORT, &conf);
    if (err == ESP_OK) {
        err = i2c_driver_install(SE_I2C_PORT, conf.mode, 0, 0, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 install failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "I2C0 for the SE: SDA=GPIO%d SCL=GPIO%d %d Hz", PIN_I2C_SDA, PIN_I2C_SCL,
             SE_I2C_HZ);
    return true;
#endif
}

static bool attach(void)
{
    if (!s_attached && bus_init() && leek_se_attach(SE_I2C_PORT) == ESP_OK) {
        s_attached = true;
    }
    return s_attached;
}

se_status_t se_probe(void)
{
    if (!attach()) {
        return SE_FAULT;
    }
    if (!leek_se_probe()) {
        return SE_ABSENT;
    }
    /* Warm reset and OpenApplication: volatile, a few ms. The certificate
     * and the rest of the identity are read when the Info screen asks. */
    if (leek_se_open() != ESP_OK) {
        ESP_LOGW(TAG, "OPTIGA at 0x%02X did not open: %s", OPTIGA_I2C_ADDR,
                 leek_se_last_error());
        return SE_FAULT;
    }
    return SE_PRESENT;
}

const char *se_name(void) { return "OPTIGA Trust M"; }

int se_info_lines(char lines[][SE_LINE_LEN], int max)
{
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(lines[n++], SE_LINE_LEN, __VA_ARGS__); } while (0)
    if (!attach() || !leek_se_probe()) {
        LINE("Chip: not found");
        return n;
    }
    leek_se_info_t info;
    if (leek_se_info(&info) != ESP_OK) {
        LINE("Chip: no answer");
        return n;
    }
    LINE("Chip: OPTIGA Trust M");
    LINE("Maker: %s", info.infineon ? "Infineon" : "NOT Infineon!");
    LINE("LCS: %s", optiga_lcs_name(info.lcs_global));
    if (info.have_sec) {
        LINE("SEC: %u", (unsigned)info.sec);
    } else {
        LINE("SEC: unread");
    }
#undef LINE
    return n;
}

/* ------------------------------------------------ se-backup.h's backend */

#if LEEK_SE_CONTAINER_WRITES

bool sebk_store_present(void)
{
    return attach() && leek_se_probe();
}

int sebk_store_read(uint8_t *out, size_t cap, size_t *len, SeBackupObj *state)
{
    optiga_container_state_t st = OPTIGA_CONTAINER_EMPTY;
    *len = 0;
    *state = SEBK_OBJ_EMPTY;
    if (leek_se_container_read(out, cap, len, &st) != ESP_OK) {
        *len = 0;
        return leek_se_last_err() == OPTIGA_E_SIZE ? SEBK_STORE_E_BIG : SEBK_STORE_E_IO;
    }
    switch (st) {
        case OPTIGA_CONTAINER_EMPTY:      *state = SEBK_OBJ_EMPTY; break;
        case OPTIGA_CONTAINER_OK:         *state = SEBK_OBJ_OK; break;
        case OPTIGA_CONTAINER_INCOMPLETE: *state = SEBK_OBJ_INCOMPLETE; break;
        case OPTIGA_CONTAINER_DAMAGED:    *state = SEBK_OBJ_DAMAGED; break;
        default:                          *state = SEBK_OBJ_FOREIGN; break;
    }
    return SEBK_STORE_OK;
}

int sebk_store_write(const uint8_t *data, size_t len)
{
    if (leek_se_container_write(data, len) == ESP_OK) {
        return SEBK_STORE_OK;
    }
    const int err = leek_se_last_err();
    ESP_LOGW(TAG, "container write: %s", leek_se_last_error());
    return err == OPTIGA_E_FOREIGN ? SEBK_STORE_E_FOREIGN
         : err == OPTIGA_E_VERIFY  ? SEBK_STORE_E_VERIFY
                                   : SEBK_STORE_E_IO;
}

int sebk_store_erase(void)
{
    if (leek_se_container_erase() == ESP_OK) {
        return SEBK_STORE_OK;
    }
    ESP_LOGW(TAG, "container erase: %s", leek_se_last_error());
    return SEBK_STORE_E_IO;
}

#endif /* LEEK_SE_CONTAINER_WRITES */

#endif /* LEEK_SE == LEEK_SE_OPTIGA */
