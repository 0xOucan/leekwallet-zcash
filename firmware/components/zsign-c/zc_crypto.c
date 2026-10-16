/* LeekWallet Zcash, C signer: Orchard primitives. See zc_crypto.h.
 *
 * Group and field arithmetic is zc_pallas.c (ours, constant time). From the
 * vendored library we take BLAKE2b, ChaCha20-Poly1305, the ZIP 32 Orchard
 * child derivation (BLAKE2b only) and FF1-AES-256 for diversifiers.
 * Every buffer that held a secret is wiped before return.
 */
#include "zc_crypto.h"

#include <string.h>

#include "blake2b.h"
#include "chacha20poly1305.h"
#include "memzero.h"
#include "aes_ct.h"
#include "orchard.h"
#include "zc_poseidon_consts.h"

/* ---- canonical encodings ------------------------------------------------ */

/* BLS12-381 scalar field r (= Jubjub base field) and the Jubjub scalar
 * field, little-endian. Used only for the (empty) Sapling bundle's anchor
 * and bsk, which the Rust side parses. */
static const uint8_t BLS_R_LE[32] = {
    0x01,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xfe,0x5b,0xfe,0xff,0x02,0xa4,0xbd,0x53,
    0x05,0xd8,0xa1,0x09,0x08,0xd8,0x39,0x33,0x48,0x7d,0x9d,0x29,0x53,0xa7,0xed,0x73};
static const uint8_t JJ_R_LE[32] = {
    0xb7,0x2c,0xf7,0xd6,0x5e,0x0e,0x97,0xd0,0x82,0x10,0xc8,0xcc,0x93,0x20,0x68,0xa6,
    0x00,0x3b,0x34,0x01,0x01,0x3b,0x67,0x06,0xa9,0xaf,0x33,0x65,0xea,0xb4,0x7d,0x0e};

static int lt_le(const uint8_t b[32], const uint8_t m[32])
{
    for (int i = 31; i >= 0; i--) {
        if (b[i] < m[i]) return 1;
        if (b[i] > m[i]) return 0;
    }
    return 0;
}

int zc_fp_canonical(const uint8_t b[32]) { return zc_bytes_lt_p(b); }
int zc_fq_canonical(const uint8_t b[32]) { return zc_bytes_lt_q(b); }
int zc_bls_fr_canonical(const uint8_t b[32]) { return lt_le(b, BLS_R_LE); }
int zc_jubjub_fr_canonical(const uint8_t b[32]) { return lt_le(b, JJ_R_LE); }

static int is_zero32(const uint8_t b[32])
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= b[i];
    return acc == 0;
}

int zc_point_valid(const uint8_t b[32])
{
    zc_pt p;
    return zc_pt_decode(&p, b);
}

int zc_point_valid_nonid(const uint8_t b[32])
{
    zc_pt p;
    return zc_pt_decode(&p, b) && !zc_pt_is_identity(&p);
}

/* ---- Sinsemilla S table and cached generators --------------------------- */

extern const uint8_t zc_sinsemilla_s[1024 * 64];

/* BLAKE2b-256 of the 64 KiB table (the same fingerprint the vendored
 * library checks; the table itself is vendored byte for byte). */
static const uint8_t S_TABLE_DIGEST[32] = {
    0xf5,0x3e,0x52,0x90,0xdd,0xa0,0xd2,0x25,0x7b,0x1f,0x2c,0xf6,0x4f,0x0b,0x16,0xe9,
    0xdf,0xdf,0xe9,0xcc,0x9a,0x9b,0x24,0x9c,0x91,0x37,0xcc,0x6a,0xab,0x45,0x25,0x97};

static zc_pt G_SPEND, G_K, G_CV_V, G_CV_R, G_NC_R, G_NC_Q, G_IVK_R, G_IVK_Q, G_GD_EMPTY;
static int s_init;   /* 0 = not yet, 1 = ok, -1 = table corrupt */

