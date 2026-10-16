/* Differential test: the Rust signer (zsign-rust, zs_*) against the C signer
 * (zsign-c, zc_*), which share no code.
 *
 * 1. Every PCZT vector through both: the same verdict and refusal code, and
 *    on acceptance the same summary byte for byte (fee, outputs, memos,
 *    sighash, counts). Each backend's signatures must verify under the other.
 * 2. Deterministic single-byte mutations of every vector (a random byte
 *    replaced, a bit flipped, or a byte nudged by +-1, so varints, tags and
 *    lengths are hit as often as payload): both backends must reach the same
 *    verdict with the same refusal code, and accepted mutations must give
 *    identical summaries. Two thirds of the positions avoid the witness auth
 *    paths (43% of these files, and never in the sighash), and every 8th
 *    accepted mutation is also signed and cross-verified. Refusal vectors get
 *    an eighth as many mutations: they mostly probe the parsers.
 * 3. On the compact (v2) vectors, mutations aimed at the output fields the
 *    device resolves itself (memo, recipient, rseed, epk, rcv, user_address):
 *    the backends agree, and an accepted one always changes the sighash.
 * 4. Signatures over a compact PCZT with a changed memo do not verify for
 *    the original.
 *
 * ZSIGN_DIFF_N (mutations per valid vector, default 600) and ZSIGN_DIFF_SEED
 * scale the run for longer fuzzing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bip39.h"
#include "memzero.h"
#include "zsign_c.h"
#include "zsign_rust.h"
#include "pczt_reader.h"   /* only to locate the witness paths */
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

static zs_summary sr, sc;

typedef struct {
    int32_t rc_r, rc_c;
    zs_sig sig_r[ZS_MAX_SIGS], sig_c[ZS_MAX_SIGS];
    uint32_t n_r, n_c;
} result;

/* Both backends on one buffer: verdicts, summaries, and (when accepted and
 * asked to) signatures, each verified by the other backend. */
static void run_both(const uint8_t *b, size_t len, const uint8_t seed[64], uint32_t account,
                     uint8_t net, int sign, result *res)
{
    int32_t e;
    memset(res, 0, sizeof *res);
    zs_ctx *r = zs_pczt_new(b, len, &e);
    res->rc_r = r ? zs_pczt_check(r, seed, account, net, &sr) : e;
    zc_ctx *c = zc_pczt_new(b, len, &e);
    res->rc_c = c ? zc_pczt_check(c, seed, account, net, &sc) : e;

    if (sign && res->rc_r == 0 && res->rc_c == 0) {
        int32_t a = zs_pczt_sign(r, seed, account, net, res->sig_r, ZS_MAX_SIGS, &res->n_r);
        int32_t z = zc_pczt_sign(c, seed, account, net, res->sig_c, ZS_MAX_SIGS, &res->n_c);
        if (a != z) res->rc_c = 1000 + z;          /* surfaced as a mismatch */
        if (a == 0 && z == 0) {
            int bad = res->n_r != res->n_c;
            for (uint32_t i = 0; !bad && i < res->n_r; i++) {
                bad |= res->sig_r[i].pool != res->sig_c[i].pool ||
                       res->sig_r[i].index != res->sig_c[i].index;
                bad |= zc_pczt_verify_sig(c, &res->sig_r[i]) != 0;   /* Rust sig, C verifies */
                bad |= zs_pczt_verify_sig(r, &res->sig_c[i]) != 0;   /* C sig, Rust verifies */
                /* RFC 6979 in both (libsecp256k1, trezor-crypto): the same
                 * transparent signature, byte for byte. */
                if (res->sig_r[i].pool == ZS_POOL_TRANSPARENT)
                    bad |= memcmp(res->sig_r[i].sig, res->sig_c[i].sig, 64) != 0 ||
                           memcmp(res->sig_r[i].pubkey, res->sig_c[i].pubkey, 33) != 0;
            }
            if (bad) res->rc_c = 2000;
        }
    }
    zs_pczt_free(r);
    zc_pczt_free(c);
}

