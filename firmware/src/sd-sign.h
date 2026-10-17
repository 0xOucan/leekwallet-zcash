/**
 * SD card airgap signing (roadmap 5).
 *
 * The companion writes LEEKZ/unsigned.pczt; the device checks it exactly as
 * it checks a USB upload (zcash-sign.c, both backends where both run), the
 * user reviews the same pages, and on ACCEPT the device signs and writes
 * LEEKZ/signed.zsig (FORMATS.md). No USB data is involved.
 *
 * What is signed is the copy in RAM that was checked and reviewed. Before
 * the signatures are written the file on the card is hashed again, and if
 * it no longer matches what was reviewed nothing is written: a card that
 * changed under the review is a card someone else had (roadmap 5.2).
 */
#ifndef LEEK_SD_SIGN_H
#define LEEK_SD_SIGN_H

#include <stdbool.h>
#include <stdint.h>

#include "zsign_rust.h"

#define SDSIGN_DIR  "LEEKZ"
#define SDSIGN_IN   "LEEKZ/unsigned.pczt"
#define SDSIGN_OUT  "LEEKZ/signed.zsig"

/* Errors of this layer; the signers' ZS_E_* and ZSIGN_E_* pass through. */
#define SDSIGN_E_NOCARD   (-50)
#define SDSIGN_E_NOFILE   (-51)   /* no LEEKZ/unsigned.pczt */
#define SDSIGN_E_IO       (-52)
#define SDSIGN_E_CHANGED  (-53)   /* the file changed between review and signing */
#define SDSIGN_E_STATE    (-54)

/** True if a card is in and holds LEEKZ/unsigned.pczt. */
bool sdsign_available(void);

/**
 * Read and check LEEKZ/unsigned.pczt for `account` on `network`. On success
 * `*summary` is what the review pages show, valid until finish or cancel.
 */
int sdsign_load(uint32_t account, uint8_t network, const zs_summary **summary);

/**
 * After the user approved: confirm the file is unchanged, sign, and write
 * LEEKZ/signed.zsig. `*n_sigs` is how many signatures it holds.
 */
int sdsign_finish(uint32_t *n_sigs);

/** Drop a loaded file without signing. */
void sdsign_cancel(void);

/** Short reason for any code above, a ZSIGN_E_* or a ZS_E_*. */
const char *sdsign_strerror(int code);

/* The largest .zsig: every entry a transparent one (DER of 73 bytes with the
 * sighash type, and the 33-byte pubkey). */
#define SDSIGN_ZSIG_MAX (4 + 1 + 32 + 2 + ZS_MAX_SIGS * (1 + 4 + 1 + 73 + 33))

/** Serialize a .zsig (FORMATS.md): version 1 if every signature is shielded,
 * version 2 otherwise. Returns the length, 0 if `cap` is short. */
size_t sdsign_zsig_encode(const uint8_t sha256[32], const zs_sig *sigs, uint32_t n,
                          uint8_t *out, size_t cap);

#endif /* LEEK_SD_SIGN_H */
