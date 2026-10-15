/* LeekWallet Zcash: types and codes shared by both signer backends.
 *
 * The Rust backend (zsign-rust, zs_*) and the C backend (zsign-c, zc_*) fill
 * exactly these structs and return exactly these codes, so the firmware can
 * run both on the same PCZT and compare the results field by field. */
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

#ifdef __cplusplus
}
#endif
