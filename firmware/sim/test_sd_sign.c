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

int main(void)
{
    printf("== test_sd_sign\n");
    if (!mkdtemp(card)) return 1;
    test_sign_from_card();
    test_changed_file_is_not_signed();
    test_refusals();
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", card);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", card);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
