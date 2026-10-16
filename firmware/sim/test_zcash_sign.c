/* PCZT intake and signing (src/zcash-sign.c) through the real vault and both
 * real signer backends.
 *
 * Every vector is uploaded in chunks, checked and signed under each backend
 * setting; the summary must match the vector and every signature must verify.
 * Around that: uploads that are oversized, out of order, short, or do not
 * match their hash; calls out of order; a locked device; and an injected
 * disagreement between the backends, which BOTH must refuse (roadmap 4.3).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "sha2.h"
#include "zcash-sign.h"
#include "zsign_c.h"
#include "../../vectors/pczt_vectors.h"

void wallet__reset_static_state_for_test(void);

static bool branch_seen[ZCASH_SIGN_BRANCH_COUNT];
static int failures;
static const char *ctx = "setup";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static const char *PIN = "1234";

static uint8_t *load(const char *path, size_t *len)
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

static void device_with(const char *mnemonic)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    CHECK(wallet_set_password(PIN, strlen(PIN)) == WALLET_OK, "no password");
    CHECK(wallet_unlock(PIN, strlen(PIN)) == WALLET_OK, "no unlock");
    CHECK(wallet_use_temporary_mnemonic(mnemonic) == WALLET_OK, "seed not adopted");
}

/* Upload `b` in `chunk`-byte pieces under its true hash. */
static int upload(const uint8_t *b, size_t len, size_t chunk, uint32_t account, uint8_t net)
{
    uint8_t h[32];
    sha256_Raw(b, len, h);
    int rc = zcash_sign_begin((uint32_t)len, h, account, net);
    for (size_t off = 0; rc == ZS_OK && off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        rc = zcash_sign_chunk((uint32_t)off, b + off, n);
    }
    return rc;
}

