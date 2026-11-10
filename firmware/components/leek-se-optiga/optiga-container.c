/* See optiga-container.h. Compiled to nothing unless
   LEEK_SE_CONTAINER_WRITES=1. */

#include "optiga-container.h"

#if LEEK_SE_CONTAINER_WRITES

#include <string.h>

#include "optiga-cmd.h"

static const uint8_t MAGIC[4] = { 'L', 'K', 'S', 'C' };

/*
 * The object image, for reads and for building a write. Static, in internal
 * RAM (.bss, never PSRAM), because 1.5 KB is too much for a caller's stack;
 * the link is used under one lock, so one buffer is enough. Wiped before
 * every return.
 */
static uint8_t img[OPTIGA_CONTAINER_OBJ_SIZE];

static void wipe(optiga_link_t *l)
{
    memset(img, 0, sizeof img);
    optiga_cmd_wipe(l);
}

uint32_t optiga_container_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= p[i];
    return acc == 0;
}

optiga_container_state_t optiga_container_parse(const uint8_t *obj, size_t n,
                                                const uint8_t **payload,
                                                size_t *len)
{
    *payload = NULL;
    *len = 0;
    if (n == 0 || all_zero(obj, n))
        return OPTIGA_CONTAINER_EMPTY;

    bool magic = n >= 4 && memcmp(obj, MAGIC, 4) == 0;
    bool unsigned_hdr = n >= 4 && all_zero(obj, 4);
    if (!magic && !unsigned_hdr)
        return OPTIGA_CONTAINER_FOREIGN;
    if (n < OPTIGA_CONTAINER_HEADER)
        return magic ? OPTIGA_CONTAINER_DAMAGED : OPTIGA_CONTAINER_FOREIGN;

    /* A newer format is not ours to judge, or to overwrite. */
    if (obj[4] != OPTIGA_CONTAINER_VERSION)
        return OPTIGA_CONTAINER_FOREIGN;
    size_t plen = ((size_t)obj[6] << 8) | obj[7];
    bool shape = obj[5] == 0 && all_zero(&obj[12], 4) &&
                 plen <= OPTIGA_CONTAINER_CAPACITY;

    if (!magic)
        /* Zero magic over a header of ours: a write cut short, so the
           payload may well be shorter than it says. Over anything else it
           is someone else's data that happens to start with four zeros. */
        return shape ? OPTIGA_CONTAINER_INCOMPLETE : OPTIGA_CONTAINER_FOREIGN;
    if (!shape || plen > n - OPTIGA_CONTAINER_HEADER)
        return OPTIGA_CONTAINER_DAMAGED;

    const uint8_t *p = &obj[OPTIGA_CONTAINER_HEADER];
    if (optiga_container_crc32(p, plen) != be32(&obj[8]))
        return OPTIGA_CONTAINER_DAMAGED;
    *payload = p;
    *len = plen;
    return OPTIGA_CONTAINER_OK;
}

/* Read the object into img and classify it. */
static int load(optiga_link_t *l, optiga_container_state_t *state,
                const uint8_t **payload, size_t *plen, uint8_t *chip_err)
{
    size_t n = 0;
    int err = optiga_cmd_get_data(l, OPTIGA_OID_CONTAINER, false, img,
                                  sizeof img, &n, chip_err);
    if (err)
        return err;
    *state = optiga_container_parse(img, n, payload, plen);
    return OPTIGA_OK;
}

/* Write img[0..total) from offset 0: the first command erases the object,
   the rest append. */
static int store(optiga_link_t *l, size_t total, uint8_t *chip_err)
{
    for (size_t off = 0; off < total; off += OPTIGA_SET_CHUNK_MAX) {
        size_t n = total - off;
        if (n > OPTIGA_SET_CHUNK_MAX)
            n = OPTIGA_SET_CHUNK_MAX;
        int err = optiga_cmd_set_data(l, OPTIGA_OID_CONTAINER, off == 0,
                                      (uint16_t)off, &img[off], n, chip_err);
        if (err)
            return err;
    }
    return OPTIGA_OK;
}