int zc_crypto_init(void)
{
    if (s_init) return s_init == 1 ? 0 : -1;
    uint8_t d[32];
    blake2b_state s;
    blake2b_Init(&s, 32);
    blake2b_Update(&s, zc_sinsemilla_s, sizeof zc_sinsemilla_s);
    blake2b_Final(&s, d, 32);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= d[i] ^ S_TABLE_DIGEST[i];
    if (diff) {
        s_init = -1;
        return -1;
    }
    static const char nc[] = "z.cash:Orchard-NoteCommit-M";
    static const char ivk[] = "z.cash:Orchard-CommitIvk-M";
    zc_hash_to_curve(&G_SPEND, "z.cash:Orchard", (const uint8_t *)"G", 1);
    zc_hash_to_curve(&G_K, "z.cash:Orchard", (const uint8_t *)"K", 1);
    zc_hash_to_curve(&G_CV_V, "z.cash:Orchard-cv", (const uint8_t *)"v", 1);
    zc_hash_to_curve(&G_CV_R, "z.cash:Orchard-cv", (const uint8_t *)"r", 1);
    zc_hash_to_curve(&G_NC_R, "z.cash:Orchard-NoteCommit-r", NULL, 0);
    zc_hash_to_curve(&G_NC_Q, "z.cash:SinsemillaQ", (const uint8_t *)nc, sizeof nc - 1);
    zc_hash_to_curve(&G_IVK_R, "z.cash:Orchard-CommitIvk-r", NULL, 0);
    zc_hash_to_curve(&G_IVK_Q, "z.cash:SinsemillaQ", (const uint8_t *)ivk, sizeof ivk - 1);
    zc_hash_to_curve(&G_GD_EMPTY, "z.cash:Orchard-gd", NULL, 0);
    s_init = 1;
    return 0;
}

/* ---- PRF^expand, ToScalar, ToBase --------------------------------------- */

void zc_prf_expand(const uint8_t key[32], const uint8_t *t1, size_t n1,
                   const uint8_t *t2, size_t n2, const uint8_t *t3, size_t n3,
                   uint8_t out[64])
{
    blake2b_state s;
    blake2b_InitPersonal(&s, 64, "Zcash_ExpandSeed", 16);
    blake2b_Update(&s, key, 32);
    if (n1) blake2b_Update(&s, t1, n1);
    if (n2) blake2b_Update(&s, t2, n2);
    if (n3) blake2b_Update(&s, t3, n3);
    blake2b_Final(&s, out, 64);
    memzero(&s, sizeof s);
}

void zc_to_scalar(const uint8_t wide[64], uint8_t out[32])
{
    zc_fe x;
    zc_fq_from_wide(&x, wide);
    zc_fq_to_bytes(out, &x);
    memzero(&x, sizeof x);
}

void zc_to_base(const uint8_t wide[64], uint8_t out[32])
{
    zc_fe x;
    zc_fp_from_wide(&x, wide);
    zc_fp_to_bytes(out, &x);
    memzero(&x, sizeof x);
}

/* ---- Orchard helpers ---------------------------------------------------- */

void zc_diversify_hash(zc_pt *gd, const uint8_t d[11])
{
    zc_hash_to_curve(gd, "z.cash:Orchard-gd", d, 11);
    if (zc_pt_is_identity(gd)) *gd = G_GD_EMPTY;
}

int zc_same_receiver(const uint8_t a[43], const uint8_t b[43])
{
    if (memcmp(a + 11, b + 11, 32) != 0) return 0;
    if (memcmp(a, b, 11) == 0) return 1;
    zc_pt ga, gb;
    zc_diversify_hash(&ga, a);
    zc_diversify_hash(&gb, b);
    return zc_pt_eq(&ga, &gb);
}

void zc_value_commit(uint8_t out[32], uint64_t v_spend, uint64_t v_output,
                     const uint8_t rcv[32])
{
    uint64_t mag = v_spend >= v_output ? v_spend - v_output : v_output - v_spend;
    uint8_t m[32] = {0};
    for (int i = 0; i < 8; i++) m[i] = (uint8_t)(mag >> (8 * i));
    zc_pt a, b;
    zc_pt_mul(&a, m, &G_CV_V);
    if (v_spend < v_output) zc_pt_neg(&a, &a);
    zc_pt_mul(&b, rcv, &G_CV_R);
    zc_pt_add(&a, &a, &b);
    zc_pt_encode(out, &a);
    memzero(&b, sizeof b);
}

void zc_note_psi(uint8_t psi[32], const uint8_t rseed[32], const uint8_t rho[32])
{
    uint8_t w[64], d = 0x09;
    zc_prf_expand(rseed, &d, 1, rho, 32, NULL, 0, w);
    zc_to_base(w, psi);
    memzero(w, sizeof w);
}

