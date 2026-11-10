/**
 * OPTIGA Trust M — the "Level 1 SE container": one blob in data object 0xF1E0.
 *
 * DEVELOPMENT ONLY, and compiled only with LEEK_SE_CONTAINER_WRITES=1. With
 * the flag at 0 (the default, and every shipped image) this header declares
 * nothing.
 *
 * Level 1 is storage, not security. The object keeps its factory access
 * rules (read Always, change while LcsO < operational), so anyone with the
 * board's I2C bus can read the blob, overwrite it, or rewrite the object's
 * metadata. Whatever goes in must already be encrypted by the caller — the
 * plan is the same PIN-derived AES-256-GCM blob the NVS vault stores — and
 * the container adds nothing to that encryption. Design and the hardened
 * Level 2 that would make the chip matter: docs/RESEARCH-SECURE-ELEMENT.md.
 *
 * Object layout (all of 0xF1E0, 1500 bytes):
 *
 *     0  4  magic "LKSC"
 *     4  1  format version (1)
 *     5  1  flags (0; anything else reads as damaged)
 *     6  2  payload length, u16 big-endian (0..1484)
 *     8  4  CRC-32 (IEEE) of the payload, big-endian
 *    12  4  reserved, zero
 *    16  L  payload
 *
 * The CRC catches a torn or decayed write. It is not authentication: anyone
 * who can write the object can write a matching CRC. The payload's own GCM
 * tag is what detects tampering.
 *
 * Write order, so a power cut never leaves a blob that reads as valid: the
 * first command erases the object and writes the header with the magic
 * zeroed, the rest follow at increasing offsets, and the magic goes in last.
 * Then the whole object is read back and compared.
 *
 * Pure C over an optiga_link_t, like the layers under it; leek-se.c adds the
 * lock. Never logs, and wipes every buffer the blob passed through.
 */

#ifndef OPTIGA_CONTAINER_H
#define OPTIGA_CONTAINER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "optiga-link.h"
#include "optiga-proto.h"

#if LEEK_SE_CONTAINER_WRITES

#define OPTIGA_CONTAINER_HEADER     16
#define OPTIGA_CONTAINER_CAPACITY   (OPTIGA_CONTAINER_OBJ_SIZE - OPTIGA_CONTAINER_HEADER)
#define OPTIGA_CONTAINER_VERSION    1

typedef enum {
    OPTIGA_CONTAINER_EMPTY = 0,     /* object empty, or all zero (erased) */
    OPTIGA_CONTAINER_OK,            /* a valid container; payload returned */
    OPTIGA_CONTAINER_INCOMPLETE,    /* our header without its magic: a write
                                       that was cut before it finished */
    OPTIGA_CONTAINER_DAMAGED,       /* our magic, but header or CRC wrong */
    OPTIGA_CONTAINER_FOREIGN,       /* anything else, including a newer
                                       format version: not ours to overwrite */
} optiga_container_state_t;

/* Container-level errors, beyond the link's (optiga-link.h). */
enum {
    OPTIGA_E_FOREIGN = 100,         /* write refused: the object holds data
                                       that is not a container; erase first */
    OPTIGA_E_VERIFY,                /* read-back did not match what was written */
};

/** CRC-32 (IEEE 802.3, reflected 0xEDB88320, init and final XOR ~0). */
uint32_t optiga_container_crc32(const uint8_t *p, size_t n);

/** Classify a whole object image. On OPTIGA_CONTAINER_OK, `payload`/`len`
 *  point into `obj`; otherwise they are NULL/0. */
optiga_container_state_t optiga_container_parse(const uint8_t *obj, size_t n,
                                                const uint8_t **payload,
                                                size_t *len);

/** Read 0xF1E0 and classify it. The payload is copied to `out` only when the
 *  state is OK; OPTIGA_E_SIZE if it does not fit in `cap`. Any other state is
 *  a success with `*len` = 0: an empty or foreign object is an answer, not a
 *  failure of the read. */
int optiga_container_read(optiga_link_t *l, uint8_t *out, size_t cap,
                          size_t *len, optiga_container_state_t *state,
                          uint8_t *chip_err);

/** Store `len` (1..OPTIGA_CONTAINER_CAPACITY) bytes, replacing what was
 *  there. Refused with OPTIGA_E_FOREIGN, before any write, if the object
 *  holds data that is not a container. */
int optiga_container_write(optiga_link_t *l, const uint8_t *data, size_t len,
                           uint8_t *chip_err);

/** Overwrite the whole object with zeros. Afterwards it reads as EMPTY. */
int optiga_container_erase(optiga_link_t *l, uint8_t *chip_err);

/** optiga_link_err_name(), plus the two container errors. */
const char *optiga_container_err_name(int err);

#endif /* LEEK_SE_CONTAINER_WRITES */

#endif /* OPTIGA_CONTAINER_H */
