/* LeekWallet Zcash: Rust signer backend, C ABI. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes. */
#define ZS_OK          0
#define ZS_ERR_ARG    (-1)
#define ZS_ERR_DERIVE (-2)
#define ZS_ERR_ENCODE (-3)

#define ZS_NET_MAIN 0
#define ZS_NET_TEST 1

#define ZS_UFVK_MAX 512
#define ZS_UA_MAX   256

/* One account as the device shows and exports it: Orchard + P2PKH (ZIP 316). */
typedef struct {
    uint8_t seed_fingerprint[32];   /* ZIP 32 seed fingerprint */
    uint8_t orchard_fvk[96];        /* ak || nk || rivk */
    uint8_t orchard_address[43];    /* raw receiver, diversifier index 0 */
    char    ufvk[ZS_UFVK_MAX];      /* "uview1..." / "uviewtest1...", NUL-terminated */
    char    ua[ZS_UA_MAX];          /* "u1..." / "utest1...", NUL-terminated */
} zs_account;

/* Derive account `account` from a 64-byte BIP39 seed. `net` is ZS_NET_*.
 * t_account: 65 bytes (chain code || compressed pubkey of m/44'/coin'/account'),
 * t_receiver: 20-byte HASH160 of the key at .../0/0. Either may be NULL to
 * leave the transparent item out of the UFVK or UA. */
int32_t zs_account_derive(const uint8_t seed[64], uint32_t account, uint8_t net,
                          const uint8_t *t_account, const uint8_t *t_receiver,
                          zs_account *out);

/* ---- PCZT signer (ZIP 374) ------------------------------------------- */

/* Refusals: each names exactly why the device will not sign. */
#define ZS_E_PARSE              (-10)
#define ZS_E_UNSUPPORTED_BRANCH (-11)
#define ZS_E_SPEND_NOT_OURS     (-12)
#define ZS_E_OUTPUT_COMMITMENT  (-13)
#define ZS_E_USER_ADDRESS       (-14)
#define ZS_E_VALUE_BALANCE      (-15)
#define ZS_E_UNSUPPORTED_BUNDLE (-16)
#define ZS_E_NETWORK            (-17)
#define ZS_E_TOO_MANY           (-18)
#define ZS_E_MISSING_DATA       (-19)
#define ZS_E_DUMMY_KEY          (-20)
#define ZS_E_NOT_CHECKED        (-21)
#define ZS_E_SIGN               (-22)
#define ZS_E_INTERNAL           (-23)

#define ZS_MAX_OUTPUTS 16
#define ZS_MAX_SIGS    32

enum { ZS_POOL_TRANSPARENT = 0, ZS_POOL_ORCHARD = 1, ZS_POOL_IRONWOOD = 2 };
enum { ZS_KIND_EXTERNAL = 0, ZS_KIND_CHANGE = 1, ZS_KIND_SELF = 2 };
enum { ZS_MEMO_NONE = 0, ZS_MEMO_TEXT = 1, ZS_MEMO_BINARY = 2, ZS_MEMO_UNRECOVERED = 3 };
#define ZS_WARN_TRANSPARENT_OUT  (1u << 0)
#define ZS_WARN_NO_USER_ADDRESS  (1u << 1)

typedef struct {
    uint8_t  pool, kind, memo_kind, _pad;
    uint16_t memo_len, _pad2;
    uint64_t value;                 /* zatoshis */
    char     address[ZS_UA_MAX];    /* empty for change */
    uint8_t  memo[512];
} zs_output;

/* Everything the review screens show. Every field is computed or verified
 * on the device; none is taken from the host as-is. */
typedef struct {
    uint32_t branch_id, tx_version, warnings;
    uint8_t  n_outputs, n_spends, n_dummies, _pad;
    uint64_t total_in, fee;
    uint8_t  sighash[32];
    zs_output outputs[ZS_MAX_OUTPUTS];
} zs_summary;

typedef struct {
    uint8_t  pool, _pad[3];
    uint32_t index;
    uint8_t  sig[64];
} zs_sig;

typedef struct zs_ctx zs_ctx;

zs_ctx *zs_pczt_new(const uint8_t *bytes, size_t len, int32_t *err);
void    zs_pczt_free(zs_ctx *ctx);
/* All checks; fills the summary. Required before zs_pczt_sign. */
int32_t zs_pczt_check(zs_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                      zs_summary *out);
/* Sign every spend of ours, after the user accepted the summary. */
int32_t zs_pczt_sign(zs_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                     zs_sig *sigs, uint32_t max, uint32_t *n_out);
/* Verify one signature against the action's rk and the sighash (as a host does). */
int32_t zs_pczt_verify_sig(const zs_ctx *ctx, const zs_sig *sig);

/* Phase 0.4 spike entry points. All return 0 on success. */
int32_t zs_spike_fvk(const uint8_t seed[64], uint32_t account, uint8_t out_fvk[96]);
int32_t zs_spike_address(const uint8_t seed[64], uint32_t account, uint8_t out_addr[43]);
int32_t zs_spike_sign(const uint8_t seed[64], uint32_t account, const uint8_t alpha[32],
                      const uint8_t sighash[32], uint8_t out_sig[64]);
int32_t zs_spike_cmx(const uint8_t seed[64], uint64_t value, uint8_t out_cmx[32]);
int32_t zs_spike_verify(const uint8_t seed[64], uint32_t account, const uint8_t alpha[32],
                        const uint8_t sighash[32], const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
