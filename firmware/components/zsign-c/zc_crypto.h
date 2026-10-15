/* LeekWallet Zcash, C signer: Orchard primitives on zc_pallas.c, with
 * BLAKE2b, ChaCha20-Poly1305 and the ZIP 32 / FF1 helpers taken from the
 * vendored libzcash-ironwood-c: canonical decoding with pasta_curves
 * semantics, value and note commitments (ZIP 212 and ZIP 2005), Poseidon and
 * nullifiers, viewing keys, note encryption and OVK recovery, RedPallas.
 *
 * Internal to zsign-c; compiled with vendor/zc_vendor_prefix.h force-included.
 * Single-threaded: a few large temporaries are static.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "zc_pallas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup: checks the BLAKE2b fingerprint of the embedded Sinsemilla
 * S table and caches the generators. 0 on success; anything else means
 * nothing may be signed. Idempotent. */
int zc_crypto_init(void);
/* Known answers for what the vendored library lacks (Poseidon) and a
 * recomputation of S table entries with our own hash-to-curve. 0 if good. */
int zc_crypto_selftest(void);

/* Byte strings as field elements: 1 if the LE value is < the modulus. */
int zc_fp_canonical(const uint8_t b[32]);         /* Pallas base field p */
int zc_fq_canonical(const uint8_t b[32]);         /* Pallas scalar field q */
int zc_bls_fr_canonical(const uint8_t b[32]);     /* BLS12-381 r (Jubjub base) */
int zc_jubjub_fr_canonical(const uint8_t b[32]);  /* Jubjub scalar field */

/* pasta_curves Point::from_bytes succeeds (identity allowed / not). */
int zc_point_valid(const uint8_t b[32]);
int zc_point_valid_nonid(const uint8_t b[32]);

/* PRF^expand_key(t1 || t2 || t3) = BLAKE2b-512("Zcash_ExpandSeed", key || t). */
void zc_prf_expand(const uint8_t key[32], const uint8_t *t1, size_t n1,
                   const uint8_t *t2, size_t n2, const uint8_t *t3, size_t n3,
                   uint8_t out[64]);
void zc_to_scalar(const uint8_t wide[64], uint8_t out[32]);   /* mod q */
void zc_to_base(const uint8_t wide[64], uint8_t out[32]);     /* mod p */

/* DiversifyHash^Orchard(d), with the spec's substitute for the identity. */
void zc_diversify_hash(zc_pt *gd, const uint8_t d[11]);
/* Address::same_expanded_receiver: equal g_d and equal pk_d. */
int zc_same_receiver(const uint8_t a[43], const uint8_t b[43]);

/* cv_net = ValueCommit^Orchard_rcv(v_spend - v_output); rcv must be < q. */
void zc_value_commit(uint8_t out[32], uint64_t v_spend, uint64_t v_output,
                     const uint8_t rcv[32]);

/* Notes (ZIP 212; ZIP 2005 for version 3). version: 2 Orchard, 3 Ironwood.
 * Return 0, or -1 if pk_d is not a valid non-identity point. */
int zc_note_commit(zc_pt *cm, const uint8_t recipient[43], uint64_t value,
                   const uint8_t rho[32], const uint8_t rseed[32], uint8_t version);
int zc_note_cmx(uint8_t cmx[32], const uint8_t recipient[43], uint64_t value,
                const uint8_t rho[32], const uint8_t rseed[32], uint8_t version);
void zc_note_psi(uint8_t psi[32], const uint8_t rseed[32], const uint8_t rho[32]);
void zc_note_esk(uint8_t esk[32], const uint8_t rseed[32], const uint8_t rho[32]);
/* RandomSeed::from_bytes(rseed, rho): 1 if the derived esk is non-zero. */
int zc_rseed_valid(const uint8_t rseed[32], const uint8_t rho[32]);

/* Poseidon P128Pow5T3, ConstantLength<2>, over Fp (inputs canonical). */
void zc_poseidon2(uint8_t out[32], const uint8_t a[32], const uint8_t b[32]);
/* nf = Extract_P([PRF^nf_nk(rho) + psi] K + cm). */
void zc_nullifier(uint8_t nf[32], const uint8_t nk[32], const uint8_t rho[32],
                  const uint8_t psi[32], const zc_pt *cm);

/* Full viewing key material. All of it is secret; callers wipe it. */
typedef struct {
    uint8_t ak[32], nk[32], rivk[32];         /* the 96-byte FVK */
    uint8_t ivk_ext[32], ivk_int[32];
    uint8_t ovk_ext[32], ovk_int[32];
    uint8_t dk_ext[32];
} zc_fvk;

/* FullViewingKey::from_bytes: ak a non-identity point with sign bit 0,
 * nk < p, rivk < q, and both incoming viewing keys non-zero. 0 if valid. */
int zc_fvk_from_bytes(zc_fvk *f, const uint8_t fvk[96]);
/* ZIP 32 Orchard account m/32'/coin'/account': ask and the FVK. 0 on success. */
int zc_account_keys(const uint8_t seed[64], uint32_t coin_type, uint32_t account,
                    uint8_t ask[32], zc_fvk *f);
/* 0 = not ours, 1 = external scope, 2 = internal scope. */
int zc_fvk_scope(const zc_fvk *f, const uint8_t recipient[43]);
/* The external address at diversifier index 0. */
void zc_fvk_default_address(const zc_fvk *f, uint8_t out[43]);

/* rk = ak + [alpha] G. 0 on success, -1 if ak is invalid or alpha >= q. */
int zc_randomize_ak(uint8_t rk[32], const uint8_t ak[32], const uint8_t alpha[32]);
/* RedPallas SpendAuth signature under rsk = ask + alpha, with a fresh 80-byte
 * random T (reddsa). Writes rk = [rsk] G. 0 on success. */
int zc_redpallas_sign(const uint8_t ask[32], const uint8_t alpha[32], const uint8_t msg[32],
                      uint8_t sig[64], uint8_t rk[32]);
/* 0 if sig verifies against rk (reddsa verify). */
int zc_redpallas_verify(const uint8_t rk[32], const uint8_t msg[32], const uint8_t sig[64]);

/* Note encryption (ZIP 212 / ZIP 2005): enc_ciphertext and epk for a note
 * and memo, as OrchardNoteEncryption::new(None, note, memo). 0 on success. */
int zc_note_encrypt(uint8_t enc[580], uint8_t epk[32], const uint8_t recipient[43],
                    uint64_t value, const uint8_t rseed[32], const uint8_t rho[32],
                    const uint8_t memo[512], uint8_t version);

/* try_output_recovery_with_ovk for one action. Returns 0 and fills
 * recipient/value/memo on success. */
int zc_recover_with_ovk(const uint8_t ovk[32], const uint8_t cv_net[32], const uint8_t cmx[32],
                        const uint8_t epk[32], const uint8_t enc[580], const uint8_t out[80],
                        const uint8_t rho[32], uint8_t version,
                        uint8_t recipient[43], uint64_t *value, uint8_t memo[512]);

/* ZIP 32 seed fingerprint. */
void zc_seed_fingerprint(const uint8_t seed[64], uint8_t out[32]);

/* Randomness for signing (platform/zc_platform_*.c). */
void zc_platform_random(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
