/* LeekWallet Zcash, C signer: host-only cross-check of zc_pallas.c and
 * zc_crypto.c against the vendored libzcash-ironwood-c, which implements the
 * same primitives with entirely different arithmetic (bit-serial reduction,
 * Jacobian/projective ladders of its own). Random inputs from a fixed seed;
 * every disagreement is a bug in one of the two.
 *
 * Not built for the device (sim/Makefile only): the vendored arithmetic is
 * ~100x slower and is not used on the signing path.
 */
#include <stdio.h>
#include <string.h>

#include "bignum.h"
#include "orchard.h"
#include "pallas.h"
#include "redpallas.h"
#include "zc_crypto.h"
#include "zc_pallas.h"

extern const uint8_t zc_sinsemilla_s[1024 * 64];

int32_t zc_xcheck(uint32_t iters, uint32_t seed);

static uint64_t s_rng;

static uint64_t rnd64(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static void rnd(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd64();
}

static bool v_lookup(uint32_t index, uint8_t out[64], void *ctx)
{
    (void)ctx;
    if (index >= 1024) return false;
    memcpy(out, zc_sinsemilla_s + (size_t)index * 64, 64);
    return true;
}

/* The vendored point, encoded as pasta_curves does. */
static void v_encode(uint8_t out[32], const pallas_point *p)
{
    if (p->infinity) {
        memset(out, 0, 32);
        return;
    }
    bignum256 t;
    bn_copy(&p->x, &t);
    bn_mod(&t, pallas_p());
    bn_write_le(&t, out);
    bn_copy(&p->y, &t);
    bn_mod(&t, pallas_p());
    if (t.val[0] & 1u) out[31] |= 0x80;
}

static int fails;

static void cmp(const char *what, uint32_t i, const uint8_t *a, const uint8_t *b, size_t n)
{
    if (memcmp(a, b, n) != 0) {
        if (fails < 10) printf("  xcheck FAIL %s (iteration %u)\n", what, i);
        fails++;
    }
}

int32_t zc_xcheck(uint32_t iters, uint32_t seed)
{
    s_rng = 0x9E3779B97F4A7C15ull ^ seed;
    fails = 0;
    if (zc_crypto_init() != 0) return -1;
    pallas_init();
    pallas_set_sinsemilla_lookup(v_lookup, NULL);

    for (uint32_t i = 0; i < iters; i++) {
        uint8_t k[32], m[16], a[32], b[32], d[11], rho[32], rseed[32], memo[512];
        pallas_point vp, vq;
        zc_pt p, q;
        bignum256 kb;

        /* hash_to_curve on a random message, then [k]P for a random 256-bit k. */
        rnd(m, sizeof m);
        pallas_hash_to_curve(&vp, "z.cash:test", m, sizeof m);
        zc_hash_to_curve(&p, "z.cash:test", m, sizeof m);
        v_encode(a, &vp);
        zc_pt_encode(b, &p);
        cmp("hash_to_curve", i, a, b, 32);

        rnd(k, 32);
        bn_read_le(k, &kb);
        pallas_point_mul(&vq, &kb, &vp);
        zc_pt_mul(&q, k, &p);
        v_encode(a, &vq);
        zc_pt_encode(b, &q);
        cmp("point_mul", i, a, b, 32);

        /* Decoding a valid encoding round-trips; a random string decodes in
         * both or in neither. */
        zc_pt r;
        if (!zc_pt_decode(&r, a) || !zc_pt_eq(&r, &q)) cmp("decode", i, a, (const uint8_t *)"", 1);
        rnd(a, 32);
        a[31] &= 0x7F;
        {
            zc_fe x;
            int canon = zc_fp_from_bytes(&x, a);
            int ours = zc_pt_decode(&r, a);
            if (canon) {
                /* Vendored decode: square root of x^3 + 5 must exist iff ours. */
                bignum256 xb, y2, t, five, y;
                bn_read_le(a, &xb);
                fp_sqr(&t, &xb);
                fp_mul(&y2, &t, &xb);
                bn_read_uint32(5, &five);
                fp_add(&y2, &y2, &five);
                int theirs = fp_sqrt(&y, &y2);
                uint8_t o = (uint8_t)ours, t2 = (uint8_t)(theirs != 0);
                cmp("decode/sqrt", i, &o, &t2, 1);
            }
        }

        /* Key components: ToScalar / ToBase against orchard_derive_keys, and
         * ak = [ask] G (sign-normalized) against redpallas_derive_ak. */
        uint8_t sk[32], ask[32], nk[32], rivk[32], x[32], w[64], dd;
        rnd(sk, 32);
        orchard_derive_keys(sk, ask, nk, rivk);
        dd = 0x06;
        zc_prf_expand(sk, &dd, 1, NULL, 0, NULL, 0, w);
        zc_to_scalar(w, x);
        cmp("to_scalar (ask)", i, ask, x, 32);
        dd = 0x07;
        zc_prf_expand(sk, &dd, 1, NULL, 0, NULL, 0, w);
        zc_to_base(w, x);
        cmp("to_base (nk)", i, nk, x, 32);
        redpallas_derive_ak(ask, a);
        {
            zc_pt g, ak;
            zc_hash_to_curve(&g, "z.cash:Orchard", (const uint8_t *)"G", 1);
            zc_pt_mul(&ak, ask, &g);
            zc_pt_encode(b, &ak);
            b[31] &= 0x7F;                     /* -ak has the same x, even y */
            cmp("ak", i, a, b, 32);
        }

        /* Note commitments, both versions, and note encryption. */
        rnd(d, 11);
        rnd(rho, 32);
        rho[31] &= 0x3F;
        rnd(rseed, 32);
        rnd(memo, sizeof memo);
        uint64_t v = rnd64() % 2100000000000000ull;
        {
            zc_pt gd, pk;
            uint8_t rec[43];
            zc_diversify_hash(&gd, d);
            zc_pt_mul(&pk, k, &gd);
            memcpy(rec, d, 11);
            zc_pt_encode(rec + 11, &pk);
            for (uint8_t ver = 2; ver <= 3; ver++) {
                uint8_t c1[32], c2[32], e1[580], e2[580], p1[32], p2[32];
                orchard_compute_cmx(d, rec + 11, v, rho, rseed, ver, c1);
                zc_note_cmx(c2, rec, v, rho, rseed, ver);
                cmp(ver == 2 ? "cmx v2" : "cmx v3", i, c1, c2, 32);
                if (orchard_compute_enc_ciphertext_from_rseed(d, rec + 11, v, rseed, rho, memo,
                                                              ver, e1, p1) == 0 &&
                    zc_note_encrypt(e2, p2, rec, v, rseed, rho, memo, ver) == 0) {
                    cmp("epk", i, p1, p2, 32);
                    cmp("enc_ciphertext", i, e1, e2, 580);
                } else {
                    cmp("note encryption ran", i, (const uint8_t *)"a", (const uint8_t *)"b", 1);
                }
            }
        }
    }
    pallas_set_sinsemilla_lookup(NULL, NULL);
    return fails;
}