static int summaries_equal(void)
{
    return memcmp(&sr, &sc, sizeof sr) == 0;
}

static void explain_summary(void)
{
    printf("    rust: fee %llu in %llu n_out %u spends %u dummies %u warn %u\n",
           (unsigned long long)sr.fee, (unsigned long long)sr.total_in, sr.n_outputs,
           sr.n_spends, sr.n_dummies, sr.warnings);
    printf("    c:    fee %llu in %llu n_out %u spends %u dummies %u warn %u\n",
           (unsigned long long)sc.fee, (unsigned long long)sc.total_in, sc.n_outputs,
           sc.n_spends, sc.n_dummies, sc.warnings);
    printf("    sighash %s\n", memcmp(sr.sighash, sc.sighash, 32) ? "DIFFERS" : "equal");
    for (int i = 0; i < ZS_MAX_OUTPUTS; i++)
        if (memcmp(&sr.outputs[i], &sc.outputs[i], sizeof sr.outputs[i]))
            printf("    output %d differs: kind %u/%u value %llu/%llu memo %u/%u\n", i,
                   sr.outputs[i].kind, sc.outputs[i].kind,
                   (unsigned long long)sr.outputs[i].value,
                   (unsigned long long)sc.outputs[i].value, sr.outputs[i].memo_kind,
                   sc.outputs[i].memo_kind);
}

static uint64_t s_rng;

static uint64_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return s_rng;
}

static const zv_pczt_vector *vector_named(const char *name, size_t *idx)
{
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++)
        if (strcmp(ZV_PCZT[i].name, name) == 0) {
            *idx = i;
            return &ZV_PCZT[i];
        }
    return NULL;
}

/* One byte of `b` replaced as in the random pass. */
static uint8_t mutate_byte(uint8_t old)
{
    uint8_t nb;
    uint64_t kind = rnd() % 10;
    if (kind < 6) {
        do nb = (uint8_t)rnd(); while (nb == old);
    } else if (kind < 9) {
        nb = old ^ (uint8_t)(1u << (rnd() % 8));
    } else {
        nb = (uint8_t)(old + ((rnd() & 1) ? 1 : 0xFF));
    }
    return nb;
}

/* 3. Compact vectors, aimed at the output fields the device resolves itself
 * (memo plaintext, recipient, rseed, epk, rcv, user_address), which random
 * positions rarely hit. Besides agreement between the backends: an accepted
 * mutation must change the sighash. Nothing the compact view leaves out may
 * let the device show one output and sign for another. */
