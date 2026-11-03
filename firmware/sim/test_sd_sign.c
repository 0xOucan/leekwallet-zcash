/* SD card airgap signing (src/sd-sign.c, roadmap 5), through the real vault,
 * both signer backends, and a directory standing in for the card.
 *
 * - a vector PCZT on the card is checked, summarised as over USB, signed,
 *   and LEEKZ/signed.zsig carries the file's hash and the signatures;
 * - a file changed between review and signing gets no signature;
 * - a tampered PCZT, the wrong network, no file and no card are refused,
 *   and nothing is written;
 * - an old signed.zsig is replaced, never appended to.
 */
#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "sd-sign.h"
#include "sdcard.h"
#include "sha2.h"
#include "zcash-sign.h"
#include "../../vectors/pczt_vectors.h"

void wallet__reset_static_state_for_test(void);
char *mkdtemp(char *tmpl);

static int failures;
static const char *ctx = "setup";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static char card[64] = "/tmp/leek-sdsign-XXXXXX";

static const zv_pczt_vector *vec(const char *name)
{
    for (size_t i = 0; i < sizeof ZV_PCZT / sizeof *ZV_PCZT; i++) {
        if (strcmp(ZV_PCZT[i].name, name) == 0) return &ZV_PCZT[i];
    }
    return NULL;
}

static uint8_t *load(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    fclose(f);
    return b;
}

static void card_file(const char *rel, const uint8_t *b, size_t len)
{
    char p[256];
    snprintf(p, sizeof p, "%s/LEEKZ", card);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s", card, rel);
    FILE *f = fopen(p, "wb");
    fwrite(b, 1, len, f);
    fclose(f);
}

static bool card_has(const char *rel)
{
    char p[256];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", card, rel);
    return stat(p, &st) == 0;
}

static void card_rm(const char *rel)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", card, rel);
    unlink(p);
}

static void device_with(const char *mnemonic)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    wallet_set_password("1234", 4);
    wallet_unlock("1234", 4);
    CHECK(wallet_use_temporary_mnemonic(mnemonic) == WALLET_OK, "seed");
    sdcard__set_root_for_test(card);
}

static void test_sign_from_card(void)
{
    printf("== a PCZT on the card is checked, reviewed and signed to signed.zsig\n");
    const zv_pczt_vector *v = vec("iw_send_shielded");
    ctx = v->name;
    size_t len;
    uint8_t *pczt = load(v->file, &len);
    CHECK(pczt, "vector file");
    device_with(v->mnemonic);
    card_rm(SDSIGN_OUT);
    card_file(SDSIGN_IN, pczt, len);
    /* A stale signature file from an earlier run must be replaced. */
    card_file(SDSIGN_OUT, (const uint8_t *)"stale stale stale", 17);

    CHECK(sdsign_available(), "the file is not seen");
    const zs_summary *s = NULL;
    int rc = sdsign_load(v->account, v->network, &s);
    CHECK(rc == 0 && s, "load: %s", sdsign_strerror(rc));
    if (!s) return;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", s->sighash[i]);
    CHECK(strcmp(hex, v->sighash) == 0, "sighash %s", hex);
    CHECK(s->n_outputs == v->n_outputs, "outputs");

    uint32_t n = 0;
    rc = sdsign_finish(&n);
    CHECK(rc == 0 && n >= 1, "finish: %s, %u sigs", sdsign_strerror(rc), n);

    char p[256];
    snprintf(p, sizeof p, "%s/%s", card, SDSIGN_OUT);
    size_t zl;
    uint8_t *z = load(p, &zl);
    uint8_t h[32];
    sha256_Raw(pczt, len, h);
    CHECK(z && zl == 39 + n * 69, "signed.zsig is %zu bytes for %u sigs", zl, n);
    CHECK(z && memcmp(z, "ZSIG", 4) == 0 && z[4] == 1, "header");
    CHECK(z && memcmp(z + 5, h, 32) == 0, "the .zsig does not name the file's hash");
    CHECK(z && (z[37] | z[38] << 8) == (int)n, "count");
    CHECK(z && z[39] == ZS_POOL_IRONWOOD, "pool %u", z ? z[39] : 0);
    CHECK(!card_has("LEEKZ/signed.zsig.tmp"), "temporary left behind");
    free(z);

    /* Signed once: the upload is consumed. */
    CHECK(sdsign_finish(&n) == SDSIGN_E_STATE, "signed twice");
    free(pczt);
}