int optiga_container_read(optiga_link_t *l, uint8_t *out, size_t cap,
                          size_t *len, optiga_container_state_t *state,
                          uint8_t *chip_err)
{
    if (chip_err)
        *chip_err = 0;
    if (!l || !out || !len || !state)
        return OPTIGA_E_PARAM;
    *len = 0;
    *state = OPTIGA_CONTAINER_EMPTY;
    const uint8_t *p;
    size_t plen;
    int err = load(l, state, &p, &plen, chip_err);
    if (!err && *state == OPTIGA_CONTAINER_OK) {
        if (plen > cap) {
            err = OPTIGA_E_SIZE;
        } else {
            memcpy(out, p, plen);
            *len = plen;
        }
    }
    wipe(l);
    return err;
}

int optiga_container_write(optiga_link_t *l, const uint8_t *data, size_t len,
                           uint8_t *chip_err)
{
    if (chip_err)
        *chip_err = 0;
    if (!l || !data || len == 0 || len > OPTIGA_CONTAINER_CAPACITY)
        return OPTIGA_E_PARAM;

    /* Never overwrite data that is not ours. */
    optiga_container_state_t st;
    const uint8_t *p;
    size_t plen;
    int err = load(l, &st, &p, &plen, chip_err);
    if (!err && st == OPTIGA_CONTAINER_FOREIGN)
        err = OPTIGA_E_FOREIGN;
    if (err)
        goto out;

    /* The header with its magic left zero, then the payload. */
    memset(img, 0, sizeof img);
    uint32_t crc = optiga_container_crc32(data, len);
    img[4] = OPTIGA_CONTAINER_VERSION;
    img[6] = (uint8_t)(len >> 8);
    img[7] = (uint8_t)len;
    img[8] = (uint8_t)(crc >> 24);
    img[9] = (uint8_t)(crc >> 16);
    img[10] = (uint8_t)(crc >> 8);
    img[11] = (uint8_t)crc;
    memcpy(&img[OPTIGA_CONTAINER_HEADER], data, len);
    err = store(l, OPTIGA_CONTAINER_HEADER + len, chip_err);
    if (err)
        goto out;

    /* Last: the magic, which is what makes the blob valid. */
    err = optiga_cmd_set_data(l, OPTIGA_OID_CONTAINER, false, 0, MAGIC,
                              sizeof MAGIC, chip_err);
    if (err)
        goto out;

    err = load(l, &st, &p, &plen, chip_err);
    if (!err && (st != OPTIGA_CONTAINER_OK || plen != len ||
                 memcmp(p, data, len) != 0))
        err = OPTIGA_E_VERIFY;
out:
    wipe(l);
    return err;
}

int optiga_container_erase(optiga_link_t *l, uint8_t *chip_err)
{
    if (chip_err)
        *chip_err = 0;
    if (!l)
        return OPTIGA_E_PARAM;
    /* Zeros over every byte, not just an erase: the manual's erase is not
       relied on to scrub what was there. */
    memset(img, 0, sizeof img);
    int err = store(l, sizeof img, chip_err);
    if (!err) {
        optiga_container_state_t st;
        const uint8_t *p;
        size_t plen;
        err = load(l, &st, &p, &plen, chip_err);
        if (!err && st != OPTIGA_CONTAINER_EMPTY)
            err = OPTIGA_E_VERIFY;
    }
    wipe(l);
    return err;
}

const char *optiga_container_err_name(int err)
{
    switch (err) {
    case OPTIGA_E_FOREIGN: return "object holds other data; erase it first";
    case OPTIGA_E_VERIFY:  return "read-back did not match";
    default:               return optiga_link_err_name(err);
    }
}

#else

/* ISO C wants at least one declaration in a translation unit. */
typedef int optiga_container_disabled;

#endif /* LEEK_SE_CONTAINER_WRITES */