static void compact_fields(uint8_t seeds[][64], long per_field)
{
    static zc_pczt layout;
    static result res;
    long runs = 0, acc = 0, bad = 0;
    printf("== compact output fields: %ld mutations per field per action\n", per_field);
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) {
        const zv_pczt_vector *v = &ZV_PCZT[i];
        if (v->expect != 0) continue;
        size_t len;
        uint8_t *b = slurp(v->file, &len);
        if (!b || zc_pczt_read(b, len, &layout) != ZC_R_OK) {
            expect(0, v->name, "vector readable for field mutation");
            free(b);
            continue;
        }
        int compact = 0;
        for (int a = 0; a < layout.n_actions; a++)
            compact |= layout.actions[a].enc_kind == ZC_ENC_MEMO;
        if (!compact) {
            free(b);
            continue;
        }
        run_both(b, len, seeds[i], v->account, v->network, 0, &res);
        uint8_t base[32];
        memcpy(base, sr.sighash, 32);
        long runs_here = 0, acc_here = 0;
        for (int a = 0; a < layout.n_actions; a++) {
            const zc_action *x = &layout.actions[a];
            struct { const uint8_t *p; size_t n; } f[] = {
                { x->enc_kind == ZC_ENC_MEMO ? x->enc.p : NULL, x->enc.len },
                { x->o_recipient, 43 }, { x->o_rseed, 32 }, { x->epk, 32 },
                { x->rcv, 32 }, { x->user_address.p, x->user_address.len },
            };
            for (size_t k = 0; k < sizeof f / sizeof f[0]; k++) {
                if (!f[k].p || !f[k].n) continue;
                for (long m = 0; m < per_field; m++) {
                    size_t pos = (size_t)(f[k].p - b) + (size_t)(rnd() % f[k].n);
                    uint8_t old = b[pos], nb = mutate_byte(old);
                    b[pos] = nb;
                    run_both(b, len, seeds[i], v->account, v->network, m % 4 == 0, &res);
                    runs_here++;
                    if (res.rc_r != res.rc_c) {
                        bad++;
                        expect(0, v->name, "field mutation: same verdict and code");
                        printf("    action %d field %zu byte %zu %02x->%02x: rust %d, c %d\n", a,
                               k, pos, old, nb, res.rc_r, res.rc_c);
                    } else if (res.rc_r == 0) {
                        acc_here++;
                        if (!summaries_equal()) {
                            bad++;
                            expect(0, v->name, "field mutation: identical summaries");
                            explain_summary();
                        }
                        if (memcmp(sr.sighash, base, 32) == 0) {
                            bad++;
                            expect(0, v->name, "accepted field mutation changes the sighash");
                            printf("    action %d field %zu byte %zu %02x->%02x\n", a, k, pos,
                                   old, nb);
                        }
                    }
                    b[pos] = old;
                }
            }
        }
        printf("  %-38s %4ld mutations, %4ld accepted by both\n", v->name, runs_here, acc_here);
        runs += runs_here;
        acc += acc_here;
        free(b);
    }
    printf("  total %ld: %ld accepted, %ld failures\n", runs, acc, bad);
}

/* 4. A changed memo is a different transaction, not a refusal: the compact
 * view carries nothing that commits to the memo but the ciphertext the device
 * rebuilds from it. So check what makes that safe: the device shows the new
 * memo (the vector's summary), and neither backend's signature over it
 * verifies for the original compact PCZT. */
static void compact_rebinding(uint8_t seeds[][64])
{
    static result res;
    size_t io, ic;
    const zv_pczt_vector *orig = vector_named("iw_send_shielded_compact", &io);
    const zv_pczt_vector *chg = vector_named("iw_send_shielded_compact_memo_changed", &ic);
    printf("== compact memo rebinding\n");
    expect(orig && chg, "compact_rebinding", "both vectors present");
    if (!orig || !chg) return;
    expect(strcmp(orig->outputs[0].memo, chg->outputs[0].memo) != 0, chg->name,
           "the vector shows a different memo");
    expect(strcmp(orig->sighash, chg->sighash) != 0, chg->name, "and has a different sighash");

    size_t len;
    uint8_t *b = slurp(chg->file, &len);
    expect(b != NULL, chg->name, "vector file readable");
    if (!b) return;
    run_both(b, len, seeds[ic], chg->account, chg->network, 1, &res);
    free(b);
    expect(res.rc_r == 0 && res.rc_c == 0 && res.n_r >= 1, chg->name, "both backends sign it");

    b = slurp(orig->file, &len);
    expect(b != NULL, orig->name, "vector file readable");
    if (!b) return;
    int32_t e;
    int verified = 0;
    zs_ctx *r = zs_pczt_new(b, len, &e);
    expect(r && zs_pczt_check(r, seeds[io], orig->account, orig->network, &sr) == 0, orig->name,
           "rust accepts the original");
    for (uint32_t i = 0; r && i < res.n_r; i++) {
        verified += zs_pczt_verify_sig(r, &res.sig_r[i]) == 0;
        verified += zs_pczt_verify_sig(r, &res.sig_c[i]) == 0;
    }
    zs_pczt_free(r);
    zc_ctx *c = zc_pczt_new(b, len, &e);
    expect(c && zc_pczt_check(c, seeds[io], orig->account, orig->network, &sc) == 0, orig->name,
           "c accepts the original");
    for (uint32_t i = 0; c && i < res.n_c; i++) {
        verified += zc_pczt_verify_sig(c, &res.sig_r[i]) == 0;
        verified += zc_pczt_verify_sig(c, &res.sig_c[i]) == 0;
    }
    zc_pczt_free(c);
    free(b);
    printf("  %u+%u signatures over the changed memo, %d verify for the original\n", res.n_r,
           res.n_c, verified);
    expect(verified == 0, chg->name, "no signature over the changed memo verifies for the original");
}

