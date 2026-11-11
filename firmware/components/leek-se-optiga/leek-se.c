/* See leek-se.h. The only file in this component that knows it is on an
   ESP32: it turns the legacy I2C driver into an optiga_bus_t and adds a lock. */

#include "leek-se.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "mbedtls/sha256.h"

#include "optiga-cmd.h"
#include "optiga-link.h"

/* Per transaction. Long enough for the largest frame at 100 kHz with margin;
   a NACK comes back far sooner than this either way. */
#define I2C_TIMEOUT_MS  25

static i2c_port_t s_port = I2C_NUM_MAX;
static SemaphoreHandle_t s_lock;
static optiga_link_t s_link;
static bool s_open;
static int s_err;
static uint8_t s_chip_err;
static char s_err_text[96];

/* Room for the largest certificate object; static for the same reason as the
   response buffer in optiga-cmd.c. */
static uint8_t s_obj[1728];

/* ------------------------------------------------------------- the bus */

static int bus_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    return i2c_master_write_to_device(s_port, OPTIGA_I2C_ADDR, buf, n,
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK ? 0 : -1;
}

static int bus_read(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    return i2c_master_read_from_device(s_port, OPTIGA_I2C_ADDR, buf, n,
                                       pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK ? 0 : -1;
}

static void bus_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    /* The link waits in 1 ms steps. At a tick rate under 1 kHz that rounds
       to zero ticks, which would turn every timeout in optiga-link.c into a
       busy spin of unknown length, so short waits are spun on purpose. */
    TickType_t t = pdMS_TO_TICKS(ms);
    if (t == 0)
        esp_rom_delay_us(ms * 1000);
    else
        vTaskDelay(t);
}

static const optiga_bus_t BUS = { NULL, bus_write, bus_read, bus_delay };

/* ------------------------------------------------------------ helpers */

static esp_err_t fail(int err, uint8_t chip_err)
{
    s_err = err;
    s_chip_err = chip_err;
    /* Anything but a clean refusal may have left the frame counters out of
       step; the next call re-initialises rather than guessing. */
    if (err != OPTIGA_E_CHIP)
        s_open = false;
    return ESP_FAIL;
}

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t open_locked(void)
{
    s_err = OPTIGA_OK;
    s_chip_err = 0;
    int err = optiga_link_init(&s_link, &BUS);
    if (err)
        return fail(err, 0);
    uint8_t ce = 0;
    err = optiga_cmd_open_app(&s_link, &ce);
    if (err)
        return fail(err, ce);
    s_open = true;
    return ESP_OK;
}

static esp_err_t ensure_open(void)
{
    return s_open ? ESP_OK : open_locked();
}

static int read_locked(uint16_t oid, bool meta, uint8_t *out, size_t cap,
                       size_t *len)
{
    uint8_t ce = 0;
    int err = optiga_cmd_get_data(&s_link, oid, meta, out, cap, len, &ce);
    if (err)
        fail(err, ce);
    return err;
}

/* ------------------------------------------------------------- the API */

esp_err_t leek_se_attach(i2c_port_t port)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock)
            return ESP_ERR_NO_MEM;
    }
    lock();
    if (s_port != port)
        s_open = false;
    s_port = port;
    unlock();
    return ESP_OK;
}

bool leek_se_probe(void)
{
    if (!s_lock)
        return false;
    lock();
    /* A zero-length write is an address-only transaction: ACK or not. */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OPTIGA_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(s_port, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    unlock();
    return err == ESP_OK;
}

esp_err_t leek_se_open(void)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t r = open_locked();
    unlock();
    return r;
}