static void hex32(const uint8_t *b, char out[65])
{
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

static void run_vector(const zv_pczt_vector *v, ZcashSignBackend be)
{
    size_t len = 0;
    uint8_t *b = load(v->file, &len);
    CHECK(b, "cannot read %s", v->file);
    if (!b) return;

    zcash_sign_set_backend(be);
    CHECK(upload(b, len, 1000, v->account, v->network) == ZS_OK, "upload refused");
    const zs_summary *s = NULL;
    int rc = zcash_sign_check(&s);
    CHECK(rc == v->expect, "[%s] check %d, expected %d (%s)", zcash_sign_backend_name(be), rc,
          v->expect, zcash_sign_strerror(rc));
    if (rc == ZS_OK && s) {
        char sh[65];
        hex32(s->sighash, sh);
        CHECK(strcmp(sh, v->sighash) == 0, "sighash");
        CHECK(s->fee == v->fee && s->branch_id == v->branch_id, "fee or branch");
        /* zGetFeatures advertises ZCASH_SIGN_BRANCHES: every branch a backend
         * accepted must be in it, and every entry must be accepted. */
        bool listed = false;
        for (size_t k = 0; k < ZCASH_SIGN_BRANCH_COUNT; k++) {
            if (ZCASH_SIGN_BRANCHES[k] == s->branch_id) {
                listed = true;
                branch_seen[k] = true;
            }
        }
        CHECK(listed, "branch %08x accepted but not advertised", s->branch_id);
        CHECK(s->n_outputs == v->n_outputs, "outputs %u", s->n_outputs);
        /* Action order is shuffled by the builder, so match by content. */
        for (unsigned i = 0; i < v->n_outputs; i++) {
            const zv_pczt_output *e = &v->outputs[i];
            bool found = false;
            for (unsigned j = 0; j < s->n_outputs; j++) {
                const zs_output *o = &s->outputs[j];
                found |= o->pool == e->pool && o->kind == e->kind && o->value == e->value &&
                         strcmp(o->address, e->address) == 0;
            }
            CHECK(found, "expected output %u not shown", i);
        }

        static zs_sig sigs[ZS_MAX_SIGS];
        uint32_t n = 0;
        rc = zcash_sign_sign(sigs, ZS_MAX_SIGS, &n);
        CHECK(rc == ZS_OK && n >= 1, "[%s] sign %d, %u sigs", zcash_sign_backend_name(be), rc, n);

        /* Verified by a fresh C context, as a host would, independently of
         * the module's own verification. */
        int32_t err = 0;
        zc_ctx *c = zc_pczt_new(b, len, &err);
        CHECK(c, "re-parse");
        for (uint32_t i = 0; c && i < n; i++) {
            CHECK(zc_pczt_verify_sig(c, &sigs[i]) == ZS_OK, "signature %u does not verify", i);
        }
        if (c) zc_pczt_free(c);
        CHECK(zcash_sign_sign(sigs, ZS_MAX_SIGS, &n) == ZSIGN_E_STATE, "signed twice");
    }
    zcash_sign_reset();
    free(b);
}

int main(void)
{
    printf("== test_zcash_sign\n== %zu vectors x 3 backends, chunked, real vault\n", ZV_PCZT_COUNT);
    static const ZcashSignBackend backends[] = {
        ZSIGN_BACKEND_RUST, ZSIGN_BACKEND_C, ZSIGN_BACKEND_BOTH };
    for (size_t i = 0; i < ZV_PCZT_COUNT; i++) {
        const zv_pczt_vector *v = &ZV_PCZT[i];
        ctx = v->name;
        device_with(v->mnemonic);
        if (v->passphrase[0]) {
            CHECK(wallet_set_passphrase(v->passphrase, strlen(v->passphrase)) == WALLET_OK, "pass");
        }
        for (size_t k = 0; k < 3; k++) run_vector(v, backends[k]);
    }

    const zv_pczt_vector *good = &ZV_PCZT[0];
    size_t len = 0;
    uint8_t *b = load(good->file, &len);
    uint8_t h[32];
    sha256_Raw(b, len, h);
    const zs_summary *s;
    zs_sig sigs[ZS_MAX_SIGS];
    uint32_t n;
    device_with(good->mnemonic);
    zcash_sign_set_backend(ZSIGN_BACKEND_BOTH);

    printf("== intake refusals\n");
    ctx = "intake";
    CHECK(zcash_sign_begin(0, h, 0, ZS_NET_TEST) == ZSIGN_E_TOO_LARGE, "empty upload");
    CHECK(zcash_sign_begin(ZCASH_PCZT_MAX + 1, h, 0, ZS_NET_TEST) == ZSIGN_E_TOO_LARGE, "oversized");
    CHECK(zcash_sign_begin(10, h, 0x80000000u, ZS_NET_TEST) == ZS_ERR_ARG, "hardened account");
    CHECK(zcash_sign_begin(10, h, 0, 9) == ZS_ERR_ARG, "unknown network");
    CHECK(zcash_sign_chunk(0, b, 10) == ZSIGN_E_STATE, "chunk with no upload");
    CHECK(zcash_sign_check(&s) == ZSIGN_E_STATE, "check with no upload");
    CHECK(zcash_sign_sign(sigs, ZS_MAX_SIGS, &n) == ZSIGN_E_STATE, "sign with no upload");

    CHECK(zcash_sign_begin((uint32_t)len, h, 0, ZS_NET_TEST) == ZS_OK, "begin");
    CHECK(zcash_sign_chunk(5, b + 5, 10) == ZSIGN_E_OFFSET, "chunk at a gap");
    CHECK(zcash_sign_chunk(0, b, 10) == ZSIGN_E_STATE, "upload survived a bad chunk");

    CHECK(zcash_sign_begin((uint32_t)len, h, 0, ZS_NET_TEST) == ZS_OK, "begin");
    CHECK(zcash_sign_chunk(0, b, len) == ZS_OK, "whole");
    CHECK(zcash_sign_chunk((uint32_t)len, b, 1) == ZSIGN_E_OFFSET, "past the end");

    CHECK(zcash_sign_begin((uint32_t)len, h, 0, ZS_NET_TEST) == ZS_OK, "begin");
    CHECK(zcash_sign_chunk(0, b, len - 1) == ZS_OK, "all but one");
    CHECK(zcash_sign_received() == len - 1, "received count");
    CHECK(zcash_sign_check(&s) == ZSIGN_E_INCOMPLETE, "checked a short upload");

    printf("== hash mismatch: one flipped byte, honest length\n");
    ctx = "hash";
    b[len / 2] ^= 1;
    CHECK(zcash_sign_begin((uint32_t)len, h, 0, ZS_NET_TEST) == ZS_OK, "begin");
    CHECK(zcash_sign_chunk(0, b, len) == ZS_OK, "chunk");
    CHECK(zcash_sign_check(&s) == ZSIGN_E_HASH && s == NULL, "hash not enforced");
    b[len / 2] ^= 1;

    printf("== wrong network or account is refused by the check\n");
    ctx = "network";
    CHECK(upload(b, len, 4096, 0, ZS_NET_MAIN) == ZS_OK, "upload");
    CHECK(zcash_sign_check(&s) == ZS_E_NETWORK, "mainnet accepted a testnet PCZT");
    ctx = "account";
    CHECK(upload(b, len, 4096, 1, ZS_NET_TEST) == ZS_OK, "upload");
    CHECK(zcash_sign_check(&s) == ZS_E_SPEND_NOT_OURS, "account 1 signed account 0's spend");

    printf("== BOTH refuses an injected disagreement; one backend alone does not see it\n");
    ctx = "disagree";
    zcash_sign__inject_disagreement_for_test(true);
    CHECK(upload(b, len, 4096, 0, ZS_NET_TEST) == ZS_OK, "upload");
    CHECK(zcash_sign_check(&s) == ZSIGN_E_DISAGREE, "disagreement not refused");
    CHECK(zcash_sign_sign(sigs, ZS_MAX_SIGS, &n) == ZSIGN_E_STATE, "signable after a disagreement");
    zcash_sign_set_backend(ZSIGN_BACKEND_RUST);
    CHECK(upload(b, len, 4096, 0, ZS_NET_TEST) == ZS_OK, "upload");
    CHECK(zcash_sign_check(&s) == ZS_OK, "rust alone");
    zcash_sign__inject_disagreement_for_test(false);
    zcash_sign_set_backend(ZSIGN_BACKEND_BOTH);

    printf("== locked between check and sign: no signature\n");
    ctx = "locked";
    CHECK(upload(b, len, 4096, 0, ZS_NET_TEST) == ZS_OK, "upload");
    CHECK(zcash_sign_check(&s) == ZS_OK, "check");
    wallet_lock();
    n = 99;
    CHECK(zcash_sign_sign(sigs, ZS_MAX_SIGS, &n) == ZSIGN_E_LOCKED && n == 0, "signed while locked");
    CHECK(upload(b, len, 4096, 0, ZS_NET_TEST) == ZS_OK, "upload");
    CHECK(zcash_sign_check(&s) == ZSIGN_E_LOCKED, "checked while locked");

    free(b);
    zcash_sign_reset();
    for (size_t k = 0; k < ZCASH_SIGN_BRANCH_COUNT; k++) {
        CHECK(branch_seen[k], "advertised branch %08x was never accepted", ZCASH_SIGN_BRANCHES[k]);
    }
    if (failures) {
        printf("\nFAILED (%d failures)\n", failures);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