/* 5. Every byte of the transparent inputs of each valid vector that has
 * them, three mutations each (from the first prevout to the end of the last
 * input's derivation path): the backends agree, every accepted mutation
 * changes the sighash, and every one that is signed gives byte-identical
 * transparent signatures. */
static void transparent_fields(uint8_t seeds[][64])
{
    static zc_pczt layout;
    static result res;
    long total = 0, accepted = 0, mismatch = 0;
    printf("== transparent input fields: every byte, 3 mutations each\n");
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) {
        const zv_pczt_vector *v = &ZV_PCZT[i];
        if (v->expect != 0 || v->n_tinputs == 0) continue;
        size_t len;
        uint8_t *b = slurp(v->file, &len);
        if (!b || zc_pczt_read(b, len, &layout) != ZC_R_OK || layout.n_tin == 0) {
            expect(0, v->name, "transparent layout");
            free(b);
            continue;
        }
        const zc_tin *last = &layout.tin[layout.n_tin - 1];
        size_t from = (size_t)(layout.tin[0].prevout_txid - b);
        size_t to = (size_t)(last->bip32_fp - b) + 32 + 1 + 5 * 5;
        if (to > len) to = len;
        uint8_t base[32];
        int32_t e;
        zs_ctx *r = zs_pczt_new(b, len, &e);
        expect(r && zs_pczt_check(r, seeds[i], v->account, v->network, &sr) == 0, v->name,
               "base checks");
        memcpy(base, sr.sighash, 32);
        zs_pczt_free(r);
        for (size_t pos = from; pos < to; pos++) {
            for (int k = 0; k < 3; k++) {
                uint8_t old = b[pos], nb = mutate_byte(old);
                b[pos] = nb;
                run_both(b, len, seeds[i], v->account, v->network, total % 4 == 0, &res);
                total++;
                if (res.rc_r != res.rc_c) {
                    if (++mismatch <= 10)
                        printf("  %s byte %zu %02x->%02x: rust %d, c %d\n", v->name, pos, old, nb,
                               res.rc_r, res.rc_c);
                } else if (res.rc_r == 0) {
                    accepted++;
                    if (!summaries_equal()) {
                        if (++mismatch <= 10) {
                            printf("  SUMMARY %s byte %zu\n", v->name, pos);
                            explain_summary();
                        }
                    } else if (memcmp(sr.sighash, base, 32) == 0 && nb != old) {
                        /* Only bytes outside the transaction (the path's
                         * fingerprint is checked, so none should be) may
                         * leave the sighash alone. */
                        if (++mismatch <= 10)
                            printf("  %s byte %zu accepted with the same sighash\n", v->name, pos);
                    }
                }
                b[pos] = old;
            }
        }
        free(b);
    }
    printf("  %ld mutations, %ld accepted by both, %ld mismatches\n", total, accepted, mismatch);
    fails += (int)mismatch;
}

