/* The C signer (zsign-c) against the same librustzcash-built vectors as
 * test_zcash_pczt.c, through the zc_* API: parse -> check -> summary -> sign
 * -> verify each signature as a companion would. Tampered vectors must be
 * refused with exactly the expected code, and a refused PCZT must not be
 * signable. Also: the Poseidon known answer, the key vectors through
 * zc_account_derive, and a tampered-buffer-after-check refusal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bip39.h"
#include "memzero.h"
#include "zsign_c.h"
#include "../../vectors/pczt_vectors.h"
#include "../../vectors/keys_vectors.h"

static int fails;

static void expect(int ok, const char *vec, const char *what)
{
    if (!ok) {
        printf("  FAIL %s: %s\n", vec, what);
        fails++;
    }
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static void hex32(const uint8_t *b, char out[65])
{
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static zs_summary sum;

static void run(const zv_pczt_vector *v)
{
    size_t len;
    uint8_t *bytes = slurp(v->file, &len);
    expect(bytes != NULL, v->name, "vector file readable");
    if (!bytes) return;

    uint8_t seed[64];
    mnemonic_to_seed(v->mnemonic, v->passphrase, seed, NULL);

    double t0 = now();
    int32_t err = 0;
    zc_ctx *ctx = zc_pczt_new(bytes, len, &err);
    int32_t rc = ctx ? zc_pczt_check(ctx, seed, v->account, v->network, &sum) : err;
    printf("  %-28s rc=%3d  check %.2fs\n", v->name, rc, now() - t0);

    if (v->expect != 0) {
        char what[96];
        snprintf(what, sizeof what, "refused with %d (got %d)", v->expect, rc);
        expect(rc == v->expect, v->name, what);
        if (ctx) {
            zs_sig sigs[ZS_MAX_SIGS];
            uint32_t n = 0;
            expect(zc_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
                       == ZS_E_NOT_CHECKED, v->name, "refused PCZT cannot be signed");
        }
        goto out;
    }

    expect(rc == 0, v->name, "check passes");
    if (rc != 0) goto out;

    char sh[65];
    hex32(sum.sighash, sh);
    expect(strcmp(sh, v->sighash) == 0, v->name, "sighash matches librustzcash");
    expect(sum.branch_id == v->branch_id, v->name, "branch id");
    expect(sum.fee == v->fee, v->name, "fee computed on device");
    expect(sum.n_outputs == v->n_outputs, v->name, "output count");
    for (int i = 0; i < v->n_outputs; i++) {
        const zv_pczt_output *e = &v->outputs[i];
        const zs_output *o = NULL;
        for (int j = 0; j < sum.n_outputs; j++) {
            const zs_output *c = &sum.outputs[j];
            if (c->pool == e->pool && c->kind == e->kind && c->value == e->value &&
                strcmp(c->address, e->address) == 0) { o = c; break; }
        }
        expect(o != NULL, v->name, "expected output shown (pool, kind, value, address)");
        if (o && e->memo[0]) {
            expect(o->memo_kind == ZS_MEMO_TEXT && o->memo_len == strlen(e->memo) &&
                   memcmp(o->memo, e->memo, o->memo_len) == 0, v->name, "memo recovered via own OVK");
        }
    }

    zs_sig sigs[ZS_MAX_SIGS];
    uint32_t n = 0;
    t0 = now();
    rc = zc_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n);
    printf("  %-28s sign %.2fs (%u sigs)\n", "", now() - t0, n);
    expect(rc == 0 && n >= 1, v->name, "signs");
    for (uint32_t i = 0; i < n; i++)
        expect(zc_pczt_verify_sig(ctx, &sigs[i]) == 0, v->name, "signature verifies against rk");

    /* Transparent inputs: shown in order with path, address and the sighash
     * librustzcash computes; one low-S signature each. */
    expect(sum.n_tinputs == v->n_tinputs, v->name, "transparent input count");
    for (int i = 0; i < v->n_tinputs && i < sum.n_tinputs; i++) {
        const zv_pczt_tinput *e = &v->tinputs[i];
        const zs_tinput *t = &sum.tinputs[i];
        hex32(t->sighash, sh);
        expect(t->value == e->value && t->scope == e->scope && t->index == e->index &&
               strcmp(t->address, e->address) == 0, v->name, "transparent input shown");
        expect(strcmp(sh, e->sighash) == 0, v->name, "transparent sighash matches librustzcash");
    }
    uint32_t n_t = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (sigs[i].pool != ZS_POOL_TRANSPARENT) continue;
        expect(sigs[i].index == n_t++, v->name, "transparent signatures in input order");
        /* The same signature with s replaced by n - s: valid ECDSA, but
         * high S, which the network's policy and libsecp256k1 refuse. */
        static const uint8_t order[32] = {
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,
            0xBA,0xAE,0xDC,0xE6,0xAF,0x48,0xA0,0x3B,0xBF,0xD2,0x5E,0x8C,0xD0,0x36,0x41,0x41 };
        zs_sig high = sigs[i];
        int borrow = 0;
        for (int k = 31; k >= 0; k--) {
            int d = order[k] - sigs[i].sig[32 + k] - borrow;
            borrow = d < 0;
            high.sig[32 + k] = (uint8_t)(d + (borrow ? 256 : 0));
        }
        expect(zc_pczt_verify_sig(ctx, &high) != 0, v->name, "high-S transparent signature rejected");
        zs_sig bad = sigs[i];
        bad.pubkey[7] ^= 1;
        expect(zc_pczt_verify_sig(ctx, &bad) != 0, v->name, "transparent signature under another key rejected");
    }
    expect(n_t == v->n_tinputs, v->name, "every transparent input signed");
    if (n) {
        zs_sig bad = sigs[0];
        bad.sig[5] ^= 1;
        expect(zc_pczt_verify_sig(ctx, &bad) != 0, v->name, "corrupted signature rejected");
    }
    expect(zc_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
               == ZS_E_NOT_CHECKED, v->name, "one check authorises one signing");

    /* The wrong seed after a passing check must not sign. */
    rc = zc_pczt_check(ctx, seed, v->account, v->network, &sum);
    uint8_t other[64];
    memcpy(other, seed, 64);
    other[0] ^= 1;
    expect(zc_pczt_sign(ctx, other, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
               == ZS_E_NOT_CHECKED, v->name, "signing with a different seed refused");
    memzero(other, sizeof other);

    /* Bytes changed after the check (TOCTOU): refused. */
    rc = zc_pczt_check(ctx, seed, v->account, v->network, &sum);
    bytes[len - 1] ^= 1;
    expect(zc_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
               == ZS_E_NOT_CHECKED, v->name, "buffer changed after check refused");
    bytes[len - 1] ^= 1;

    /* Static context: a second open context is refused until freed. */
    expect(zc_pczt_new(bytes, len, &err) == NULL && err == ZS_E_INTERNAL, v->name,
           "single static context");

out:
    zc_pczt_free(ctx);
    memzero(seed, sizeof seed);
    free(bytes);
}

