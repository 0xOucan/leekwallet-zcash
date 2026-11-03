/**
 * App Storage Keys. See zcash-storage.h.
 */
#include "zcash-storage.h"

#include <string.h>

#include "memzero.h"

bool zcash_app_id_valid(const char *app_id, size_t len)
{
    return zc_app_id_valid(app_id, len) == 1;
}

typedef struct {
    const char *app_id;
    size_t      len;
    uint32_t    account;
    uint8_t     network;
    uint8_t    *out;
} StorageKeyCtx;

static WalletError storage_key_with_seed(const uint8_t seed[64], void *p)
{
    const StorageKeyCtx *c = p;
    return zc_storage_key(seed, 64, c->network, c->account, c->app_id, c->len, c->out) == ZS_OK
               ? WALLET_OK
               : WALLET_ERROR_DERIVATION_FAILED;
}

WalletError zcash_storage_key(const char *app_id, size_t len, uint32_t account,
                              uint8_t network, uint8_t out[32])
{
    StorageKeyCtx c = { app_id, len, account, network, out };
    WalletError err = wallet_with_seed(storage_key_with_seed, &c);
    if (err != WALLET_OK) {
        memzero(out, 32);
    }
    return err;
}