void zc_note_esk(uint8_t esk[32], const uint8_t rseed[32], const uint8_t rho[32])
{
    uint8_t w[64], d = 0x04;
    zc_prf_expand(rseed, &d, 1, rho, 32, NULL, 0, w);
    zc_to_scalar(w, esk);
    memzero(w, sizeof w);
}

int zc_rseed_valid(const uint8_t rseed[32], const uint8_t rho[32])
{
    uint8_t esk[32];
    zc_note_esk(esk, rseed, rho);
    int ok = !is_zero32(esk);
    memzero(esk, sizeof esk);
    return ok;
}

int zc_note_commit(zc_pt *cm, const uint8_t recipient[43], uint64_t value,
                   const uint8_t rho[32], const uint8_t rseed[32], uint8_t version)
{
    static uint8_t msg[136], pre[105];
    uint8_t gd_b[32], psi[32], rcm[32], w[64];
    zc_pt pk, gd, s, rr;
    if (!zc_pt_decode(&pk, recipient + 11) || zc_pt_is_identity(&pk)) return -1;
    zc_diversify_hash(&gd, recipient);
    zc_pt_encode(gd_b, &gd);

    /* g_d (256) || pk_d (256) || v (64) || rho (255) || psi (255) bits */
    memset(msg, 0, sizeof msg);
    memcpy(msg, gd_b, 32);
    memcpy(msg + 32, recipient + 11, 32);
    for (int i = 0; i < 8; i++) msg[64 + i] = (uint8_t)(value >> (8 * i));
    memcpy(msg + 72, rho, 31);
    msg[103] = rho[31] & 0x7F;
    zc_note_psi(psi, rseed, rho);
    for (int k = 0; k < 32; k++) {
        uint8_t b = k == 31 ? (psi[k] & 0x7F) : psi[k];
        msg[103 + k] |= (uint8_t)(b << 7);
        msg[104 + k] |= (uint8_t)(b >> 1);
    }

    if (version == 3) {
        /* ZIP 2005: rcm = ToScalar(PRF^expand(rseed, [0x0B] || g_d || pk_d ||
         * I2LEOSP64(v) || rho || psi)). */
        pre[0] = 0x0B;
        memcpy(pre + 1, gd_b, 32);
        memcpy(pre + 33, recipient + 11, 32);
        memcpy(pre + 65, msg + 64, 8);
        memcpy(pre + 73, rho, 32);
        zc_prf_expand(rseed, pre, sizeof pre, psi, 32, NULL, 0, w);
    } else {
        uint8_t d = 0x05;
        zc_prf_expand(rseed, &d, 1, rho, 32, NULL, 0, w);
    }
    zc_to_scalar(w, rcm);

    zc_sinsemilla(&s, &G_NC_Q, msg, 1086, zc_sinsemilla_s);
    zc_pt_mul(&rr, rcm, &G_NC_R);
    zc_pt_add(cm, &s, &rr);

    memzero(msg, sizeof msg);
    memzero(pre, sizeof pre);
    memzero(psi, sizeof psi);
    memzero(rcm, sizeof rcm);
    memzero(w, sizeof w);
    memzero(&rr, sizeof rr);
    memzero(&s, sizeof s);
    return 0;
}

int zc_note_cmx(uint8_t cmx[32], const uint8_t recipient[43], uint64_t value,
                const uint8_t rho[32], const uint8_t rseed[32], uint8_t version)
{
    zc_pt cm;
    if (zc_note_commit(&cm, recipient, value, rho, rseed, version) != 0) return -1;
    zc_pt_extract(cmx, &cm);
    return 0;
}

/* ---- Poseidon ----------------------------------------------------------- */

/* P128Pow5T3: x^5 S-box, 4 + 4 full rounds around 56 partial rounds. */
static void pos_permute(zc_fe st[3])
{
    zc_fe t, ns[3];
    for (int r = 0; r < 64; r++) {
        int full = r < 4 || r >= 60;
        for (int i = 0; i < 3; i++) {
            memcpy(t.v, ZC_POSEIDON_RC_M[r][i], sizeof t.v);
            zc_fp_add(&st[i], &st[i], &t);
        }
        for (int i = 0; i < (full ? 3 : 1); i++) {
            zc_fe x2, x4;
            zc_fp_sqr(&x2, &st[i]);
            zc_fp_sqr(&x4, &x2);
            zc_fp_mul(&st[i], &x4, &st[i]);
        }
        for (int i = 0; i < 3; i++) {
            memset(&ns[i], 0, sizeof ns[i]);
            for (int j = 0; j < 3; j++) {
                memcpy(t.v, ZC_POSEIDON_MDS_M[i][j], sizeof t.v);
                zc_fp_mul(&t, &t, &st[j]);
                zc_fp_add(&ns[i], &ns[i], &t);
            }
        }
        memcpy(st, ns, sizeof ns);
    }
    memzero(&t, sizeof t);
    memzero(ns, sizeof ns);
}

