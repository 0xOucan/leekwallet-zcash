/**
 * The SE container on the host: se-backup.h's four store functions over a
 * 1500-byte image of OPTIGA object 0xF1E0, plus se_info_lines() (se.h).
 *
 * The object image uses the real container format: the LKSC header is built
 * here byte for byte as optiga-container.h lays it out, and every read is
 * classified by the real optiga_container_parse(). So what se-backup.c meets
 * here is what it would meet over the link; the link and the chip themselves
 * are test_se_container's (sim/test_se_optiga.c), not this file's.
 *
 * Fault switches: no chip, link errors, a chip that refuses the write as
 * foreign, a chip whose read-back differs, and one that silently stores
 * something else. fake_se_image() gives the test the raw object to plant
 * foreign, damaged or cut data in.
 */
#include "fake_se_store.h"

#include <stdio.h>
#include <string.h>

#include "optiga-container.h"
#include "se.h"

static uint8_t obj[OPTIGA_CONTAINER_OBJ_SIZE];
static bool present = true;
static bool fail_io;
static bool refuse_foreign_write;
static bool verify_fails;
static bool corrupt_writes;
static int writes, erases;

void fake_se_reset(void)
{
    memset(obj, 0, sizeof obj);
    present = true;
    fail_io = refuse_foreign_write = verify_fails = corrupt_writes = false;
    writes = erases = 0;
}

uint8_t *fake_se_image(void) { return obj; }
void fake_se_present(bool p) { present = p; }
void fake_se_fail_io(bool f) { fail_io = f; }
void fake_se_verify_fails(bool f) { verify_fails = f; }
void fake_se_corrupt_writes(bool f) { corrupt_writes = f; }
int fake_se_writes(void) { return writes; }
int fake_se_erases(void) { return erases; }

void fake_se_put_container(const uint8_t *payload, size_t len)
{
    memset(obj, 0, sizeof obj);
    memcpy(obj, "LKSC", 4);
    obj[4] = OPTIGA_CONTAINER_VERSION;
    obj[5] = 0;
    obj[6] = (uint8_t)(len >> 8);
    obj[7] = (uint8_t)len;
    uint32_t crc = optiga_container_crc32(payload, len);
    obj[8] = (uint8_t)(crc >> 24);
    obj[9] = (uint8_t)(crc >> 16);
    obj[10] = (uint8_t)(crc >> 8);
    obj[11] = (uint8_t)crc;
    memcpy(obj + OPTIGA_CONTAINER_HEADER, payload, len);
}

bool sebk_store_present(void) { return present; }

int sebk_store_read(uint8_t *out, size_t cap, size_t *len, SeBackupObj *state)
{
    *len = 0;
    *state = SEBK_OBJ_EMPTY;
    if (!present || fail_io) return SEBK_STORE_E_IO;
    const uint8_t *p = NULL;
    size_t n = 0;
    switch (optiga_container_parse(obj, sizeof obj, &p, &n)) {
        case OPTIGA_CONTAINER_EMPTY:      *state = SEBK_OBJ_EMPTY; return SEBK_STORE_OK;
        case OPTIGA_CONTAINER_INCOMPLETE: *state = SEBK_OBJ_INCOMPLETE; return SEBK_STORE_OK;
        case OPTIGA_CONTAINER_DAMAGED:    *state = SEBK_OBJ_DAMAGED; return SEBK_STORE_OK;
        case OPTIGA_CONTAINER_FOREIGN:    *state = SEBK_OBJ_FOREIGN; return SEBK_STORE_OK;
        case OPTIGA_CONTAINER_OK:
            if (n > cap) return SEBK_STORE_E_BIG;
            memcpy(out, p, n);
            *len = n;
            *state = SEBK_OBJ_OK;
            return SEBK_STORE_OK;
    }
    return SEBK_STORE_E_IO;
}

int sebk_store_write(const uint8_t *data, size_t len)
{
    if (!present || fail_io) return SEBK_STORE_E_IO;
    if (len == 0 || len > OPTIGA_CONTAINER_CAPACITY) return SEBK_STORE_E_IO;
    /* optiga_container_write's rule: never over someone else's data. */
    const uint8_t *p = NULL;
    size_t n = 0;
    if (optiga_container_parse(obj, sizeof obj, &p, &n) == OPTIGA_CONTAINER_FOREIGN ||
        refuse_foreign_write) {
        return SEBK_STORE_E_FOREIGN;
    }
    writes++;
    fake_se_put_container(data, len);
    if (corrupt_writes) {
        /* Stored, CRC and all, but not what was sent - and the chip says OK. */
        obj[OPTIGA_CONTAINER_HEADER + len - 1] ^= 0x01;
        fake_se_put_container(obj + OPTIGA_CONTAINER_HEADER, len);
    }
    return verify_fails ? SEBK_STORE_E_VERIFY : SEBK_STORE_OK;
}

int sebk_store_erase(void)
{
    if (!present || fail_io) return SEBK_STORE_E_IO;
    erases++;
    memset(obj, 0, sizeof obj);
    return SEBK_STORE_OK;
}

int se_info_lines(char lines[][SE_LINE_LEN], int max)
{
    int n = 0;
    if (!present) {
        if (n < max) snprintf(lines[n++], SE_LINE_LEN, "Chip: not found");
        return n;
    }
    static const char *const L[] = { "Chip: OPTIGA Trust M", "Maker: Infineon",
                                     "LCS: operational", "SEC: 0" };
    for (int i = 0; i < 4 && n < max; i++) snprintf(lines[n++], SE_LINE_LEN, "%s", L[i]);
    return n;
}
