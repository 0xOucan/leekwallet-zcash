/* See optiga-cmd.h. */

#include "optiga-cmd.h"

#include <string.h>

#include "optiga-proto.h"

/*
 * The largest response any command here can produce: a 1728-byte certificate
 * object (the Trust M maximum for 0xE0E0-0xE0E3) plus the 4-byte header.
 * Static, because the link is used under one lock and 1.7 KB is too much to
 * put on a FreeRTOS task's stack for every caller.
 */
#define RESP_MAX    (1728 + 4)
static uint8_t resp[RESP_MAX];

/* Run `cmd` and leave OutData in `data`/`len` (pointing into resp). */
static int run(optiga_link_t *l, const uint8_t *cmd, size_t cmd_len,
               const uint8_t **data, size_t *len, uint8_t *chip_err)
{
    size_t n = 0;
    if (chip_err)
        *chip_err = 0;
    int err = optiga_link_transceive(l, cmd, cmd_len, resp, sizeof resp, &n);
    if (err)
        return err;
    switch (optiga_apdu_response(resp, n, data, len)) {
    case OPTIGA_APDU_OK:
        return OPTIGA_OK;
    case OPTIGA_APDU_FAILED:
        break;
    default:
        return OPTIGA_E_REPLY;
    }

    /* Refused. Ask why, without flushing the answer away. */
    if (chip_err) {
        uint8_t q[OPTIGA_APDU_MAX];
        size_t qn = optiga_apdu_get_error(q);
        const uint8_t *e;
        size_t el;
        if (optiga_link_transceive(l, q, qn, resp, sizeof resp, &n) == OPTIGA_OK &&
            optiga_apdu_response(resp, n, &e, &el) == OPTIGA_APDU_OK && el == 1)
            *chip_err = e[0];
    }
    return OPTIGA_E_CHIP;
}

int optiga_cmd_open_app(optiga_link_t *l, uint8_t *chip_err)
{
    uint8_t cmd[OPTIGA_APDU_MAX];
    size_t n = optiga_apdu_open_app(cmd);
    const uint8_t *data;
    size_t len;
    int err = run(l, cmd, n, &data, &len, chip_err);
    if (err)
        return err;
    return len == 0 ? OPTIGA_OK : OPTIGA_E_REPLY;
}

int optiga_cmd_get_data(optiga_link_t *l, uint16_t oid, bool metadata,
                        uint8_t *out, size_t cap, size_t *out_len,
                        uint8_t *chip_err)
{
    *out_len = 0;
    uint8_t cmd[OPTIGA_APDU_MAX];
    size_t n = optiga_apdu_get_data(cmd, oid, metadata);
    const uint8_t *data;
    size_t len;
    int err = run(l, cmd, n, &data, &len, chip_err);
    if (err)
        return err;
    if (len > cap)
        return OPTIGA_E_SIZE;
    memcpy(out, data, len);
    *out_len = len;
    return OPTIGA_OK;
}

int optiga_cmd_get_random(optiga_link_t *l, uint8_t *out, uint16_t want,
                          uint8_t *chip_err)
{
    uint8_t cmd[OPTIGA_APDU_MAX];
    size_t n = optiga_apdu_get_random(cmd, want);
    if (n == 0)
        return OPTIGA_E_PARAM;
    const uint8_t *data;
    size_t len;
    int err = run(l, cmd, n, &data, &len, chip_err);
    if (err)
        return err;
    if (len != want)
        return OPTIGA_E_REPLY;
    memcpy(out, data, len);
    /* Not secret - it is never used as one - but there is no reason to leave
       it lying in a shared buffer either. */
    memset(resp, 0, sizeof resp);
    return OPTIGA_OK;
}

#if LEEK_SE_CONTAINER_WRITES
int optiga_cmd_set_data(optiga_link_t *l, uint16_t oid, bool erase_first,
                        uint16_t offset, const uint8_t *data, size_t len,
                        uint8_t *chip_err)
{
    if (chip_err)
        *chip_err = 0;
    uint8_t cmd[OPTIGA_APDU_SET_MAX];
    size_t n = optiga_apdu_set_data(cmd, sizeof cmd, oid, erase_first, offset,
                                    data, len);
    if (n == 0)
        return OPTIGA_E_PARAM;
    const uint8_t *out;
    size_t out_len;
    int err = run(l, cmd, n, &out, &out_len, chip_err);
    memset(cmd, 0, sizeof cmd);
    if (err)
        return err;
    return out_len == 0 ? OPTIGA_OK : OPTIGA_E_REPLY;
}

void optiga_cmd_wipe(optiga_link_t *l)
{
    memset(resp, 0, sizeof resp);
    if (l) {
        memset(l->tx, 0, sizeof l->tx);
        memset(l->rx, 0, sizeof l->rx);
        l->rx_len = 0;
    }
}
#endif
