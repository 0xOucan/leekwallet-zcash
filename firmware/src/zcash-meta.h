/**
 * Per-account metadata (roadmap 1.4): birthday height, flags, label.
 *
 * None of it is secret, and none of it is needed to sign; it saves a
 * companion asking the user for the birthday again, and a rescan from the
 * wrong height. Records are keyed by the open seed's short ZIP 32
 * fingerprint and the ZIP 32 account, so they follow the seed, not a vault
 * slot number.
 *
 * Where a record lives depends on the seed, on purpose:
 *   - a stored wallet with no BIP39 passphrase: NVS, so it survives reboots;
 *   - a temporary or SD seed, or any seed with a passphrase: RAM only, gone
 *     on lock. Writing such a seed's fingerprint to flash would leave a
 *     record that the seed (or the hidden wallet) was ever used here, which
 *     is what those modes exist to avoid.
 * Each NVS record is a single blob write, so a power cut leaves the old
 * record or the new one, never a mix.
 */
#ifndef LEEK_ZCASH_META_H
#define LEEK_ZCASH_META_H

#include <stdbool.h>
#include <stdint.h>

#define ZMETA_LABEL_MAX 32
#define ZMETA_FLAG_USE_QSK 0x01

typedef struct {
    uint32_t birthday;              /* 0: unknown */
    uint8_t  flags;                 /* ZMETA_FLAG_* */
    char     label[ZMETA_LABEL_MAX + 1];
} ZcashMeta;

typedef enum {
    ZMETA_OK_STORED = 0,   /* in flash */
    ZMETA_OK_SESSION,      /* in RAM until lock */
    ZMETA_E_NO_SEED,       /* nothing open to attach it to */
    ZMETA_E_ARG,
    ZMETA_E_NVS,
} ZcashMetaResult;

/** The record for `account` of the open seed. False if there is none. */
bool zmeta_get(uint32_t account, ZcashMeta *out);

/** Store or replace the record for `account` of the open seed. */
ZcashMetaResult zmeta_set(uint32_t account, const ZcashMeta *m);

/** Drop every session record. Called on lock. */
void zmeta_forget_session(void);

#ifdef LEEK_HOST_TEST
void zmeta__reset_for_test(void);
#endif

#endif /* LEEK_ZCASH_META_H */