static void test_changed_file_is_not_signed(void)
{
    printf("== a file changed between review and signing gets no signature\n");
    const zv_pczt_vector *v = vec("iw_send_shielded");
    ctx = "changed";
    size_t len;
    uint8_t *pczt = load(v->file, &len);
    device_with(v->mnemonic);
    card_rm(SDSIGN_OUT);
    card_file(SDSIGN_IN, pczt, len);
    const zs_summary *s = NULL;
    CHECK(sdsign_load(v->account, v->network, &s) == 0, "load");
    pczt[len / 2] ^= 1;
    card_file(SDSIGN_IN, pczt, len);   /* swapped while the user read */
    uint32_t n = 99;
    CHECK(sdsign_finish(&n) == SDSIGN_E_CHANGED && n == 0, "a changed file was signed");
    CHECK(!card_has(SDSIGN_OUT), "a signature file was written");
    free(pczt);
}

static void test_refusals(void)
{
    printf("== tampered, wrong network, no file, no card: refused, nothing written\n");
    const zv_pczt_vector *t = vec("r_output_value_tampered");
    ctx = t->name;
    size_t len;
    uint8_t *pczt = load(t->file, &len);
    device_with(t->mnemonic);
    card_rm(SDSIGN_OUT);
    card_file(SDSIGN_IN, pczt, len);
    const zs_summary *s = (const zs_summary *)1;
    int rc = sdsign_load(t->account, t->network, &s);
    CHECK(rc == t->expect && s == NULL, "tampered: %d, expected %d", rc, t->expect);
    uint32_t n;
    CHECK(sdsign_finish(&n) == SDSIGN_E_STATE, "a refused file could be signed");
    CHECK(!card_has(SDSIGN_OUT), "written after a refusal");
    free(pczt);

    const zv_pczt_vector *v = vec("iw_send_shielded");
    ctx = "network";
    pczt = load(v->file, &len);
    card_file(SDSIGN_IN, pczt, len);
    CHECK(sdsign_load(v->account, ZS_NET_MAIN, &s) == ZS_E_NETWORK, "mainnet signed a testnet file");
    free(pczt);

    ctx = "no file";
    card_rm(SDSIGN_IN);
    CHECK(!sdsign_available(), "a missing file is offered");
    CHECK(sdsign_load(0, ZS_NET_TEST, &s) == SDSIGN_E_NOFILE, "no file");

    ctx = "no card";
    sdcard__set_root_for_test(NULL);
    CHECK(sdsign_load(0, ZS_NET_TEST, &s) == SDSIGN_E_NOCARD, "no card");
}

/* Roadmap 10.1: a shield signs two transparent inputs, and signed.zsig is
 * version 2 with a DER || SIGHASH_ALL and a pubkey per input. */
static void test_shield_from_card(void)
{
    printf("== a shield on the card: transparent signatures, .zsig version 2\n");
    const zv_pczt_vector *v = vec("t_shield");
    ctx = v ? v->name : "t_shield";
    CHECK(v, "vector");
    if (!v) return;
    size_t len;
    uint8_t *pczt = load(v->file, &len);
    device_with(v->mnemonic);
    card_rm(SDSIGN_OUT);
    card_file(SDSIGN_IN, pczt, len);
    const zs_summary *s = NULL;
    int rc = sdsign_load(v->account, v->network, &s);
    CHECK(rc == 0 && s && s->n_tinputs == 2, "load: %s", sdsign_strerror(rc));
    uint32_t n = 0;
    rc = sdsign_finish(&n);
    CHECK(rc == 0 && n == 2, "finish: %s, %u sigs", sdsign_strerror(rc), n);

    char p[256];
    snprintf(p, sizeof p, "%s/%s", card, SDSIGN_OUT);
    size_t zl;
    uint8_t *z = load(p, &zl);
    CHECK(z && zl > 39 && z[4] == 2, "version 2");
    size_t at = 39;
    for (uint32_t i = 0; z && i < n; i++) {
        CHECK(at + 6 <= zl && z[at] == ZS_POOL_TRANSPARENT && z[at + 1] == i, "entry %u header", i);
        size_t l = z[at + 5];
        const uint8_t *d = z + at + 6;
        CHECK(l >= 9 && l <= 73 && at + 6 + l + 33 <= zl, "entry %u length %zu", i, l);
        CHECK(d[0] == 0x30 && d[1] == l - 3 && d[2] == 0x02, "entry %u is not DER", i);
        CHECK(d[l - 1] == 0x01, "entry %u is not SIGHASH_ALL", i);
        uint8_t pk0 = z[at + 6 + l];
        CHECK(pk0 == 0x02 || pk0 == 0x03, "entry %u pubkey prefix %02x", i, pk0);
        at += 6 + l + 33;
    }
    CHECK(at == zl, "trailing bytes: %zu of %zu", at, zl);
    free(z);
    free(pczt);
}