int main(void)
{
    const char *ns = getenv("ZSIGN_DIFF_N"), *ss = getenv("ZSIGN_DIFF_SEED");
    long per_vector = ns ? strtol(ns, NULL, 10) : 600;
    s_rng = 0x5EED5EED12345678ull ^ (ss ? (uint64_t)strtoull(ss, NULL, 10) : 0);

    printf("== test_zsign_diff\n== %zu vectors through both backends\n", ZV_PCZT_COUNT);
    static uint8_t seeds[ZV_PCZT_COUNT][64];
    static result res;
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) {
        const zv_pczt_vector *v = &ZV_PCZT[i];
        size_t len;
        uint8_t *b = slurp(v->file, &len);
        expect(b != NULL, v->name, "vector file readable");
        if (!b) continue;
        mnemonic_to_seed(v->mnemonic, v->passphrase, seeds[i], NULL);
        run_both(b, len, seeds[i], v->account, v->network, 1, &res);
        printf("  %-28s rust %3d  c %3d  sigs %u/%u\n", v->name, res.rc_r, res.rc_c, res.n_r,
               res.n_c);
        expect(res.rc_r == res.rc_c, v->name, "same verdict and code");
        expect(res.rc_r == v->expect, v->name, "the expected verdict");
        if (res.rc_r == 0 && res.rc_c == 0) {
            expect(summaries_equal(), v->name, "identical summaries");
            if (!summaries_equal()) explain_summary();
            expect(res.n_r >= 1 && res.n_r == res.n_c, v->name, "both sign the same spends");
        }
        free(b);
    }

    printf("== %ld single-byte mutations per valid vector, %ld per refusal vector\n",
           per_vector, per_vector / 8);
    long total = 0, accepted = 0, verdict_mismatch = 0, summary_mismatch = 0, code_mismatch = 0;
    static zc_pczt layout;
    static uint8_t in_witness[16384];
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) {
        const zv_pczt_vector *v = &ZV_PCZT[i];
        long count = v->expect == 0 ? per_vector : per_vector / 8;
        size_t len;
        uint8_t *b = slurp(v->file, &len);
        if (!b || len > sizeof in_witness) {
            expect(0, v->name, "vector readable for mutation");
            free(b);
            continue;
        }
        memset(in_witness, 0, sizeof in_witness);
        if (zc_pczt_read(b, len, &layout) == ZC_R_OK)
            for (int a = 0; a < layout.n_actions; a++)
                if (layout.actions[a].witness)
                    memset(in_witness + (layout.actions[a].witness - b), 1, 32 * 32);
        long acc_here = 0;
        for (long m = 0; m < count; m++) {
            size_t pos;
            do pos = (size_t)(rnd() % len);
            while (in_witness[pos] && rnd() % 3);
            uint8_t old = b[pos], nb = mutate_byte(old);
            b[pos] = nb;
            run_both(b, len, seeds[i], v->account, v->network, acc_here % 8 == 0, &res);
            total++;
            int ar = res.rc_r == 0, ac = res.rc_c == 0;
            if (ar != ac) {
                verdict_mismatch++;
                fails++;
                if (verdict_mismatch <= 20)
                    printf("  VERDICT %s byte %zu %02x->%02x: rust %d, c %d\n", v->name, pos, old,
                           nb, res.rc_r, res.rc_c);
            } else if (ar) {
                accepted++;
                acc_here++;
                if (!summaries_equal()) {
                    summary_mismatch++;
                    fails++;
                    if (summary_mismatch <= 10) {
                        printf("  SUMMARY %s byte %zu %02x->%02x\n", v->name, pos, old, nb);
                        explain_summary();
                    }
                }
            } else if (res.rc_r != res.rc_c) {
                code_mismatch++;
                fails++;
                if (code_mismatch <= 20)
                    printf("  code    %s byte %zu %02x->%02x: rust %d, c %d\n", v->name, pos, old,
                           nb, res.rc_r, res.rc_c);
            }
            b[pos] = old;
        }
        printf("  %-28s %5ld mutations, %5ld accepted by both\n", v->name, count, acc_here);
        free(b);
    }
    printf("  total %ld: %ld accepted, %ld verdict mismatches, %ld summary mismatches, "
           "%ld refusal-code mismatches\n",
           total, accepted, verdict_mismatch, summary_mismatch, code_mismatch);

    compact_fields(seeds, per_vector / 75 > 0 ? per_vector / 75 : 1);
    compact_rebinding(seeds);
    transparent_fields(seeds);
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) memzero(seeds[i], 64);

    if (fails) {
        printf("\nFAILED (%d failures)\n", fails);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
