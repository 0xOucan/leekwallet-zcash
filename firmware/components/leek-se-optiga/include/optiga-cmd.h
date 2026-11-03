/**
 * OPTIGA Trust M — the three read-only commands, end to end over a link.
 *
 * Pure C like the two layers under it. Each call builds the APDU
 * (optiga-proto.c), runs it (optiga-link.c) and checks the response; when the
 * chip refuses a command, the reason is fetched from 0xF1C2 straight away,
 * before anything else can overwrite it.
 *
 * There is intentionally no optiga_cmd_set_* and no way to send an arbitrary
 * APDU. See optiga-proto.h.
 */

#ifndef OPTIGA_CMD_H
#define OPTIGA_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "optiga-link.h"

/** Open the Trust M application with a fresh (volatile) context. Required
 *  before any other command. `chip_err` gets 0xF1C2 on OPTIGA_E_CHIP and may
 *  be NULL. */
int optiga_cmd_open_app(optiga_link_t *l, uint8_t *chip_err);

/** Read a whole data object, or its metadata. */
int optiga_cmd_get_data(optiga_link_t *l, uint16_t oid, bool metadata,
                        uint8_t *out, size_t cap, size_t *out_len,
                        uint8_t *chip_err);

/** `len` (8..256) bytes from the chip's TRNG, for the self-test only. */
int optiga_cmd_get_random(optiga_link_t *l, uint8_t *out, uint16_t len,
                          uint8_t *chip_err);

#endif /* OPTIGA_CMD_H */
