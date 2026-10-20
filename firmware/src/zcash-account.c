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
#include "zsign_c.h"

#if !defined(LEEK_HOST_TEST)
#include "esp_heap_caps.h"
#include "esp_log.h"
int zc_crypto_init(void);   /* zsign-c, private header: 0 ok, -1 table corrupt */
static const char *TAG = "zaccount";
#define DERIVE_FAIL(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define DERIVE_FAIL(...) ((void)0)
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
        DERIVE_FAIL("derive failed: transparent account key");
        goto done;
    }
    memcpy(out->t_account, node.chain_code, 32);
    memcpy(out->t_account + 32, node.public_key, 33);

    if (hdnode_private_ckd(&node, 0) != 1 || hdnode_private_ckd(&node, 0) != 1 ||
        hdnode_fill_public_key(&node) != 0) {
        DERIVE_FAIL("derive failed: transparent receiver key");
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
    const int32_t rc = zc_account_derive(seed, c->account, c->network, out->t_account,
                                         out->t_receiver, &out->keys);
    if (rc == ZS_OK) {
        err = WALLET_OK;
    } else {
        /* Which step, and how much internal RAM was left: a failure here
         * used to reach the user only as "derivation failed". */
        DERIVE_FAIL("derive failed: zc_account_derive rc=%d, crypto init %d, "
                    "internal heap %u B (largest %u B)", (int)rc, zc_crypto_init(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
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
    const bool ran = zcash_worker_run(derive_job, &job);
    WalletError err = ran ? job.err : WALLET_ERROR_DERIVATION_FAILED;
    if (err != WALLET_OK) {
        DERIVE_FAIL("account %u net %u: %s, wallet error %d", (unsigned)account,
                    (unsigned)network, ran ? "derived" : "the Zcash worker did not run",
                    (int)err);
        memset(out, 0, sizeof *out);
    }
    return err;
}

/* The account's P2PKH key at m/44'/coin'/account'/0/index, from the account
 * key the derivation kept (chain code || compressed pubkey). */
static bool t_receiver_at(const ZcashAccount *a, uint32_t index, uint8_t hash160[20],
                          char *addr, size_t addr_cap)
{
    HDNode node;
    bool ok = hdnode_from_xpub(3, 0x80000000u | a->account, a->t_account, a->t_account + 32,
                               SECP256K1_NAME, &node) == 1 &&
              hdnode_public_ckd(&node, 0) == 1 && hdnode_public_ckd(&node, index) == 1;
    if (ok) {
        ecdsa_get_pubkeyhash(node.public_key, HASHER_SHA2_RIPEMD, hash160);
        hdnode_get_address(&node, a->network == ZCASH_NET_MAIN ? ZCASH_T1_PREFIX : ZCASH_TM_PREFIX,
                           addr, addr_cap);
    }
    memzero(&node, sizeof node);
    return ok;
}

typedef struct {
    const ZcashAccount *a;
    const char         *ua;
    uint8_t            *index_le;
    uint8_t             p2pkh[20];
    int                 has_p2pkh;
    int32_t             rc;
} UaCheckJob;

static void ua_check_job(void *p)
{
    UaCheckJob *job = p;
    job->rc = zc_ua_owned(job->ua, job->a->network, job->a->keys.orchard_fvk, job->index_le,
                          job->p2pkh, &job->has_p2pkh);
}

ZcashUaCheck zcash_ua_check(const ZcashAccount *a, const char *ua, uint8_t index_le[11],
                            char t_address_out[ZCASH_TADDR_MAX])
{
    t_address_out[0] = 0;
    if (!a || !ua) return ZCASH_UA_MALFORMED;
    UaCheckJob job = { a, ua, index_le, {0}, 0, ZS_E_INTERNAL };
    if (!zcash_worker_run(ua_check_job, &job)) return ZCASH_UA_MALFORMED;
    switch (job.rc) {
    case ZS_OK: break;
    case ZS_E_USER_ADDRESS: return ZCASH_UA_NOT_OURS;
    case ZS_E_UNSUPPORTED_BUNDLE: return ZCASH_UA_UNSUPPORTED;
    default: return ZCASH_UA_MALFORMED;
    }
    if (!job.has_p2pkh) return ZCASH_UA_OURS;

    /* ZIP 316: a P2PKH receiver at diversifier index j is the external
     * transparent key j, so only indices below 2^31 carry one. */
    for (int i = 4; i < 11; i++) {
        if (index_le[i]) return ZCASH_UA_NOT_OURS;
    }
    uint32_t j = (uint32_t)index_le[0] | (uint32_t)index_le[1] << 8 |
                 (uint32_t)index_le[2] << 16 | (uint32_t)index_le[3] << 24;
    if (j >= 0x80000000u) return ZCASH_UA_NOT_OURS;
    uint8_t mine[20];
    char addr[ZCASH_TADDR_MAX];
    if (!t_receiver_at(a, j, mine, addr, sizeof addr)) return ZCASH_UA_MALFORMED;
    if (memcmp(mine, job.p2pkh, 20) != 0) return ZCASH_UA_NOT_OURS;
    memcpy(t_address_out, addr, sizeof addr);
    return ZCASH_UA_OURS;
}
