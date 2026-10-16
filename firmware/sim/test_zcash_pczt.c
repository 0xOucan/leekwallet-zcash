/* The PCZT signer against librustzcash-built vectors (vectors/pczt_vectors.h).
 *
 * For each vector, through the C ABI the firmware will use:
 *   parse -> check (every commitment recomputed on the device) -> compare
 *   the summary the screen would show -> sign -> verify each signature the
 *   way a companion applies it.
 * Tampered vectors must be refused with exactly the expected reason, and a
 * refused PCZT must not be signable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bip39.h"
#include "memzero.h"
#include "zsign_rust.h"
#include "../../vectors/pczt_vectors.h"

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

static zs_summary sum; /* ~12 KB: static, as on the device */

static void run(const zv_pczt_vector *v)
{
    size_t len;
    uint8_t *bytes = slurp(v->file, &len);
    expect(bytes != NULL, v->name, "vector file readable");
    if (!bytes) return;

    uint8_t seed[64];
    mnemonic_to_seed(v->mnemonic, v->passphrase, seed, NULL);

    int32_t err = 0;
    zs_ctx *ctx = zs_pczt_new(bytes, len, &err);
    int32_t rc = ctx ? zs_pczt_check(ctx, seed, v->account, v->network, &sum) : err;

    if (v->expect != 0) {
        char what[96];
        snprintf(what, sizeof what, "refused with %d (got %d)", v->expect, rc);
        expect(rc == v->expect, v->name, what);
        if (ctx) {
            zs_sig sigs[ZS_MAX_SIGS];
            uint32_t n = 0;
            expect(zs_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
                       == ZS_E_NOT_CHECKED, v->name, "refused PCZT cannot be signed");
        }
        goto out;
    }

    expect(rc == 0, v->name, "check passes");
    if (rc != 0) { printf("    (rc=%d)\n", rc); goto out; }

    char sh[65];
    hex32(sum.sighash, sh);
    expect(strcmp(sh, v->sighash) == 0, v->name, "sighash matches librustzcash");
    expect(sum.branch_id == v->branch_id, v->name, "branch id");
    expect(sum.fee == v->fee, v->name, "fee computed on device");
    expect(sum.n_outputs == v->n_outputs, v->name, "output count");
    /* Action order is shuffled by the builder (for privacy), so match by content. */
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

    /* Transparent inputs: shown in order, each with its path and the
     * sighash librustzcash computes for it. */
    expect(sum.n_tinputs == v->n_tinputs, v->name, "transparent input count");
    uint64_t t_in = 0;
    for (int i = 0; i < v->n_tinputs && i < sum.n_tinputs; i++) {
        const zv_pczt_tinput *e = &v->tinputs[i];
        const zs_tinput *t = &sum.tinputs[i];
        hex32(t->sighash, sh);
        expect(t->value == e->value && t->scope == e->scope && t->index == e->index,
               v->name, "transparent input value and path");
        expect(strcmp(t->address, e->address) == 0, v->name, "transparent input address");
        expect(strcmp(sh, e->sighash) == 0, v->name, "transparent sighash matches librustzcash");
        t_in += e->value;
    }
    expect(sum.t_in == t_in, v->name, "transparent input total");

    zs_sig sigs[ZS_MAX_SIGS];
    uint32_t n = 0;
    rc = zs_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n);
    expect(rc == 0 && n >= 1, v->name, "signs");
    uint32_t n_t = 0;
    for (uint32_t i = 0; i < n; i++) {
        expect(zs_pczt_verify_sig(ctx, &sigs[i]) == 0, v->name, "signature verifies against rk");
        if (sigs[i].pool != ZS_POOL_TRANSPARENT) continue;
        expect(sigs[i].index == n_t, v->name, "one transparent signature per input, in order");
        n_t++;
        zs_sig bad = sigs[i];
        bad.sig[40] ^= 1;
        expect(zs_pczt_verify_sig(ctx, &bad) != 0, v->name, "corrupted transparent signature rejected");
        bad = sigs[i];
        bad.pubkey[10] ^= 1;
        expect(zs_pczt_verify_sig(ctx, &bad) != 0, v->name, "transparent signature under another key rejected");
        bad = sigs[i];
        bad.index = 99;
        expect(zs_pczt_verify_sig(ctx, &bad) != 0, v->name, "transparent signature for a missing input rejected");
    }
    expect(n_t == v->n_tinputs, v->name, "every transparent input signed");
    if (n) {
        zs_sig bad = sigs[0];
        bad.sig[5] ^= 1;
        expect(zs_pczt_verify_sig(ctx, &bad) != 0, v->name, "corrupted signature rejected");
    }
    expect(zs_pczt_sign(ctx, seed, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
               == ZS_E_NOT_CHECKED, v->name, "one check authorises one signing");

    /* The wrong seed after a passing check must not sign. */
    rc = zs_pczt_check(ctx, seed, v->account, v->network, &sum);
    uint8_t other[64];
    memcpy(other, seed, 64);
    other[0] ^= 1;
    expect(zs_pczt_sign(ctx, other, v->account, v->network, sigs, ZS_MAX_SIGS, &n)
               == ZS_E_NOT_CHECKED, v->name, "signing with a different seed refused");
    memzero(other, sizeof other);

out:
    zs_pczt_free(ctx);
    memzero(seed, sizeof seed);
    free(bytes);
}

int main(void)
{
    printf("== test_zcash_pczt\n== %zu PCZT vectors: check, summary, sign, refusals\n",
           ZV_PCZT_COUNT);
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) run(&ZV_PCZT[i]);
    if (fails) {
        printf("\nFAILED (%d failures)\n", fails);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
