/* LeekWallet Zcash, C signer: Pallas field and curve arithmetic. See
 * zc_pallas.h. Constants come from tools/gen_pallas.py (zc_pallas_consts.h).
 */
#include "zc_pallas.h"

#include <string.h>

#include "blake2b.h"
#include "memzero.h"
#include "zc_pallas_consts.h"

/* ---- Montgomery arithmetic over 8 x 32-bit limbs ------------------------ */

/* Both moduli are 1 mod 2^32, so -m^-1 mod 2^32 = 0xffffffff and the
 * per-word reduction factor is simply -t[0]. */

/* r = a * b / 2^256 mod m, for a < 2^256 and b < m. */
static void mont_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8],
                     const uint32_t m[8])
{
    uint32_t t[10] = {0};
    for (int i = 0; i < 8; i++) {
        uint64_t c = 0;
        uint32_t bi = b[i];
        for (int j = 0; j < 8; j++) {
            c += (uint64_t)a[j] * bi + t[j];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[8];
        t[8] = (uint32_t)c;
        t[9] = (uint32_t)(c >> 32);

        uint32_t u = (uint32_t)0 - t[0];
        c = (uint64_t)u + t[0];             /* u * m[0] + t[0], m[0] = 1 */
        c >>= 32;
        for (int j = 1; j < 8; j++) {
            c += (uint64_t)u * m[j] + t[j];
            t[j - 1] = (uint32_t)c;
            c >>= 32;
        }
        c += t[8];
        t[7] = (uint32_t)c;
        t[8] = t[9] + (uint32_t)(c >> 32);
    }
    /* t < 2m: subtract m once if t >= m, without branching. */
    uint32_t s[8];
    uint64_t borrow = 0;
    for (int j = 0; j < 8; j++) {
        uint64_t d = (uint64_t)t[j] - m[j] - borrow;
        s[j] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
    /* t >= m  <=>  no final borrow, or t[8] set */
    uint32_t keep_t = (uint32_t)borrow & (uint32_t)(t[8] == 0 ? 1 : 0);
    uint32_t mask = (uint32_t)0 - keep_t;
    for (int j = 0; j < 8; j++) r[j] = (t[j] & mask) | (s[j] & ~mask);
}

static void mod_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8],
                    const uint32_t m[8])
{
    uint32_t t[8], s[8];
    uint64_t c = 0;
    for (int j = 0; j < 8; j++) {
        c += (uint64_t)a[j] + b[j];
        t[j] = (uint32_t)c;
        c >>= 32;
    }
    /* a + b < 2m < 2^256, so no carry out. */
    uint64_t borrow = 0;
    for (int j = 0; j < 8; j++) {
        uint64_t d = (uint64_t)t[j] - m[j] - borrow;
        s[j] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
    uint32_t mask = (uint32_t)0 - (uint32_t)borrow;   /* borrow: t < m, keep t */
    for (int j = 0; j < 8; j++) r[j] = (t[j] & mask) | (s[j] & ~mask);
}

static void mod_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8],
                    const uint32_t m[8])
{
    uint32_t t[8];
    uint64_t borrow = 0;
    for (int j = 0; j < 8; j++) {
        uint64_t d = (uint64_t)a[j] - b[j] - borrow;
        t[j] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
    uint32_t mask = (uint32_t)0 - (uint32_t)borrow;   /* went negative: add m */
    uint64_t c = 0;
    for (int j = 0; j < 8; j++) {
        c += (uint64_t)t[j] + (m[j] & mask);
        r[j] = (uint32_t)c;
        c >>= 32;
    }
}

static int lt_mod(const uint32_t a[8], const uint32_t m[8])
{
    uint64_t borrow = 0;
    for (int j = 0; j < 8; j++) {
        uint64_t d = (uint64_t)a[j] - m[j] - borrow;
        borrow = (d >> 32) & 1;
    }
    return (int)borrow;
}

static void load_le(uint32_t r[8], const uint8_t b[32])
{
    for (int j = 0; j < 8; j++)
        r[j] = (uint32_t)b[4 * j] | (uint32_t)b[4 * j + 1] << 8 |
               (uint32_t)b[4 * j + 2] << 16 | (uint32_t)b[4 * j + 3] << 24;
}

static void store_le(uint8_t b[32], const uint32_t a[8])
{
    for (int j = 0; j < 8; j++) {
        b[4 * j] = (uint8_t)a[j];
        b[4 * j + 1] = (uint8_t)(a[j] >> 8);
        b[4 * j + 2] = (uint8_t)(a[j] >> 16);
        b[4 * j + 3] = (uint8_t)(a[j] >> 24);
    }
}

static const uint32_t ONE_RAW[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };

/* Generic helpers parameterized by modulus. */
static int from_bytes(zc_fe *r, const uint8_t b[32], const uint32_t m[8], const uint32_t r2[8])
{
    uint32_t x[8];
    load_le(x, b);
    int ok = lt_mod(x, m);
    uint32_t mask = (uint32_t)0 - (uint32_t)ok;
    for (int j = 0; j < 8; j++) x[j] &= mask;        /* non-canonical -> 0 */
    mont_mul(r->v, x, r2, m);
    memzero(x, sizeof x);
    return ok;
}

static void to_bytes(uint8_t b[32], const zc_fe *a, const uint32_t m[8])
{
    uint32_t x[8];
    mont_mul(x, a->v, ONE_RAW, m);
    store_le(b, x);
    memzero(x, sizeof x);
}

static void from_wide(zc_fe *r, const uint8_t w[64], const uint32_t m[8],
                      const uint32_t r2[8], const uint32_t r3[8])
{
    uint32_t lo[8], hi[8], a[8], b[8];
    load_le(lo, w);
    load_le(hi, w + 32);
    mont_mul(a, lo, r2, m);   /* lo * R      */
    mont_mul(b, hi, r3, m);   /* hi * R^2    */
    mod_add(r->v, a, b, m);   /* (lo + hi 2^256) R */
    memzero(lo, sizeof lo);
    memzero(hi, sizeof hi);
    memzero(a, sizeof a);
    memzero(b, sizeof b);
}

static void set_u64(zc_fe *r, uint64_t v, const uint32_t m[8], const uint32_t r2[8])
{
    uint32_t x[8] = { (uint32_t)v, (uint32_t)(v >> 32), 0, 0, 0, 0, 0, 0 };
    mont_mul(r->v, x, r2, m);
}

/* r = a^e for a public exponent e (left to right; the branch is on e). */
static void pow_pub(zc_fe *r, const zc_fe *a, const uint32_t e[8], const uint32_t m[8],
                    const uint32_t r2[8])
{
    zc_fe acc;
    set_u64(&acc, 1, m, r2);
    int started = 0;
    for (int i = 255; i >= 0; i--) {
        if (started) mont_mul(acc.v, acc.v, acc.v, m);
        if ((e[i / 32] >> (i % 32)) & 1) {
            mont_mul(acc.v, acc.v, a->v, m);
            started = 1;
        }
    }
    *r = acc;
    memzero(&acc, sizeof acc);
}

/* ---- Fp ------------------------------------------------------------------ */

void zc_fp_set_u64(zc_fe *r, uint64_t v) { set_u64(r, v, ZC_P, ZC_P_R2); }
int zc_fp_from_bytes(zc_fe *r, const uint8_t b[32]) { return from_bytes(r, b, ZC_P, ZC_P_R2); }
void zc_fp_to_bytes(uint8_t b[32], const zc_fe *a) { to_bytes(b, a, ZC_P); }
void zc_fp_from_wide(zc_fe *r, const uint8_t w[64]) { from_wide(r, w, ZC_P, ZC_P_R2, ZC_P_R3); }
void zc_fp_add(zc_fe *r, const zc_fe *a, const zc_fe *b) { mod_add(r->v, a->v, b->v, ZC_P); }
void zc_fp_sub(zc_fe *r, const zc_fe *a, const zc_fe *b) { mod_sub(r->v, a->v, b->v, ZC_P); }
void zc_fp_mul(zc_fe *r, const zc_fe *a, const zc_fe *b) { mont_mul(r->v, a->v, b->v, ZC_P); }
void zc_fp_sqr(zc_fe *r, const zc_fe *a) { mont_mul(r->v, a->v, a->v, ZC_P); }
void zc_fp_inv(zc_fe *r, const zc_fe *a) { pow_pub(r, a, ZC_P_MINUS2, ZC_P, ZC_P_R2); }

void zc_fp_neg(zc_fe *r, const zc_fe *a)
{
    static const uint32_t zero[8];
    mod_sub(r->v, zero, a->v, ZC_P);
}

int zc_fp_is_odd(const zc_fe *a)
{
    uint8_t b[32];
    zc_fp_to_bytes(b, a);
    int odd = b[0] & 1;
    memzero(b, sizeof b);
    return odd;
}

/* RFC 9380 Appendix I.4: constant-time Tonelli-Shanks (p - 1 = 2^32 T). */
int zc_fp_sqrt(zc_fe *r, const zc_fe *a)
{
    zc_fe z, t, b, c, tt, zt, one, chk;
    zc_fp_set_u64(&one, 1);
    pow_pub(&z, a, ZC_P_T_MINUS1_HALF, ZC_P, ZC_P_R2);   /* x^((T-1)/2) */
    zc_fp_sqr(&t, &z);
    zc_fp_mul(&t, &t, a);                                   /* x^T */
    zc_fp_mul(&z, &z, a);                                   /* x^((T+1)/2) */
    b = t;
    memcpy(c.v, ZC_P_ROOT_M, sizeof c.v);
    for (int i = ZC_P_TWO_ADICITY; i >= 2; i--) {
        for (int j = 1; j <= i - 2; j++) zc_fp_sqr(&b, &b);
        uint32_t e = (uint32_t)zc_fe_eq(&b, &one);
        zc_fp_mul(&zt, &z, &c);
        zc_fe_cmov(&zt, &z, e);       /* zt = e ? z : z * c */
        z = zt;
        zc_fp_sqr(&c, &c);
        zc_fp_mul(&tt, &t, &c);
        zc_fe_cmov(&tt, &t, e);
        t = tt;
        b = t;
    }
    zc_fp_sqr(&chk, &z);
    int ok = zc_fe_eq(&chk, a);
    *r = z;
    memzero(&z, sizeof z);
    memzero(&t, sizeof t);
    memzero(&b, sizeof b);
    memzero(&tt, sizeof tt);
    memzero(&zt, sizeof zt);
    memzero(&chk, sizeof chk);
    return ok;
}

/* ---- Fq ------------------------------------------------------------------ */

void zc_fq_set_u64(zc_fe *r, uint64_t v) { set_u64(r, v, ZC_Q, ZC_Q_R2); }
int zc_fq_from_bytes(zc_fe *r, const uint8_t b[32]) { return from_bytes(r, b, ZC_Q, ZC_Q_R2); }
void zc_fq_to_bytes(uint8_t b[32], const zc_fe *a) { to_bytes(b, a, ZC_Q); }
void zc_fq_from_wide(zc_fe *r, const uint8_t w[64]) { from_wide(r, w, ZC_Q, ZC_Q_R2, ZC_Q_R3); }
void zc_fq_add(zc_fe *r, const zc_fe *a, const zc_fe *b) { mod_add(r->v, a->v, b->v, ZC_Q); }
void zc_fq_mul(zc_fe *r, const zc_fe *a, const zc_fe *b) { mont_mul(r->v, a->v, b->v, ZC_Q); }

void zc_fq_neg(zc_fe *r, const zc_fe *a)
{
    static const uint32_t zero[8];
    mod_sub(r->v, zero, a->v, ZC_Q);
}

/* ---- shared helpers ------------------------------------------------------- */

int zc_fe_is_zero(const zc_fe *a)
{
    uint32_t acc = 0;
    for (int j = 0; j < 8; j++) acc |= a->v[j];
    return (int)(((acc | ((uint32_t)0 - acc)) >> 31) ^ 1);
}

int zc_fe_eq(const zc_fe *a, const zc_fe *b)
{
    uint32_t acc = 0;
    for (int j = 0; j < 8; j++) acc |= a->v[j] ^ b->v[j];
    return (int)(((acc | ((uint32_t)0 - acc)) >> 31) ^ 1);
}

void zc_fe_cmov(zc_fe *r, const zc_fe *a, uint32_t c)
{
    uint32_t mask = (uint32_t)0 - (c & 1);
    for (int j = 0; j < 8; j++) r->v[j] ^= mask & (r->v[j] ^ a->v[j]);
}

int zc_bytes_lt_p(const uint8_t b[32])
{
    uint32_t x[8];
    load_le(x, b);
    return lt_mod(x, ZC_P);
}

int zc_bytes_lt_q(const uint8_t b[32])
{
    uint32_t x[8];
    load_le(x, b);
    return lt_mod(x, ZC_Q);
}

/* ---- points: Renes-Costello-Batina, a = 0, b = 5 (b3 = 15) --------------- */

static void mul_b3(zc_fe *r, const zc_fe *a)
{
    zc_fe t2, t16;
    zc_fp_add(&t2, a, a);
    zc_fp_add(&t16, &t2, &t2);
    zc_fp_add(&t16, &t16, &t16);
    zc_fp_add(&t16, &t16, &t16);      /* 16a */
    zc_fp_sub(r, &t16, a);            /* 15a */
}

void zc_pt_identity(zc_pt *r)
{
    memset(r, 0, sizeof *r);
    zc_fp_set_u64(&r->y, 1);
}

int zc_pt_is_identity(const zc_pt *p) { return zc_fe_is_zero(&p->z); }

/* Algorithm 7 (complete addition, a = 0). r may alias a or b. */
void zc_pt_add(zc_pt *r, const zc_pt *a, const zc_pt *b)
{
    zc_fe t0, t1, t2, t3, t4, x3, y3, z3;
    zc_fp_mul(&t0, &a->x, &b->x);
    zc_fp_mul(&t1, &a->y, &b->y);
    zc_fp_mul(&t2, &a->z, &b->z);
    zc_fp_add(&t3, &a->x, &a->y);
    zc_fp_add(&t4, &b->x, &b->y);
    zc_fp_mul(&t3, &t3, &t4);
    zc_fp_add(&t4, &t0, &t1);
    zc_fp_sub(&t3, &t3, &t4);
    zc_fp_add(&t4, &a->y, &a->z);
    zc_fp_add(&x3, &b->y, &b->z);
    zc_fp_mul(&t4, &t4, &x3);
    zc_fp_add(&x3, &t1, &t2);
    zc_fp_sub(&t4, &t4, &x3);
    zc_fp_add(&x3, &a->x, &a->z);
    zc_fp_add(&y3, &b->x, &b->z);
    zc_fp_mul(&x3, &x3, &y3);
    zc_fp_add(&y3, &t0, &t2);
    zc_fp_sub(&y3, &x3, &y3);
    zc_fp_add(&x3, &t0, &t0);
    zc_fp_add(&t0, &x3, &t0);
    mul_b3(&t2, &t2);
    zc_fp_add(&z3, &t1, &t2);
    zc_fp_sub(&t1, &t1, &t2);
    mul_b3(&y3, &y3);
    zc_fp_mul(&x3, &t4, &y3);
    zc_fp_mul(&t2, &t3, &t1);
    zc_fp_sub(&x3, &t2, &x3);
    zc_fp_mul(&y3, &y3, &t0);
    zc_fp_mul(&t1, &t1, &z3);
    zc_fp_add(&y3, &t1, &y3);
    zc_fp_mul(&t0, &t0, &t3);
    zc_fp_mul(&z3, &z3, &t4);
    zc_fp_add(&z3, &z3, &t0);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

/* Algorithm 9 (doubling, a = 0). r may alias a. */
void zc_pt_dbl(zc_pt *r, const zc_pt *a)
{
    zc_fe t0, t1, t2, x3, y3, z3;
    zc_fp_sqr(&t0, &a->y);
    zc_fp_add(&z3, &t0, &t0);
    zc_fp_add(&z3, &z3, &z3);
    zc_fp_add(&z3, &z3, &z3);
    zc_fp_mul(&t1, &a->y, &a->z);
    zc_fp_sqr(&t2, &a->z);
    mul_b3(&t2, &t2);
    zc_fp_mul(&x3, &t2, &z3);
    zc_fp_add(&y3, &t0, &t2);
    zc_fp_mul(&z3, &t1, &z3);
    zc_fp_add(&t1, &t2, &t2);
    zc_fp_add(&t2, &t1, &t2);
    zc_fp_sub(&t0, &t0, &t2);
    zc_fp_mul(&y3, &t0, &y3);
    zc_fp_add(&y3, &x3, &y3);
    zc_fp_mul(&t1, &a->x, &a->y);
    zc_fp_mul(&x3, &t0, &t1);
    zc_fp_add(&x3, &x3, &x3);
    r->x = x3;
    r->y = y3;
    r->z = z3;
}

void zc_pt_neg(zc_pt *r, const zc_pt *a)
{
    r->x = a->x;
    zc_fp_neg(&r->y, &a->y);
    r->z = a->z;
}

int zc_pt_eq(const zc_pt *a, const zc_pt *b)
{
    zc_fe l, r;
    int ia = zc_pt_is_identity(a), ib = zc_pt_is_identity(b);
    if (ia || ib) return ia && ib;
    zc_fp_mul(&l, &a->x, &b->z);
    zc_fp_mul(&r, &b->x, &a->z);
    int ex = zc_fe_eq(&l, &r);
    zc_fp_mul(&l, &a->y, &b->z);
    zc_fp_mul(&r, &b->y, &a->z);
    return ex & zc_fe_eq(&l, &r);
}

static void pt_cmov(zc_pt *r, const zc_pt *a, uint32_t c)
{
    zc_fe_cmov(&r->x, &a->x, c);
    zc_fe_cmov(&r->y, &a->y, c);
    zc_fe_cmov(&r->z, &a->z, c);
}

void zc_pt_mul(zc_pt *r, const uint8_t k[32], const zc_pt *p)
{
    zc_pt tab[16], acc, sel;
    zc_pt_identity(&tab[0]);
    tab[1] = *p;
    for (int i = 2; i < 16; i++) {
        if (i & 1) zc_pt_add(&tab[i], &tab[i - 1], p);
        else zc_pt_dbl(&tab[i], &tab[i / 2]);
    }
    zc_pt_identity(&acc);
    for (int w = 63; w >= 0; w--) {
        uint32_t nib = (uint32_t)(k[w / 2] >> (4 * (w & 1))) & 0xF;
        if (w != 63)
            for (int d = 0; d < 4; d++) zc_pt_dbl(&acc, &acc);
        zc_pt_identity(&sel);
        for (uint32_t i = 1; i < 16; i++) {
            uint32_t x = i ^ nib;
            uint32_t eq = ((x | ((uint32_t)0 - x)) >> 31) ^ 1;
            pt_cmov(&sel, &tab[i], eq);
        }
        zc_pt_add(&acc, &acc, &sel);
    }
    *r = acc;
    memzero(tab, sizeof tab);
    memzero(&acc, sizeof acc);
    memzero(&sel, sizeof sel);
}

void zc_pt_mul_fq(zc_pt *r, const zc_fe *k, const zc_pt *p)
{
    uint8_t kb[32];
    zc_fq_to_bytes(kb, k);
    zc_pt_mul(r, kb, p);
    memzero(kb, sizeof kb);
}

void zc_pt_affine(zc_fe *x, zc_fe *y, const zc_pt *p)
{
    zc_fe zi;
    zc_fp_inv(&zi, &p->z);
    zc_fp_mul(x, &p->x, &zi);
    zc_fp_mul(y, &p->y, &zi);
    memzero(&zi, sizeof zi);
}

void zc_pt_encode(uint8_t b[32], const zc_pt *p)
{
    if (zc_pt_is_identity(p)) {
        memset(b, 0, 32);
        return;
    }
    zc_fe x, y;
    zc_pt_affine(&x, &y, p);
    zc_fp_to_bytes(b, &x);
    b[31] |= (uint8_t)(zc_fp_is_odd(&y) << 7);
    memzero(&x, sizeof x);
    memzero(&y, sizeof y);
}

void zc_pt_extract(uint8_t b[32], const zc_pt *p)
{
    if (zc_pt_is_identity(p)) {
        memset(b, 0, 32);
        return;
    }
    zc_fe x, y;
    zc_pt_affine(&x, &y, p);
    zc_fp_to_bytes(b, &x);
    memzero(&x, sizeof x);
    memzero(&y, sizeof y);
}

int zc_pt_decode(zc_pt *r, const uint8_t b[32])
{
    uint8_t xb[32];
    zc_fe x, y2, y, five;
    memcpy(xb, b, 32);
    uint32_t ysign = xb[31] >> 7;
    xb[31] &= 0x7F;
    if (!zc_fp_from_bytes(&x, xb)) return 0;
    if (zc_fe_is_zero(&x) && !ysign) {
        zc_pt_identity(r);
        return 1;
    }
    zc_fp_sqr(&y2, &x);
    zc_fp_mul(&y2, &y2, &x);
    zc_fp_set_u64(&five, 5);
    zc_fp_add(&y2, &y2, &five);
    if (!zc_fp_sqrt(&y, &y2)) return 0;
    if ((uint32_t)zc_fp_is_odd(&y) != ysign) zc_fp_neg(&y, &y);
    r->x = x;
    r->y = y;
    zc_fp_set_u64(&r->z, 1);
    return 1;
}

/* ---- hash to curve -------------------------------------------------------- */

static void blake2b_dst(blake2b_state *s, const char *domain)
{
    static const char curve[] = "pallas";
    static const char tail[] = "_XMD:BLAKE2b_SSWU_RO_";
    size_t dl = strlen(domain);
    uint8_t len = (uint8_t)(22 + (sizeof curve - 1) + dl);
    blake2b_Update(s, (const uint8_t *)domain, dl);
    blake2b_Update(s, (const uint8_t *)"-", 1);
    blake2b_Update(s, (const uint8_t *)curve, sizeof curve - 1);
    blake2b_Update(s, (const uint8_t *)tail, sizeof tail - 1);
    blake2b_Update(s, &len, 1);
}

static void hash_to_field(zc_fe u[2], const char *domain, const uint8_t *msg, size_t len)
{
    static const uint8_t zero_personal[16];
    static const uint8_t zeros[128];
    uint8_t b0[64], b1[64], b2[64], x[64];
    blake2b_state s;
    blake2b_InitPersonal(&s, 64, zero_personal, 16);
    blake2b_Update(&s, zeros, 128);
    if (len) blake2b_Update(&s, msg, len);
    static const uint8_t hdr[3] = { 0, 128, 0 };
    blake2b_Update(&s, hdr, 3);
    blake2b_dst(&s, domain);
    blake2b_Final(&s, b0, 64);

    uint8_t one = 1, two = 2;
    blake2b_InitPersonal(&s, 64, zero_personal, 16);
    blake2b_Update(&s, b0, 64);
    blake2b_Update(&s, &one, 1);
    blake2b_dst(&s, domain);
    blake2b_Final(&s, b1, 64);

    for (int i = 0; i < 64; i++) x[i] = b0[i] ^ b1[i];
    blake2b_InitPersonal(&s, 64, zero_personal, 16);
    blake2b_Update(&s, x, 64);
    blake2b_Update(&s, &two, 1);
    blake2b_dst(&s, domain);
    blake2b_Final(&s, b2, 64);

    /* Each block is a big-endian integer, reduced mod p. */
    for (int i = 0; i < 64; i++) x[i] = b1[63 - i];
    zc_fp_from_wide(&u[0], x);
    for (int i = 0; i < 64; i++) x[i] = b2[63 - i];
    zc_fp_from_wide(&u[1], x);
}

static void fe_load(zc_fe *r, const uint32_t v[8]) { memcpy(r->v, v, sizeof r->v); }

/* g(x) = x^3 + A x + B on iso-Pallas. */
static void iso_g(zc_fe *r, const zc_fe *x)
{
    zc_fe a, b, t;
    fe_load(&a, ZC_P_ISO_A_M);
    fe_load(&b, ZC_P_ISO_B_M);
    zc_fp_sqr(&t, x);
    zc_fp_add(&t, &t, &a);
    zc_fp_mul(&t, &t, x);
    zc_fp_add(r, &t, &b);
}

/* RFC 9380 simplified SWU onto iso-Pallas, affine. */
static void map_sswu(zc_fe *x, zc_fe *y, const zc_fe *u)
{
    zc_fe A, B, Z, one, zu2, ta, x1, x2, gx, t;
    fe_load(&A, ZC_P_ISO_A_M);
    fe_load(&B, ZC_P_ISO_B_M);
    fe_load(&Z, ZC_P_SSWU_Z_M);
    zc_fp_set_u64(&one, 1);

    zc_fp_sqr(&zu2, u);
    zc_fp_mul(&zu2, &zu2, &Z);            /* Z u^2 */
    zc_fp_sqr(&ta, &zu2);
    zc_fp_add(&ta, &ta, &zu2);            /* Z^2 u^4 + Z u^2 */
    if (zc_fe_is_zero(&ta)) {
        /* x1 = B / (Z A) */
        zc_fp_mul(&t, &Z, &A);
        zc_fp_inv(&t, &t);
        zc_fp_mul(&x1, &B, &t);
    } else {
        /* x1 = (-B / A) (1 + 1/ta) */
        zc_fp_inv(&t, &ta);
        zc_fp_add(&t, &t, &one);
        zc_fp_mul(&t, &t, &B);
        zc_fp_inv(&x1, &A);
        zc_fp_mul(&x1, &x1, &t);
        zc_fp_neg(&x1, &x1);
    }
    iso_g(&gx, &x1);
    if (zc_fp_sqrt(y, &gx)) {
        *x = x1;
    } else {
        zc_fp_mul(&x2, &zu2, &x1);
        iso_g(&gx, &x2);
        zc_fp_sqrt(y, &gx);               /* a square whenever g(x1) is not */
        *x = x2;
    }
    if (zc_fp_is_odd(u) != zc_fp_is_odd(y)) zc_fp_neg(y, y);
}

void zc_hash_to_curve(zc_pt *r, const char *domain, const uint8_t *msg, size_t len)
{
    zc_fe u[2], x0, y0, x1, y1, l, t, x3, y3, A;
    hash_to_field(u, domain, msg, len);
    map_sswu(&x0, &y0, &u[0]);
    map_sswu(&x1, &y1, &u[1]);

    /* Affine addition on iso-Pallas (general A). */
    if (zc_fe_eq(&x0, &x1)) {
        zc_fe sy;
        zc_fp_add(&sy, &y0, &y1);
        if (zc_fe_is_zero(&sy)) {          /* q1 = -q0: the sum is the identity */
            zc_pt_identity(r);
            return;
        }
        fe_load(&A, ZC_P_ISO_A_M);         /* tangent: (3 x^2 + A) / 2y */
        zc_fp_sqr(&l, &x0);
        zc_fp_add(&t, &l, &l);
        zc_fp_add(&l, &t, &l);
        zc_fp_add(&l, &l, &A);
        zc_fp_add(&t, &y0, &y0);
        zc_fp_inv(&t, &t);
        zc_fp_mul(&l, &l, &t);
    } else {
        zc_fp_sub(&l, &y1, &y0);
        zc_fp_sub(&t, &x1, &x0);
        zc_fp_inv(&t, &t);
        zc_fp_mul(&l, &l, &t);
    }
    zc_fp_sqr(&x3, &l);
    zc_fp_sub(&x3, &x3, &x0);
    zc_fp_sub(&x3, &x3, &x1);
    zc_fp_sub(&t, &x0, &x3);
    zc_fp_mul(&y3, &l, &t);
    zc_fp_sub(&y3, &y3, &y0);

    /* 3-isogeny to Pallas. */
    zc_fe k[13], nx, dx, ny, dy;
    for (int i = 0; i < 13; i++) fe_load(&k[i], ZC_P_ISO_M[i]);
    zc_fp_mul(&nx, &k[0], &x3);
    zc_fp_add(&nx, &nx, &k[1]);
    zc_fp_mul(&nx, &nx, &x3);
    zc_fp_add(&nx, &nx, &k[2]);
    zc_fp_mul(&nx, &nx, &x3);
    zc_fp_add(&nx, &nx, &k[3]);
    zc_fp_add(&dx, &x3, &k[4]);
    zc_fp_mul(&dx, &dx, &x3);
    zc_fp_add(&dx, &dx, &k[5]);
    zc_fp_mul(&ny, &k[6], &x3);
    zc_fp_add(&ny, &ny, &k[7]);
    zc_fp_mul(&ny, &ny, &x3);
    zc_fp_add(&ny, &ny, &k[8]);
    zc_fp_mul(&ny, &ny, &x3);
    zc_fp_add(&ny, &ny, &k[9]);
    zc_fp_mul(&ny, &ny, &y3);
    zc_fp_add(&dy, &x3, &k[10]);
    zc_fp_mul(&dy, &dy, &x3);
    zc_fp_add(&dy, &dy, &k[11]);
    zc_fp_mul(&dy, &dy, &x3);
    zc_fp_add(&dy, &dy, &k[12]);
    if (zc_fe_is_zero(&dx) || zc_fe_is_zero(&dy)) {   /* kernel of the isogeny */
        zc_pt_identity(r);
        return;
    }
    /* (nx/dx, ny/dy) as projective (nx dy : ny dx : dx dy). */
    zc_fp_mul(&r->x, &nx, &dy);
    zc_fp_mul(&r->y, &ny, &dx);
    zc_fp_mul(&r->z, &dx, &dy);
}

/* ---- Sinsemilla ------------------------------------------------------------ */

/* The table index is the message chunk, so lookups depend on message bits.
 * Every Sinsemilla message here is either PCZT data the host already holds
 * (output and spent notes) or viewing-key material (ak, nk in CommitIvk);
 * no spending secret ever reaches this function. */
void zc_sinsemilla(zc_pt *r, const zc_pt *Q, const uint8_t *bits, size_t num_bits,
                   const uint8_t *s_table)
{
    zc_pt acc = *Q, s;
    size_t chunks = (num_bits + 9) / 10;
    for (size_t i = 0; i < chunks; i++) {
        uint32_t j = 0;
        for (int b = 0; b < 10; b++) {
            size_t bit = i * 10 + (size_t)b;
            if (bit < num_bits && (bits[bit / 8] >> (bit % 8)) & 1) j |= 1u << b;
        }
        const uint8_t *e = s_table + (size_t)j * 64;
        uint32_t xr[8], yr[8];
        load_le(xr, e);
        load_le(yr, e + 32);
        mont_mul(s.x.v, xr, ZC_P_R2, ZC_P);
        mont_mul(s.y.v, yr, ZC_P_R2, ZC_P);
        zc_fp_set_u64(&s.z, 1);
        zc_pt_add(&s, &acc, &s);          /* acc + S */
        zc_pt_add(&acc, &s, &acc);        /* (acc + S) + acc */
    }
    *r = acc;
}