/* What the card holds as signed.zsig: 0 none, 1 a whole .zsig for the PCZT
 * whose hash is `h`, -1 anything else (short, for another file, garbage). */
static int zsig_state(const uint8_t h[32])
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", card, SDSIGN_OUT);
    size_t zl;
    uint8_t *z = load(p, &zl);
    if (!z) return 0;
    int st = -1;
    if (zl >= 39 && memcmp(z, "ZSIG", 4) == 0 && z[4] == 1 && memcmp(z + 5, h, 32) == 0) {
        uint32_t n = (uint32_t)(z[37] | z[38] << 8);
        if (n >= 1 && zl == 39 + n * 69) st = 1;
    }
    free(z);
    return st;
}

/* docs/AUDIT-SD-WRITES.md: every step of a load and a signing, broken every
 * way the fake card knows. Afterwards the card holds this PCZT's whole
 * .zsig or none - never a short one, never another file's - and a success
 * is only reported for a .zsig that is really there. */
static void test_sign_broken_at_every_step(void)
{
    printf("== signing broken at any step: a whole .zsig for this PCZT, or none\n");
    const zv_pczt_vector *v = vec("iw_send_shielded");
    ctx = "every step";
    size_t len;
    uint8_t *pczt = load(v->file, &len);
    uint8_t h[32];
    sha256_Raw(pczt, len, h);
    device_with(v->mnemonic);
    card_rm(SDSIGN_OUT);
    card_file(SDSIGN_IN, pczt, len);

    /* Count the steps of a clean load and finish, over an earlier answer for
     * the same file (the case with the most steps: it is kept at load, then
     * replaced). */
    const zs_summary *s = NULL;
    uint32_t n = 0;
    CHECK(sdsign_load(v->account, v->network, &s) == 0 && sdsign_finish(&n) == 0, "first signing");
    sdcard__set_root_for_test(card);
    sdcard__fault_at_step_for_test(0, SDCARD_FAULT_NONE);
    CHECK(sdsign_load(v->account, v->network, &s) == 0 && sdsign_finish(&n) == 0, "counting");
    int steps = sdcard__steps_for_test();
    CHECK(steps >= 7, "a signing is only %d steps", steps);

    static const struct { SdcardFault kind; const char *name; } kinds[] = {
        { SDCARD_FAULT_FAIL, "fail" }, { SDCARD_FAULT_CUT, "cut" },
        { SDCARD_FAULT_TEAR, "tear" }, { SDCARD_FAULT_FLIP, "flip" },
    };
    int tried = 0;
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        for (int step = 1; step <= steps; step++) {
            char what[48];
            snprintf(what, sizeof what, "%s at step %d", kinds[k].name, step);
            sdcard__set_root_for_test(card);
            sdcard__fault_at_step_for_test(step, kinds[k].kind);
            int rc = sdsign_load(v->account, v->network, &s);
            if (rc == 0) rc = sdsign_finish(&n);
            else sdsign_cancel();
            sdcard__set_root_for_test(card);   /* rebooted */
            int st = zsig_state(h);
            CHECK(st >= 0, "%s: signed.zsig is not a whole .zsig for this file", what);
            if (rc == 0) CHECK(st == 1, "%s: success reported, no signed.zsig", what);
            /* Whatever a cut left, the next load starts clean. */
            CHECK(sdsign_load(v->account, v->network, &s) == 0, "%s: reload", what);
            CHECK(!card_has("LEEKZ/signed.zsig.tmp"), "%s: a temporary survived the next load", what);
            sdsign_cancel();
            tried++;
        }
    }

    /* A full card at every size short of the file: the old answer stands. */
    for (long room = 0; room < 39 + 69; room += 13) {
        char what[48];
        snprintf(what, sizeof what, "full card at %ld bytes", room);
        sdcard__set_root_for_test(card);
        CHECK(sdsign_load(v->account, v->network, &s) == 0, "%s: load", what);
        sdcard__card_space_for_test(room);
        CHECK(sdsign_finish(&n) == SDSIGN_E_IO, "%s: reported written", what);
        CHECK(zsig_state(h) == 1, "%s: the earlier .zsig was lost or damaged", what);
        tried++;
    }
    printf("  %d broken signings, %d steps each\n", tried, steps);
    free(pczt);
}

