/* LeekWallet Zcash: bounds-checked, allocation-free PCZT reader (ZIP 374).
 *
 * Reads "PCZT" || u32 LE version || postcard body, for encoding versions 1
 * and 2 of librustzcash's `pczt` crate (0.10.0-pre.2). The field order and
 * types follow the serde structs in pczt/src/{lib,common,transparent,sapling,
 * orchard}.rs; see pczt_reader.c for the per-struct layout.
 *
 * Zero-copy: every byte field is a pointer into the caller's buffer, which
 * must outlive the parsed struct and must not change while it is used. The
 * caller provides the zc_pczt (about 9 KB on a 32-bit target); nothing is
 * allocated.
 *
 * This layer checks only the wire format: lengths against remaining input,
 * Option and bool tags, enum variants, UTF-8 of strings, varint ranges, caps
 * on counts, and no trailing bytes. Semantic validity (curve points, field
 * elements, commitments) is checked by zsign_c.c.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caps on what the reader stores. Everything it only skips (inputs, Sapling,
 * maps, derivation paths) is bounded by the input length alone. A PCZT over
 * a cap is refused with ZS_E_TOO_MANY; the Rust side has no cap, so this is
 * a known difference, in the refusing direction only. */
#define ZC_MAX_ACTIONS     64   /* Orchard + Ironwood together */
#define ZC_MAX_T_OUTPUTS   32

/* Reader result codes. */
#define ZC_R_OK        0
#define ZC_R_PARSE   (-1)   /* malformed: maps to ZS_E_PARSE */
#define ZC_R_TOO_MANY (-2)  /* a count above our caps: maps to ZS_E_TOO_MANY */

/* A variable-length byte string; p == NULL means "absent" for Option fields. */
typedef struct {
    const uint8_t *p;
    uint32_t len;
} zc_bytes;

typedef struct {
    uint32_t tx_version, version_group_id, consensus_branch_id;
    uint32_t fallback_lock_time, expiry_height, coin_type;
    uint8_t  has_fallback_lock_time, tx_modifiable;
} zc_global;

typedef struct {
    uint64_t value;
    zc_bytes script_pubkey;
    zc_bytes redeem_script;     /* optional */
    zc_bytes user_address;      /* optional, valid UTF-8 */
} zc_tout;

/* EncCiphertext (v2): either the 580-byte ciphertext or the memo plaintext
 * with trailing zeros stripped (<= 512 bytes, last byte non-zero). v1 always
 * carries the ciphertext. */
enum { ZC_ENC_CIPHERTEXT = 0, ZC_ENC_MEMO = 1 };

typedef struct {
    /* Action */
    const uint8_t *cv_net;          /* 32, optional in v2 */
    const uint8_t *rcv;             /* 32, optional */
    /* Spend */
    const uint8_t *nullifier;       /* 32 */
    const uint8_t *rk;              /* 32 */
    const uint8_t *spend_auth_sig;  /* 64, optional */
    const uint8_t *s_recipient;     /* 43, optional */
    const uint8_t *rho;             /* 32, optional */
    const uint8_t *s_rseed;         /* 32, optional */
    const uint8_t *fvk;             /* 96, optional */
    const uint8_t *witness;         /* 32 x 32 auth path, optional */
    const uint8_t *alpha;           /* 32, optional */
    const uint8_t *dummy_sk;        /* 32, optional */
    uint64_t s_value;
    uint32_t witness_pos;
    uint8_t  has_s_value;
    uint8_t  s_zip32_present, s_zip32_hardened;  /* every index >= 2^31 */
    /* Output */
    uint8_t  enc_kind;              /* ZC_ENC_* */
    uint8_t  has_o_value;
    uint8_t  o_zip32_present, o_zip32_hardened;
    const uint8_t *cmx;             /* 32, optional in v2 */
    const uint8_t *epk;             /* 32 */
    zc_bytes enc;                   /* ciphertext or stripped memo */
    zc_bytes out_ct;
    const uint8_t *o_recipient;     /* 43, optional */
    const uint8_t *o_rseed;         /* 32, optional */
    const uint8_t *ock;             /* 32, optional */
    zc_bytes user_address;          /* optional, valid UTF-8 */
    uint64_t o_value;
} zc_action;

/* An Orchard-protocol bundle (Orchard or Ironwood pool). */
typedef struct {
    uint8_t  flags;
    uint8_t  value_sum_neg;         /* (magnitude, is_negative) */
    uint8_t  note_version;          /* 2 (V2) or 3 (V3) */
    uint8_t  has_zkproof;
    uint64_t value_sum_mag;
    const uint8_t *anchor;          /* 32, optional (None in v1 = all-zero + empty) */
    const uint8_t *bsk;             /* 32, optional */
    uint16_t first, n;              /* actions[first .. first+n) */
} zc_obundle;

typedef struct {
    uint8_t  present;               /* non-empty or explicitly encoded */
    uint16_t n_spends, n_outputs;
    const uint8_t *anchor;          /* 32, optional */
    const uint8_t *bsk;             /* 32, optional */
    uint8_t  value_sum_fits_i64;
    int64_t  value_sum;             /* valid when value_sum_fits_i64 */
} zc_sapling;

typedef struct {
    uint32_t   version;             /* PCZT encoding version, 1 or 2 */
    zc_global  g;
    uint16_t   n_tin, n_tout;
    uint8_t    tin_lock_time_set;   /* any input carries a required lock time */
    zc_tout    tout[ZC_MAX_T_OUTPUTS];
    zc_sapling sapling;
    zc_obundle orchard, ironwood;
    uint16_t   n_actions;
    zc_action  actions[ZC_MAX_ACTIONS];
} zc_pczt;

/* Parse `len` bytes at `buf` into `out`. Returns ZC_R_OK, ZC_R_PARSE or
 * ZC_R_TOO_MANY. On failure `out` is left in an unspecified state. */
int zc_pczt_read(const uint8_t *buf, size_t len, zc_pczt *out);

/* Strict UTF-8 validation (as Rust's str::from_utf8). 1 if valid. */
int zc_utf8_valid(const uint8_t *s, size_t n);

#ifdef __cplusplus
}
#endif
