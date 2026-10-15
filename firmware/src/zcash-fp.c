/**
 * The seed fingerprint. See zcash-fp.h.
 */
#include "zcash-fp.h"

#include <stdio.h>
#include <string.h>

#include "blake2b.h"
#include "memzero.h"

void zcash_short_fp(const uint8_t fp[32], char out[ZCASH_FP_CHARS + 1])
{
    snprintf(out, ZCASH_FP_CHARS + 1, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
}

static WalletError fingerprint_with_seed(const uint8_t seed[64], void *ctx)
{
    blake2b_state s;
    const uint8_t len = 64;
    blake2b_InitPersonal(&s, 32, "Zcash_HD_Seed_FP", 16);
    blake2b_Update(&s, &len, 1);
    blake2b_Update(&s, seed, 64);
    blake2b_Final(&s, ctx, 32);
    memzero(&s, sizeof s);
    return WALLET_OK;
}

WalletError zcash_seed_fingerprint(uint8_t out[32])
{
    WalletError err = wallet_with_seed(fingerprint_with_seed, out);
    if (err != WALLET_OK) {
        memset(out, 0, 32);
    }
    return err;
}