void zc_poseidon2(uint8_t out[32], const uint8_t a[32], const uint8_t b[32])
{
    zc_fe st[3];
    zc_fp_from_bytes(&st[0], a);
    zc_fp_from_bytes(&st[1], b);
    /* ConstantLength<2>: capacity element 2 * 2^64. */
    zc_fp_set_u64(&st[2], 2);
    for (int i = 0; i < 64; i++) zc_fp_add(&st[2], &st[2], &st[2]);
    pos_permute(st);
    zc_fp_to_bytes(out, &st[0]);
    memzero(st, sizeof st);
}

void zc_nullifier(uint8_t nf[32], const uint8_t nk[32], const uint8_t rho[32],
                  const uint8_t psi[32], const zc_pt *cm)
{
    uint8_t k[32];
    zc_fe a, b;
    zc_pt t;
    zc_poseidon2(k, nk, rho);
    zc_fp_from_bytes(&a, k);
    zc_fp_from_bytes(&b, psi);
    zc_fp_add(&a, &a, &b);        /* in Fp; p < q, so it is already < q */
    zc_fp_to_bytes(k, &a);
    zc_pt_mul(&t, k, &G_K);
    zc_pt_add(&t, &t, cm);
    zc_pt_extract(nf, &t);
    memzero(k, sizeof k);
    memzero(&a, sizeof a);
    memzero(&b, sizeof b);
    memzero(&t, sizeof t);
}

/* ---- viewing keys ------------------------------------------------------- */

/* ivk = Extract_P(CommitIvk_rivk(Extract_P(ak), nk)); -1 if zero. */
static int commit_ivk(uint8_t ivk[32], const uint8_t ak[32], const uint8_t nk[32],
                      const uint8_t rivk[32])
{
    uint8_t msg[64] = {0};
    zc_pt s, r;
    memcpy(msg, ak, 32);
    msg[31] &= 0x7F;                                  /* x-coordinate, 255 bits */
    for (int i = 0; i < 255; i++) {
        int sb = i / 8, st = i % 8, db = (255 + i) / 8, dt = (255 + i) % 8;
        if (nk[sb] & (1 << st)) msg[db] |= (uint8_t)(1 << dt);
    }
    zc_sinsemilla(&s, &G_IVK_Q, msg, 510, zc_sinsemilla_s);
    zc_pt_mul(&r, rivk, &G_IVK_R);
    zc_pt_add(&s, &s, &r);
    zc_pt_extract(ivk, &s);
    memzero(msg, sizeof msg);
    memzero(&s, sizeof s);
    memzero(&r, sizeof r);
    return is_zero32(ivk) ? -1 : 0;
}

int zc_fvk_from_bytes(zc_fvk *f, const uint8_t fvk[96])
{
    uint8_t w[64], rivk_int[32], d82 = 0x82, d83 = 0x83;
    memset(f, 0, sizeof *f);
    if (is_zero32(fvk) || (fvk[31] & 0x80)) return -1;
    if (!zc_point_valid(fvk)) return -1;
    if (!zc_fp_canonical(fvk + 32) || !zc_fq_canonical(fvk + 64)) return -1;
    memcpy(f->ak, fvk, 32);
    memcpy(f->nk, fvk + 32, 32);
    memcpy(f->rivk, fvk + 64, 32);

    if (commit_ivk(f->ivk_ext, f->ak, f->nk, f->rivk) != 0) goto bad;
    zc_prf_expand(f->rivk, &d82, 1, f->ak, 32, f->nk, 32, w);
    memcpy(f->dk_ext, w, 32);
    memcpy(f->ovk_ext, w + 32, 32);

    /* ZIP 32 internal FVK: rivk_internal = ToScalar(PRF^expand(rivk, [0x83] || ak || nk)). */
    zc_prf_expand(f->rivk, &d83, 1, f->ak, 32, f->nk, 32, w);
    zc_to_scalar(w, rivk_int);
    if (commit_ivk(f->ivk_int, f->ak, f->nk, rivk_int) != 0) goto bad;
    zc_prf_expand(rivk_int, &d82, 1, f->ak, 32, f->nk, 32, w);
    memcpy(f->ovk_int, w + 32, 32);
    memzero(w, sizeof w);
    memzero(rivk_int, sizeof rivk_int);
    return 0;
bad:
    memzero(w, sizeof w);
    memzero(rivk_int, sizeof rivk_int);
    memzero(f, sizeof *f);
    return -1;
}

