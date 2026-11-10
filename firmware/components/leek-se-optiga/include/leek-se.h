/**
 * Secure element bring-up: an Infineon OPTIGA Trust M on the shared I2C bus.
 *
 * RESEARCH AND TESTING ONLY. Nothing here stores a key, a PIN or a seed, and
 * in the default build nothing here can: the layers below have no way to
 * express a write to the chip (see optiga-proto.h). The one exception is the
 * development-only container at the end of this file, compiled only with
 * LEEK_SE_CONTAINER_WRITES=1. What this API does is find the chip, identify
 * it, report its state and prove the link works, so that the design in
 * docs/RESEARCH-SECURE-ELEMENT.md can be built on a part that is known to
 * answer.
 *
 * The I2C driver is NOT installed here. The board already runs the legacy
 * driver on I2C0 for the panel (src/oled.c), and ESP-IDF 5.5 refuses to have
 * both I2C drivers in one image (sdkconfig.s3cam explains). The caller
 * installs it, or lets oled_i2c_init() do so, and hands the port in.
 *
 * Thread-safe: every call takes one mutex. Each call can block for a few
 * milliseconds (identity reads) up to a few hundred (the certificate at
 * 400 kHz), so call it from a task, never from the UI's draw path.
 */

#ifndef LEEK_SE_H
#define LEEK_SE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

#include "optiga-proto.h"
#include "optiga-container.h"

typedef struct {
    uint16_t data_reg_len;          /* link: frame size the chip reported */

    uint8_t uid[OPTIGA_UID_LEN];    /* 0xE0C2, raw */
    optiga_uid_t id;                /* ...split into fields */
    bool infineon;                  /* CIM byte is 0xCD */

    uint8_t lcs_global;             /* 0xE0C0 */
    uint8_t lcs_app;                /* 0xF1C0 */
    uint8_t sec;                    /* 0xE0C5, security event counter */
    uint16_t max_com_buf;           /* 0xE0C6 */
    bool have_lcs_app, have_sec, have_max_com;

    bool have_cert;                 /* 0xE0E0 parsed as an X.509 certificate */
    size_t cert_object_len;         /* the whole data object */
    size_t cert_len;                /* the DER inside it */
    uint8_t cert_sha256[32];        /* SHA-256 of the DER, for display */
} leek_se_info_t;

typedef struct {
    bool uid_stable;                /* two reads of 0xE0C2 agree */
    bool rng_ok;                    /* sanity check on 256 TRNG bytes */
    bool rng_distinct;              /* two 32-byte draws differ */
    optiga_rng_report_t rng;
    bool passed;                    /* all of the above */
} leek_se_selftest_t;

/** Use the legacy I2C driver already installed on `port`. Idempotent. */
esp_err_t leek_se_attach(i2c_port_t port);

/** Is anything ACKing at 0x30? Cheap; says nothing about what it is. */
bool leek_se_probe(void);

/** Reset the link (warm reset, volatile) and open the Trust M application. */
esp_err_t leek_se_open(void);

/** Identity and state, all from data objects readable "Always". Opens the
 *  application first if needed. */
esp_err_t leek_se_info(leek_se_info_t *info);

/** Read one data object or its metadata. Read-only by construction. */
esp_err_t leek_se_read(uint16_t oid, bool metadata,
                       uint8_t *out, size_t cap, size_t *len);

/** Read-only and volatile checks only. The random bytes it draws are judged
 *  and discarded; they never reach the wallet. */
esp_err_t leek_se_selftest(leek_se_selftest_t *st);

/** Text for the last failure: link error and, if the chip refused a
 *  command, its 0xF1C2 code. Static storage, valid until the next call. */
const char *leek_se_last_error(void);

/**
 * The integration hook for an info screen: `info` as short lines of at most
 * LEEK_SE_LINE_LEN-1 characters, the width of the 128 px panel in the 5x7
 * font. Returns the number of lines written. Draws nothing; the UI owns the
 * screen. See the component README.
 */
#define LEEK_SE_LINE_LEN    22
size_t leek_se_summary_lines(const leek_se_info_t *info,
                             char lines[][LEEK_SE_LINE_LEN], size_t max);

#if LEEK_SE_CONTAINER_WRITES
/*
 * The Level 1 SE container (optiga-container.h): one opaque blob in data
 * object 0xF1E0. DEVELOPMENT ONLY, compiled only with
 * LEEK_SE_CONTAINER_WRITES=1, and not called by the wallet. Storage, not
 * security: the object keeps its factory access rules, so anyone on the I2C
 * bus can read or rewrite it. Pass only data that is already encrypted.
 * Nothing here logs the blob, and every buffer it crosses is wiped.
 */

/** Largest blob the container holds: 1500 bytes less a 16-byte header. */
size_t leek_se_container_capacity(void);

/** Read the blob. `*state` says what the object holds; `*len` is non-zero
 *  only when it is OPTIGA_CONTAINER_OK. */
esp_err_t leek_se_container_read(uint8_t *out, size_t cap, size_t *len,
                                 optiga_container_state_t *state);

/** Replace the blob (1..capacity bytes), then read it back. Refused, with
 *  nothing written, if the object holds data that is not a container. */
esp_err_t leek_se_container_write(const uint8_t *data, size_t len);

/** Overwrite the whole object with zeros; it then reads as EMPTY. */
esp_err_t leek_se_container_erase(void);
#endif

#endif /* LEEK_SE_H */