esp_err_t leek_se_info(leek_se_info_t *info)
{
    if (!s_lock || !info)
        return ESP_ERR_INVALID_STATE;
    memset(info, 0, sizeof *info);
    lock();
    esp_err_t r = ensure_open();
    if (r != ESP_OK)
        goto out;
    info->data_reg_len = s_link.data_reg_len;

    size_t n = 0;
    uint8_t b[4];

    /* The UID is the one read that must succeed: it is what identifies the
       part. The rest are reported when present and skipped when not, so a
       firmware revision that lacks one object still prints everything else. */
    if (read_locked(OPTIGA_OID_COPROC_UID, false, info->uid, sizeof info->uid, &n) ||
        !optiga_parse_uid(info->uid, n, &info->id)) {
        if (s_err == OPTIGA_OK)
            fail(OPTIGA_E_REPLY, 0);
        r = ESP_FAIL;
        goto out;
    }
    info->infineon = info->id.cim == OPTIGA_CIM_INFINEON;

    if (read_locked(OPTIGA_OID_LCS_GLOBAL, false, b, sizeof b, &n) == 0 && n == 1)
        info->lcs_global = b[0];
    if (read_locked(OPTIGA_OID_LCS_APP, false, b, sizeof b, &n) == 0 && n == 1) {
        info->lcs_app = b[0];
        info->have_lcs_app = true;
    }
    if (read_locked(OPTIGA_OID_SEC, false, b, sizeof b, &n) == 0 && n == 1) {
        info->sec = b[0];
        info->have_sec = true;
    }
    if (read_locked(OPTIGA_OID_MAX_COM_BUF, false, b, sizeof b, &n) == 0 && n == 2) {
        info->max_com_buf = (uint16_t)((b[0] << 8) | b[1]);
        info->have_max_com = true;
    }
    if (read_locked(OPTIGA_OID_DEVICE_CERT, false, s_obj, sizeof s_obj, &n) == 0) {
        const uint8_t *der;
        size_t dn;
        info->cert_object_len = n;
        if (optiga_cert_find(s_obj, n, &der, &dn)) {
            info->have_cert = true;
            info->cert_len = dn;
            mbedtls_sha256(der, dn, info->cert_sha256, 0);
        }
    }
    /* Optional reads may have recorded an error; the call as a whole still
       succeeded, and the link is re-opened next time if one broke it. */
    r = ESP_OK;
out:
    unlock();
    return r;
}

esp_err_t leek_se_read(uint16_t oid, bool metadata, uint8_t *out, size_t cap,
                       size_t *len)
{
    if (!s_lock || !out || !len)
        return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t r = ensure_open();
    if (r == ESP_OK && read_locked(oid, metadata, out, cap, len) != OPTIGA_OK)
        r = ESP_FAIL;
    unlock();
    return r;
}

esp_err_t leek_se_selftest(leek_se_selftest_t *st)
{
    if (!s_lock || !st)
        return ESP_ERR_INVALID_STATE;
    memset(st, 0, sizeof *st);
    lock();
    esp_err_t r = ensure_open();
    if (r != ESP_OK)
        goto out;

    /* A deterministic read twice: catches a link that "works" by returning
       whatever happens to be in a buffer. */
    uint8_t u1[OPTIGA_UID_LEN], u2[OPTIGA_UID_LEN];
    size_t n1 = 0, n2 = 0;
    if (read_locked(OPTIGA_OID_COPROC_UID, false, u1, sizeof u1, &n1) ||
        read_locked(OPTIGA_OID_COPROC_UID, false, u2, sizeof u2, &n2)) {
        r = ESP_FAIL;
        goto out;
    }
    st->uid_stable = n1 == OPTIGA_UID_LEN && n1 == n2 && memcmp(u1, u2, n1) == 0;

    /* The TRNG: one long draw for the sanity check, two short ones that must
       differ. Volatile - GetRandom changes nothing on the chip. */
    static uint8_t big[256];
    uint8_t a[32], b[32], ce = 0;
    int err = optiga_cmd_get_random(&s_link, big, sizeof big, &ce);
    if (!err)
        err = optiga_cmd_get_random(&s_link, a, sizeof a, &ce);
    if (!err)
        err = optiga_cmd_get_random(&s_link, b, sizeof b, &ce);
    if (err) {
        r = fail(err, ce);
        goto out;
    }
    st->rng_ok = optiga_rng_sanity(big, sizeof big, &st->rng);
    st->rng_distinct = memcmp(a, b, sizeof a) != 0;
    st->passed = st->uid_stable && st->rng_ok && st->rng_distinct;

    /* Never entropy for anything; do not leave it around to become some. */
    memset(big, 0, sizeof big);
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
out:
    unlock();
    return r;
}