static void keys(void)
{
    int ok = 0;
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const zv_key_vector *v = &ZV_KEYS[i];
        /* The vector's BIP39 seed and transparent half (computed by
           librustzcash) go in as they are: this checks zsign-c alone. */
        zs_account a, o;
        int32_t rc = zc_account_derive(v->seed, v->account, v->network,
                                       v->transparent_account_pubkey,
                                       v->transparent_receiver, &a);
        int32_t ro = zc_account_derive(v->seed, v->account, v->network, NULL, NULL, &o);
        int good = rc == ZS_OK && ro == ZS_OK &&
                   memcmp(a.seed_fingerprint, v->seed_fingerprint, 32) == 0 &&
                   memcmp(a.orchard_fvk, v->orchard_fvk, 96) == 0 &&
                   memcmp(a.orchard_address, v->orchard_address_raw, 43) == 0 &&
                   strcmp(a.ufvk, v->ufvk) == 0 && strcmp(a.ua, v->ua) == 0 &&
                   strcmp(o.ua, v->ua_orchard_only) == 0;
        expect(good, v->name, "zc_account_derive: seed fp, FVK, address, UFVK, UA");
        ok += good;
        if (i == 0) {
            /* Mutation check: a different seed must not reproduce the vector. */
            uint8_t seed[64];
            memcpy(seed, v->seed, 64);
            seed[0] ^= 1;
            zc_account_derive(seed, v->account, v->network, NULL, NULL, &o);
            expect(memcmp(o.orchard_fvk, v->orchard_fvk, 96) != 0, v->name,
                   "mutated seed changes the FVK");
            memzero(seed, sizeof seed);
        }
    }
    printf("  key vectors: %d/%zu\n", ok, ZV_KEYS_COUNT);
}

/* Host-only (zc_xcheck.c): our Pallas arithmetic against the vendored one. */
int32_t zc_xcheck(uint32_t iters, uint32_t seed);

int main(void)
{
    printf("== test_zcash_pczt_c\n");
    expect(zc_selftest() == 0, "selftest", "Poseidon known answer + Sinsemilla table");
    double t0 = now();
    int32_t x = zc_xcheck(4, 1);
    printf("== cross-check against the vendored arithmetic: %d mismatches (%.1fs)\n", x,
           now() - t0);
    expect(x == 0, "xcheck", "hash-to-curve, [k]P, decode, keys, cmx, note encryption");
    printf("== %zu PCZT vectors through zsign-c\n", ZV_PCZT_COUNT);
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) run(&ZV_PCZT[i]);
    keys();
    if (fails) {
        printf("\nFAILED (%d failures)\n", fails);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