int zc_account_keys(const uint8_t seed[64], uint32_t coin_type, uint32_t account,
                    uint8_t ask[32], zc_fvk *f)
{
    uint8_t sk[32], fvk[96], w[64], d;
    zc_fe a;
    zc_pt ak;
    int rc = -1;
    if (account >= 0x80000000u) return -1;
    orchard_derive_account_sk(seed, coin_type, account, sk);

    d = 0x06;                                     /* ask = ToScalar(PRF(sk, 6)) */
    zc_prf_expand(sk, &d, 1, NULL, 0, NULL, 0, w);
    zc_fq_from_wide(&a, w);
    d = 0x07;                                     /* nk = ToBase(PRF(sk, 7)) */
    zc_prf_expand(sk, &d, 1, NULL, 0, NULL, 0, w);
    zc_to_base(w, fvk + 32);
    d = 0x08;                                     /* rivk = ToScalar(PRF(sk, 8)) */
    zc_prf_expand(sk, &d, 1, NULL, 0, NULL, 0, w);
    zc_to_scalar(w, fvk + 64);
    if (zc_fe_is_zero(&a)) goto out;

    /* ak = [ask] G, with ask negated if that makes the sign of ak's y even. */
    zc_pt_mul_fq(&ak, &a, &G_SPEND);
    zc_pt_encode(fvk, &ak);
    {
        zc_fe na;
        uint32_t odd = fvk[31] >> 7;
        zc_fq_neg(&na, &a);
        zc_fe_cmov(&a, &na, odd);
        fvk[31] &= 0x7F;                          /* the encoding of -ak */
        memzero(&na, sizeof na);
    }
    zc_fq_to_bytes(ask, &a);
    if (zc_fvk_from_bytes(f, fvk) != 0) {
        memzero(ask, 32);
        goto out;
    }
    rc = 0;
out:
    memzero(sk, sizeof sk);
    memzero(fvk, sizeof fvk);
    memzero(w, sizeof w);
    memzero(&a, sizeof a);
    memzero(&ak, sizeof ak);
    return rc;
}

static int pkd_matches(const uint8_t ivk[32], const zc_pt *gd, const uint8_t pkd[32])
{
    zc_pt pk;
    uint8_t enc[32];
    zc_pt_mul(&pk, ivk, gd);
    zc_pt_encode(enc, &pk);
    return memcmp(enc, pkd, 32) == 0;
}

int zc_fvk_scope(const zc_fvk *f, const uint8_t recipient[43])
{
    zc_pt gd;
    zc_diversify_hash(&gd, recipient);
    if (pkd_matches(f->ivk_ext, &gd, recipient + 11)) return 1;
    if (pkd_matches(f->ivk_int, &gd, recipient + 11)) return 2;
    return 0;
}

void zc_fvk_address_at(const zc_fvk *f, const uint8_t j[11], uint8_t out[43])
{
    zc_pt gd, pk;
    ff1_aes256_encrypt(f->dk_ext, j, out);        /* d = FF1-AES256_dk(j) */
    zc_diversify_hash(&gd, out);
    zc_pt_mul(&pk, f->ivk_ext, &gd);
    zc_pt_encode(out + 11, &pk);
}

void zc_fvk_default_address(const zc_fvk *f, uint8_t out[43])
{
    static const uint8_t j0[11];
    zc_fvk_address_at(f, j0, out);
}

/* FF1 over radix 2, n = 88, empty tweak, as ff1_aes256_encrypt: numeral 0
 * is bit 0 of byte 0 and the most significant bit of each 44-bit half. */
static uint64_t ff1_half(const uint8_t *d, int off)
{
    uint64_t v = 0;
    for (int i = 0; i < 44; i++) {
        if (d[(off + i) / 8] & (1 << ((off + i) % 8))) v |= 1ULL << (43 - i);
    }
    return v;
}