#if LEEK_SE_CONTAINER_WRITES
size_t leek_se_container_capacity(void)
{
    return OPTIGA_CONTAINER_CAPACITY;
}

esp_err_t leek_se_container_read(uint8_t *out, size_t cap, size_t *len,
                                 optiga_container_state_t *state)
{
    if (!s_lock || !out || !len || !state)
        return ESP_ERR_INVALID_STATE;
    *len = 0;
    *state = OPTIGA_CONTAINER_EMPTY;
    lock();
    esp_err_t r = ensure_open();
    if (r == ESP_OK) {
        uint8_t ce = 0;
        int err = optiga_container_read(&s_link, out, cap, len, state, &ce);
        if (err)
            r = fail(err, ce);
    }
    unlock();
    return r;
}

esp_err_t leek_se_container_write(const uint8_t *data, size_t len)
{
    if (!s_lock || !data)
        return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > OPTIGA_CONTAINER_CAPACITY)
        return ESP_ERR_INVALID_SIZE;
    lock();
    esp_err_t r = ensure_open();
    if (r == ESP_OK) {
        uint8_t ce = 0;
        int err = optiga_container_write(&s_link, data, len, &ce);
        if (err)
            r = fail(err, ce);
    }
    unlock();
    return r;
}

esp_err_t leek_se_container_erase(void)
{
    if (!s_lock)
        return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t r = ensure_open();
    if (r == ESP_OK) {
        uint8_t ce = 0;
        int err = optiga_container_erase(&s_link, &ce);
        if (err)
            r = fail(err, ce);
    }
    unlock();
    return r;
}
#endif

int leek_se_last_err(void)
{
    return s_err;
}

const char *leek_se_last_error(void)
{
#if LEEK_SE_CONTAINER_WRITES
    if (s_err == OPTIGA_E_FOREIGN || s_err == OPTIGA_E_VERIFY) {
        snprintf(s_err_text, sizeof s_err_text, "%s",
                 optiga_container_err_name(s_err));
        return s_err_text;
    }
#endif
    if (s_err == OPTIGA_E_CHIP)
        snprintf(s_err_text, sizeof s_err_text, "%s: 0x%02X %s",
                 optiga_link_err_name(s_err), s_chip_err,
                 optiga_error_name(s_chip_err));
    else
        snprintf(s_err_text, sizeof s_err_text, "%s", optiga_link_err_name(s_err));
    return s_err_text;
}

size_t leek_se_summary_lines(const leek_se_info_t *info,
                             char lines[][LEEK_SE_LINE_LEN], size_t max)
{
    size_t i = 0;
#define LINE(...) do { if (i < max) snprintf(lines[i++], LEEK_SE_LINE_LEN, __VA_ARGS__); } while (0)
    LINE("OPTIGA Trust M");
    LINE("%s", info->infineon ? "Infineon (CIM CD)" : "Unknown maker!");
    LINE("FW %08lX b%04X", (unsigned long)info->id.fw_id, info->id.esw_build);
    LINE("LCS %s", optiga_lcs_name(info->lcs_global));
    if (info->have_sec)
        LINE("Sec events %u", info->sec);
    if (info->have_cert) {
        const uint8_t *h = info->cert_sha256;
        LINE("Cert %u B", (unsigned)info->cert_len);
        LINE("%02X%02X%02X%02X %02X%02X%02X%02X", h[0], h[1], h[2], h[3],
             h[4], h[5], h[6], h[7]);
    } else {
        LINE("No certificate");
    }
#undef LINE
    return i;
}