/* An earlier request's signed.zsig is gone before the review, so a rejected
 * or failed signing never leaves another file's answer next to this one. */
static void test_stale_zsig_is_dropped(void)
{
    printf("== a signed.zsig for another file is removed before the review\n");
    const zv_pczt_vector *v = vec("iw_send_shielded");
    ctx = "stale";
    size_t len;
    uint8_t *pczt = load(v->file, &len);
    uint8_t h[32];
    sha256_Raw(pczt, len, h);
    device_with(v->mnemonic);
    card_file(SDSIGN_IN, pczt, len);

    uint8_t other[39 + 69];
    memset(other, 0x11, sizeof other);
    memcpy(other, "ZSIG", 4);
    other[4] = 1;
    other[37] = 1;
    other[38] = 0;
    static const struct { size_t len; const char *name; } stale[] = {
        { sizeof other, "another file's .zsig" }, { 0, "an empty file" },
        { 20, "a short file" },
    };
    for (size_t i = 0; i < sizeof stale / sizeof *stale; i++) {
        ctx = stale[i].name;
        card_file(SDSIGN_OUT, other, stale[i].len);
        card_file("LEEKZ/signed.zsig.tmp", other, sizeof other);
        const zs_summary *s = NULL;
        CHECK(sdsign_load(v->account, v->network, &s) == 0 && s, "load");
        CHECK(!card_has(SDSIGN_OUT), "still on the card during the review");
        CHECK(!card_has("LEEKZ/signed.zsig.tmp"), "a temporary is still on the card");
        sdsign_cancel();
        CHECK(!card_has(SDSIGN_OUT), "back after a rejection");
    }

    /* This file's own answer survives a review that is then rejected. */
    ctx = "own answer";
    const zs_summary *s = NULL;
    uint32_t n = 0;
    CHECK(sdsign_load(v->account, v->network, &s) == 0 && sdsign_finish(&n) == 0, "sign");
    CHECK(sdsign_load(v->account, v->network, &s) == 0, "load again");
    sdsign_cancel();
    CHECK(zsig_state(h) == 1, "this file's .zsig was removed");

    /* A card that will not let the old answer go is refused before the
     * review, not after it. */
    ctx = "stuck";
    card_file(SDSIGN_OUT, other, sizeof other);
    sdcard__set_root_for_test(card);
    sdcard__fault_at_step_for_test(2, SDCARD_FAULT_FAIL);   /* the .zsig's unlink */
    s = (const zs_summary *)1;
    CHECK(sdsign_load(v->account, v->network, &s) == SDSIGN_E_IO && s == NULL, "reviewed anyway");
    CHECK(sdsign_finish(&n) == SDSIGN_E_STATE, "signable after a refusal");
    sdcard__set_root_for_test(card);
    card_rm(SDSIGN_OUT);
    free(pczt);
}

int main(void)
{
    printf("== test_sd_sign\n");
    if (!mkdtemp(card)) return 1;
    test_sign_from_card();
    test_shield_from_card();
    test_changed_file_is_not_signed();
    test_sign_broken_at_every_step();
    test_stale_zsig_is_dropped();
    test_refusals();
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", card);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", card);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
