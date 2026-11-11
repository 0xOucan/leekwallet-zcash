/**
 * The ZIP 32 seed fingerprint of the open seed, and its short form.
 *
 * The short form, "ZFP 1A2B3C4D" on screen, is the first four bytes in
 * upper-case hex: what the home and wallet screens show after unlock
 * (roadmap 1.5), what zecz prints on import, and the key per-account
 * metadata is filed under (zcash-meta.c). Kept apart from the screens so
 * every user of it links nothing else.
 */
#ifndef LEEK_ZCASH_FP_H
#define LEEK_ZCASH_FP_H

#include <stdint.h>

#include "leek-wallet.h"

#define ZCASH_FP_CHARS 8

/** "1A2B3C4D": the short form of a 32-byte seed fingerprint. */
void zcash_short_fp(const uint8_t fp[32], char out[ZCASH_FP_CHARS + 1]);

/**
 * The ZIP 32 seed fingerprint of the unlocked wallet's seed (passphrase
 * applied): BLAKE2b-256, personalisation "Zcash_HD_Seed_FP", over
 * len(seed) || seed. Cheap once the seed is cached; runs on the caller's
 * stack. On failure `out` is zeroed.
 */
WalletError zcash_seed_fingerprint(uint8_t out[32]);

/**
 * The same fingerprint for a phrase that is not the open seed, with no BIP39
 * passphrase: what identifies a backup copy (se-backup.h). One BIP39 stretch
 * (2048 rounds of PBKDF2-HMAC-SHA512, about half a second on the S3) on the
 * caller's stack; the seed is wiped before it returns.
 */
void zcash_fp_of_phrase(const char *phrase, uint8_t out[32]);

#endif /* LEEK_ZCASH_FP_H */
