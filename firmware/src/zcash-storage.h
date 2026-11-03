/**
 * App Storage Keys (docs/zcash/zip-draft-app-storage-keys.md).
 *
 * The key an app encrypts its stored wallet data under, one per app, network
 * and account, derived from the seed with ZIP 32 Registered key derivation.
 * The derivation itself is the C signer's (zc_storage_key in zsign-c); this
 * is the open seed's side of it, kept apart from the screens and the
 * protocol so each links nothing else.
 */
#ifndef LEEK_ZCASH_STORAGE_H
#define LEEK_ZCASH_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "leek-wallet.h"
#include "zsign_c.h"

#define ZCASH_APP_ID_MAX ZC_APP_ID_MAX

/** True if app_id[0..len) is a valid App identifier, exactly as given. */
bool zcash_app_id_valid(const char *app_id, size_t len);

/**
 * The Storage Key of the unlocked wallet's seed (passphrase applied) for
 * `app_id` on `network`, `account`. Runs on the caller's stack. On failure
 * `out` is zeroed; the caller wipes it after use either way.
 */
WalletError zcash_storage_key(const char *app_id, size_t len, uint32_t account,
                              uint8_t network, uint8_t out[32]);

#endif /* LEEK_ZCASH_STORAGE_H */