static void ff1_put_half(uint8_t *d, int off, uint64_t v)
{
    for (int i = 0; i < 44; i++) {
        uint8_t bit = (uint8_t)(1 << ((off + i) % 8));
        if (v & (1ULL << (43 - i))) d[(off + i) / 8] |= bit;
        else d[(off + i) / 8] &= (uint8_t)~bit;
    }
}

void zc_ff1_decrypt(const uint8_t key[32], const uint8_t in[11], uint8_t out[11])
{
    const uint64_t mask = (1ULL << 44) - 1;
    aes_ct_256_ctx ctx;
    uint8_t P[16] = {1, 2, 1, 0, 0, 2, 10, 44, 0, 0, 0, 88, 0, 0, 0, 0};
    uint8_t prf_p[16], R[16], S[16];
    aes_ct_256_keysched(key, &ctx);
    aes_ct_256_ecb_encrypt(&ctx, P, prf_p);

    /* Encryption round i maps (A, B) to (B, A + F_i(B)); undo it from the
     * last round down: B was the new A, and A = new B - F_i(B). */
    uint64_t a = ff1_half(in, 0), b = ff1_half(in, 44);
    for (int i = 9; i >= 0; i--) {
        uint64_t prev_b = a;
        memset(R, 0, sizeof R);
        R[9] = (uint8_t)i;
        for (int k = 0; k < 6; k++) R[10 + k] = (uint8_t)(prev_b >> (8 * (5 - k)));
        for (int k = 0; k < 16; k++) R[k] ^= prf_p[k];
        aes_ct_256_ecb_encrypt(&ctx, R, S);
        uint64_t y = 0;
        for (int k = 6; k < 12; k++) y = (y << 8) | S[k];
        y |= (uint64_t)(S[5] & 0x0F) << 48;
        y &= mask;
        a = (b - y) & mask;
        b = prev_b;
    }
    memcpy(out, in, 11);
    ff1_put_half(out, 0, a);
    ff1_put_half(out, 44, b);
    memzero(&ctx, sizeof ctx);
    memzero(prf_p, sizeof prf_p);
}

int zc_fvk_external_index(const zc_fvk *f, const uint8_t recipient[43], uint8_t j[11])
{
    uint8_t again[43];
    if (zc_fvk_scope(f, recipient) != 1) return 0;
    zc_ff1_decrypt(f->dk_ext, recipient, j);
    /* FF1 is a permutation, so this holds by construction; checked anyway,
     * since a wrong j here would put a wrong t-address on the screen. */
    zc_fvk_address_at(f, j, again);
    return memcmp(again, recipient, 43) == 0;
}

/* ---- RedPallas ------------------------------------------------------------ */

static void hstar(zc_fe *out, const uint8_t *a, size_t na, const uint8_t b[32],
                  const uint8_t msg[32])
{
    blake2b_state s;
    uint8_t h[64];
    blake2b_InitPersonal(&s, 64, "Zcash_RedPallasH", 16);
    blake2b_Update(&s, a, na);
    blake2b_Update(&s, b, 32);
    blake2b_Update(&s, msg, 32);
    blake2b_Final(&s, h, 64);
    zc_fq_from_wide(out, h);
    memzero(h, sizeof h);
    memzero(&s, sizeof s);
}

int zc_randomize_ak(uint8_t rk[32], const uint8_t ak[32], const uint8_t alpha[32])
{
    zc_pt a, t;
    if (!zc_pt_decode(&a, ak) || !zc_fq_canonical(alpha)) return -1;
    zc_pt_mul(&t, alpha, &G_SPEND);
    zc_pt_add(&a, &a, &t);
    zc_pt_encode(rk, &a);
    return 0;
}

int zc_redpallas_sign(const uint8_t ask[32], const uint8_t alpha[32], const uint8_t msg[32],
                      uint8_t sig[64], uint8_t rk[32])
{
    zc_fe sk, al, r, c;
    zc_pt R;
    uint8_t T[80], rb[32];
    if (!zc_fq_from_bytes(&sk, ask) || !zc_fq_from_bytes(&al, alpha)) return -1;
    zc_fq_add(&sk, &sk, &al);                      /* rsk = ask + alpha */
    zc_pt_mul_fq(&R, &sk, &G_SPEND);
    zc_pt_encode(rk, &R);

    zc_platform_random(T, sizeof T);
    hstar(&r, T, sizeof T, rk, msg);               /* r = H*(T || vk || M) */
    zc_pt_mul_fq(&R, &r, &G_SPEND);
    zc_pt_encode(rb, &R);
    hstar(&c, rb, 32, rk, msg);                    /* c = H*(R || vk || M) */
    zc_fq_mul(&c, &c, &sk);
    zc_fq_add(&r, &r, &c);                         /* S = r + c rsk */
    memcpy(sig, rb, 32);
    zc_fq_to_bytes(sig + 32, &r);

    memzero(&sk, sizeof sk);
    memzero(&al, sizeof al);
    memzero(&r, sizeof r);
    memzero(&c, sizeof c);
    memzero(&R, sizeof R);
    memzero(T, sizeof T);
    return 0;
}

