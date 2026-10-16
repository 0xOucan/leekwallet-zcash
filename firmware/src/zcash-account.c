/**
 * Zcash account derivation. See zcash-account.h.
 */
#include "zcash-account.h"

#include <string.h>

#include "bip32.h"
#include "curves.h"
#include "ecdsa.h"
#include "memzero.h"
#include "zcash-worker.h"
#ifndef LEEK_HOST_TEST
#include "zsign_c.h"
#endif


#define H(i) (0x80000000u | (i))

/* Base58Check version prefixes for P2PKH: "t1" mainnet, "tm" testnet. */
#define ZCASH_T1_PREFIX 0x1CB8
#define ZCASH_TM_PREFIX 0x1D25

uint32_t zcash_coin_type(uint8_t network)
{
    return network == ZCASH_NET_MAIN ? 133 : 1;
}

typedef struct {
    uint32_t      account;
    uint8_t       network;
    ZcashAccount *out;
} DeriveCtx;

static WalletError derive_with_seed(const uint8_t seed[64], void *p)
{
    DeriveCtx *c = p;
    ZcashAccount *out = c->out;
    WalletError err = WALLET_ERROR_DERIVATION_FAILED;

    /* Transparent half: m/44'/coin'/account', then the first external key. */
    HDNode node;
    if (hdnode_from_seed(seed, 64, SECP256K1_NAME, &node) != 1 ||
        hdnode_private_ckd(&node, H(44)) != 1 ||
        hdnode_private_ckd(&node, H(zcash_coin_type(c->network))) != 1 ||
        hdnode_private_ckd(&node, H(c->account)) != 1 ||
        hdnode_fill_public_key(&node) != 0) {
        goto done;
    }
    memcpy(out->t_account, node.chain_code, 32);
    memcpy(out->t_account + 32, node.public_key, 33);

    if (hdnode_private_ckd(&node, 0) != 1 || hdnode_private_ckd(&node, 0) != 1 ||
        hdnode_fill_public_key(&node) != 0) {
        goto done;
    }
    ecdsa_get_pubkeyhash(node.public_key, HASHER_SHA2_RIPEMD, out->t_receiver);
    hdnode_get_address(&node, c->network == ZCASH_NET_MAIN ? ZCASH_T1_PREFIX : ZCASH_TM_PREFIX,
                       out->t_address, sizeof out->t_address);

    /* Orchard half, and the UFVK/UA joining both. */
/* C on the device, Rust on the host: see the backend note in
     * zcash-sign.c (0.75 s against 97 s per account on the S3). Both match
     * every librustzcash key vector (test_zcash_keys). */
#if defined(LEEK_HOST_TEST)
    if (zs_account_derive(seed, c->account, c->network, out->t_account, out->t_receiver,
                          &out->keys) == ZS_OK) {
        err = WALLET_OK;
    }
#else
    if (zc_account_derive(seed, c->account, c->network, out->t_account, out->t_receiver,
                          &out->keys) == ZS_OK) {
        err = WALLET_OK;
    }
#endif

done:
    memzero(&node, sizeof node);
    return err;
}

/* Runs on the Zcash worker: Sinsemilla and the Rust stack want its 64 KB. */
typedef struct {
    DeriveCtx   c;
    WalletError err;
} DeriveJob;

static void derive_job(void *p)
{
    DeriveJob *job = p;
    job->err = wallet_with_seed(derive_with_seed, &job->c);
}

WalletError zcash_account_derive(uint32_t account, uint8_t network, ZcashAccount *out)
{
    if (!out || account >= 0x80000000u ||
        (network != ZCASH_NET_MAIN && network != ZCASH_NET_TEST)) {
        return WALLET_ERROR_DERIVATION_FAILED;
    }
    memset(out, 0, sizeof *out);
    out->account = account;
    out->network = network;

    DeriveJob job = { { account, network, out }, WALLET_ERROR_DERIVATION_FAILED };
    WalletError err = zcash_worker_run(derive_job, &job) ? job.err : WALLET_ERROR_DERIVATION_FAILED;
    if (err != WALLET_OK) {
        memset(out, 0, sizeof *out);
    }
    return err;
}
