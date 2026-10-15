/* LeekWallet Zcash, C signer: Pallas field and curve arithmetic.
 *
 * Our own implementation, not the vendored library's: Montgomery
 * multiplication over 8 x 32-bit limbs (R = 2^256) for both Pallas fields,
 * and the Renes-Costello-Batina complete formulas for y^2 = x^3 + 5 in
 * homogeneous projective coordinates. Every operation on a field element or
 * point runs in time independent of its value; scalar multiplication uses a
 * fixed 4-bit window with a full-table constant-time lookup. The exceptions
 * are named: zc_hash_to_curve and zc_pt_decode branch on whether a value is
 * a square, and only ever see public data (protocol constants, diversifiers
 * and points taken from the PCZT).
 *
 * The vendored libzcash-ironwood-c has Pallas arithmetic too, but its field
 * multiplication reduces bit by bit, with a data-dependent subtraction per
 * bit: about 100x slower and not constant time. It is kept only as a
 * cross-check in the host tests (zc_xcheck.c).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A field element in Montgomery form, fully reduced (< modulus). */
typedef struct { uint32_t v[8]; } zc_fe;
/* A point in homogeneous projective coordinates; the identity has Z = 0. */
typedef struct { zc_fe x, y, z; } zc_pt;

/* ---- Fp, the base field ---------------------------------------------- */
void zc_fp_set_u64(zc_fe *r, uint64_t v);
int  zc_fp_from_bytes(zc_fe *r, const uint8_t b[32]);   /* 1 iff b < p */
void zc_fp_to_bytes(uint8_t b[32], const zc_fe *a);
void zc_fp_from_wide(zc_fe *r, const uint8_t w[64]);    /* LE 512-bit mod p */
void zc_fp_add(zc_fe *r, const zc_fe *a, const zc_fe *b);
void zc_fp_sub(zc_fe *r, const zc_fe *a, const zc_fe *b);
void zc_fp_neg(zc_fe *r, const zc_fe *a);
void zc_fp_mul(zc_fe *r, const zc_fe *a, const zc_fe *b);
void zc_fp_sqr(zc_fe *r, const zc_fe *a);
void zc_fp_inv(zc_fe *r, const zc_fe *a);               /* 0 -> 0 */
int  zc_fp_sqrt(zc_fe *r, const zc_fe *a);              /* 1 iff a is a square */
int  zc_fp_is_odd(const zc_fe *a);                      /* of the canonical value */

/* ---- Fq, the scalar field -------------------------------------------- */
void zc_fq_set_u64(zc_fe *r, uint64_t v);
int  zc_fq_from_bytes(zc_fe *r, const uint8_t b[32]);   /* 1 iff b < q */
void zc_fq_to_bytes(uint8_t b[32], const zc_fe *a);
void zc_fq_from_wide(zc_fe *r, const uint8_t w[64]);    /* LE 512-bit mod q */
void zc_fq_add(zc_fe *r, const zc_fe *a, const zc_fe *b);
void zc_fq_neg(zc_fe *r, const zc_fe *a);
void zc_fq_mul(zc_fe *r, const zc_fe *a, const zc_fe *b);

/* ---- shared, value-independent helpers ------------------------------- */
int  zc_fe_is_zero(const zc_fe *a);                      /* 1 or 0 */
int  zc_fe_eq(const zc_fe *a, const zc_fe *b);           /* 1 or 0 */
void zc_fe_cmov(zc_fe *r, const zc_fe *a, uint32_t c);   /* r = c ? a : r; c is 0/1 */
/* 1 iff the 32 little-endian bytes are < p (resp. q). */
int  zc_bytes_lt_p(const uint8_t b[32]);
int  zc_bytes_lt_q(const uint8_t b[32]);

/* ---- points ----------------------------------------------------------- */
void zc_pt_identity(zc_pt *r);
int  zc_pt_is_identity(const zc_pt *p);
void zc_pt_add(zc_pt *r, const zc_pt *a, const zc_pt *b);   /* complete */
void zc_pt_dbl(zc_pt *r, const zc_pt *a);
void zc_pt_neg(zc_pt *r, const zc_pt *a);
int  zc_pt_eq(const zc_pt *a, const zc_pt *b);
/* r = [k]p for any 256-bit little-endian k (reduced mod q implicitly). */
void zc_pt_mul(zc_pt *r, const uint8_t k[32], const zc_pt *p);
void zc_pt_mul_fq(zc_pt *r, const zc_fe *k, const zc_pt *p);
/* pasta_curves Point::from_bytes / to_bytes. decode returns 1 if valid. */
int  zc_pt_decode(zc_pt *r, const uint8_t b[32]);
void zc_pt_encode(uint8_t b[32], const zc_pt *p);
/* Extract_P: the affine x-coordinate (0 for the identity), canonical bytes. */
void zc_pt_extract(uint8_t b[32], const zc_pt *p);
/* Affine (x, y), for points known not to be the identity. */
void zc_pt_affine(zc_fe *x, zc_fe *y, const zc_pt *p);

/* pasta_curves hash_to_curve("pallas", domain)(msg): BLAKE2b-512
 * expand_message_xmd, simplified SWU onto iso-Pallas, 3-isogeny. */
void zc_hash_to_curve(zc_pt *r, const char *domain, const uint8_t *msg, size_t len);

/* Sinsemilla HashToPoint from Q over num_bits message bits (LSB first),
 * using the 1024-entry S table (x || y, canonical little-endian). */
void zc_sinsemilla(zc_pt *r, const zc_pt *Q, const uint8_t *bits, size_t num_bits,
                   const uint8_t *s_table);

#ifdef __cplusplus
}
#endif