int zc_redpallas_verify(const uint8_t rk[32], const uint8_t msg[32], const uint8_t sig[64])
{
    zc_pt vk, R, lhs, rhs;
    zc_fe c;
    uint8_t cb[32];
    if (!zc_pt_decode(&vk, rk) || !zc_pt_decode(&R, sig) || !zc_fq_canonical(sig + 32))
        return -1;
    hstar(&c, sig, 32, rk, msg);
    zc_fq_to_bytes(cb, &c);
    zc_pt_mul(&lhs, sig + 32, &G_SPEND);           /* [S] G */
    zc_pt_mul(&rhs, cb, &vk);                      /* [c] vk */
    zc_pt_add(&rhs, &rhs, &R);
    return zc_pt_eq(&lhs, &rhs) ? 0 : -1;
}

/* ---- note encryption ---------------------------------------------------- */

static const uint8_t ZERO_NONCE[12];

static void kdf_orchard(uint8_t key[32], const zc_pt *shared, const uint8_t epk[32])
{
    uint8_t ss[32];
    blake2b_state h;
    zc_pt_encode(ss, shared);
    blake2b_InitPersonal(&h, 32, "Zcash_OrchardKDF", 16);
    blake2b_Update(&h, ss, 32);
    blake2b_Update(&h, epk, 32);
    blake2b_Final(&h, key, 32);
    memzero(ss, sizeof ss);
    memzero(&h, sizeof h);
}

int zc_note_encrypt(uint8_t enc[580], uint8_t epk[32], const uint8_t recipient[43],
                    uint64_t value, const uint8_t rseed[32], const uint8_t rho[32],
                    const uint8_t memo[512], uint8_t version)
{
    static uint8_t pt[564];
    uint8_t esk[32], key[32];
    zc_pt pkd, gd, e, shared;
    if (!zc_pt_decode(&pkd, recipient + 11) || zc_pt_is_identity(&pkd)) return -1;
    zc_note_esk(esk, rseed, rho);
    if (is_zero32(esk)) return -1;
    zc_diversify_hash(&gd, recipient);
    zc_pt_mul(&e, esk, &gd);
    zc_pt_encode(epk, &e);
    zc_pt_mul(&shared, esk, &pkd);
    kdf_orchard(key, &shared, epk);

    pt[0] = version;                               /* lead byte 0x02 / 0x03 */
    memcpy(pt + 1, recipient, 11);
    for (int i = 0; i < 8; i++) pt[12 + i] = (uint8_t)(value >> (8 * i));
    memcpy(pt + 20, rseed, 32);
    memcpy(pt + 52, memo, 512);
    chacha20poly1305_encrypt(key, ZERO_NONCE, NULL, 0, pt, sizeof pt, enc, enc + 564);

    memzero(pt, sizeof pt);
    memzero(esk, sizeof esk);
    memzero(key, sizeof key);
    memzero(&shared, sizeof shared);
    return 0;
}

int zc_recover_with_ovk(const uint8_t ovk[32], const uint8_t cv_net[32], const uint8_t cmx[32],
                        const uint8_t epk[32], const uint8_t enc[580], const uint8_t out[80],
                        const uint8_t rho[32], uint8_t version,
                        uint8_t recipient[43], uint64_t *value, uint8_t memo[512])
{
    static uint8_t pt[564];
    uint8_t ock[32], op[64], key[32], esk2[32], cmx2[32], e2[32];
    zc_pt pkd, shared, gd, ep;
    blake2b_state h;
    int rc = -1;

    blake2b_InitPersonal(&h, 32, "Zcash_Orchardock", 16);
    blake2b_Update(&h, ovk, 32);
    blake2b_Update(&h, cv_net, 32);
    blake2b_Update(&h, cmx, 32);
    blake2b_Update(&h, epk, 32);
    blake2b_Final(&h, ock, 32);
    if (chacha20poly1305_decrypt(ock, ZERO_NONCE, NULL, 0, out, 64, out + 64, op) != 0) goto done;

    /* pk_d: a non-identity point; esk: a canonical, non-zero scalar. */
    if (!zc_pt_decode(&pkd, op) || zc_pt_is_identity(&pkd)) goto done;
    if (!zc_fq_canonical(op + 32) || is_zero32(op + 32)) goto done;

    zc_pt_mul(&shared, op + 32, &pkd);
    kdf_orchard(key, &shared, epk);
    if (chacha20poly1305_decrypt(key, ZERO_NONCE, NULL, 0, enc, 564, enc + 564, pt) != 0) goto done;

    if (pt[0] != version) goto done;              /* the pool's note version */
    memcpy(recipient, pt + 1, 11);
    memcpy(recipient + 11, op, 32);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | pt[12 + i];
    const uint8_t *rseed = pt + 20;

    /* RandomSeed::from_bytes (esk non-zero) and the ZIP 212 esk check. */
    zc_note_esk(esk2, rseed, rho);
    if (is_zero32(esk2)) goto done;
    {
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= esk2[i] ^ op[32 + i];
        if (diff) goto done;
    }
    /* check_note_validity: the note commits to cmx and esk gives epk. */
    if (zc_note_cmx(cmx2, recipient, v, rho, rseed, version) != 0) goto done;
    if (memcmp(cmx2, cmx, 32) != 0) goto done;
    zc_diversify_hash(&gd, recipient);
    zc_pt_mul(&ep, esk2, &gd);
    zc_pt_encode(e2, &ep);
    if (memcmp(e2, epk, 32) != 0) goto done;

    *value = v;
    memcpy(memo, pt + 52, 512);
    rc = 0;
done:
    memzero(ock, sizeof ock);
    memzero(op, sizeof op);
    memzero(key, sizeof key);
    memzero(pt, sizeof pt);
    memzero(esk2, sizeof esk2);
    memzero(&shared, sizeof shared);
    memzero(&h, sizeof h);
    return rc;
}

void zc_seed_fingerprint(const uint8_t seed[64], uint8_t out[32])
{
    blake2b_state s;
    uint8_t len = 64;
    blake2b_InitPersonal(&s, 32, "Zcash_HD_Seed_FP", 16);
    blake2b_Update(&s, &len, 1);
    blake2b_Update(&s, seed, 64);
    blake2b_Final(&s, out, 32);
    memzero(&s, sizeof s);
}

/* ---- self test ------------------------------------------------------------ */

int zc_crypto_selftest(void)
{
    /* Poseidon(0, 1) from zcash-test-vectors orchard_poseidon_hash. */
    static const uint8_t expect[32] = {
        0x83, 0x58, 0xd7, 0x11, 0xa0, 0x32, 0x9d, 0x38, 0xbe, 0xcd, 0x54, 0xfb, 0xa7,
        0xc2, 0x83, 0xed, 0x3e, 0x08, 0x9a, 0x39, 0xc9, 0x1b, 0x6a, 0x9d, 0x10, 0xef,
        0xb0, 0x2b, 0xc3, 0xf1, 0x2f, 0x06 };
    uint8_t a[32] = {0}, b[32] = {0}, h[32];
    if (zc_crypto_init() != 0) return -1;
    b[0] = 1;
    zc_poseidon2(h, a, b);
    if (memcmp(h, expect, 32) != 0) return -1;

    /* Recompute S table entries with our hash-to-curve: S(j) =
     * GroupHash("z.cash:SinsemillaS", I2LEOSP32(j)). */
    static const uint32_t js[3] = { 0, 517, 1023 };
    for (int i = 0; i < 3; i++) {
        uint8_t jb[4] = { (uint8_t)js[i], (uint8_t)(js[i] >> 8), 0, 0 }, x[32], y[32];
        zc_pt s;
        zc_fe fx, fy;
        zc_hash_to_curve(&s, "z.cash:SinsemillaS", jb, 4);
        zc_pt_affine(&fx, &fy, &s);
        zc_fp_to_bytes(x, &fx);
        zc_fp_to_bytes(y, &fy);
        if (memcmp(x, zc_sinsemilla_s + js[i] * 64, 32) != 0 ||
            memcmp(y, zc_sinsemilla_s + js[i] * 64 + 32, 32) != 0)
            return -1;
    }
    return 0;
}
