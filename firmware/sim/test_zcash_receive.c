/* The receive pages (src/zcash-receive.c) and the seed fingerprint.
 *
 * For every key vector (librustzcash): the unified and transparent
 * addresses reassemble in full from their text pages, nothing is truncated,
 * each QR fits the panel, and the QR the panel would draw scans back to the
 * exact address (zbarimg, when installed). One snapshot of the first page.
 *
 * The fingerprint (roadmap 1.5): the device's ZIP 32 seed fingerprint
 * matches every vector, passphrase included, and different passphrases on
 * one mnemonic give different short forms.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bip39.h"
#include "oled.h"
#include "oled-core.h"
#include "zcash-fp.h"
#include "zcash-receive.h"
#include "../../vectors/keys_vectors.h"

static int failures;
static const char *ctx = "";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

/* The panel's transport, which oled-core.c calls and this suite does not need. */
void leek_log(const char *level, const char *tag, const char *fmt, ...) { }
esp_err_t oled_refresh(void) { return ESP_OK; }

/* ------------------------------------------------- the wallet, for the fp */

static const char *cur_mnemonic, *cur_passphrase;
static bool locked;

WalletError wallet_with_seed(WalletSeedFn fn, void *c)
{
    if (locked) return WALLET_ERROR_LOCKED;
    uint8_t seed[64];
    mnemonic_to_seed(cur_mnemonic, cur_passphrase, seed, NULL);
    WalletError err = fn(seed, c);
    memset(seed, 0, sizeof seed);
    return err;
}

/* ------------------------------------------------------------ helpers */

static void account_from(const zv_key_vector *v, ZcashAccount *a)
{
    memset(a, 0, sizeof *a);
    a->account = v->account;
    a->network = v->network;
    memcpy(a->keys.seed_fingerprint, v->seed_fingerprint, 32);
    snprintf(a->keys.ufvk, sizeof a->keys.ufvk, "%s", v->ufvk);
    snprintf(a->keys.ua, sizeof a->keys.ua, "%s", v->ua);
    snprintf(a->t_address, sizeof a->t_address, "%s", v->transparent_address);
}

static bool have_zbar;

/* Draw the QR as the panel would, write the 128x64 frame as a PGM at 4x
 * inside a dark bezel, and read it back with zbarimg. */
static bool scans_to(const ZRecvPage *p, const char *want)
{
    oled_clear();
    if (oled_draw_qrcode_at(p->qr, p->version, p->scale) != ESP_OK) return false;
    if (!have_zbar) return true;

    const uint8_t *fb = oled_core_framebuffer();
    const int S = 4, M = 16, W = 128 * S + 2 * M, H = 64 * S + 2 * M;
    char path[] = "build/zrecv_qrXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int px = (x - M) / S, py = (y - M) / S;
            bool lit = x >= M && y >= M && px < 128 && py < 64 &&
                       (fb[(py / 8) * 128 + px] >> (py % 8)) & 1;
            /* As the panel looks: lit pixels are the code's light modules
             * and quiet zone, and everything unlit, the bezel included, is
             * dark. */
            fputc(lit ? 255 : 0, f);
        }
    }
    fclose(f);

    char cmd[128], got[512] = {0};
    snprintf(cmd, sizeof cmd, "zbarimg -q --raw %s 2>/dev/null", path);
    FILE *z = popen(cmd, "r");
    size_t n = z ? fread(got, 1, sizeof got - 1, z) : 0;
    if (z) pclose(z);
    unlink(path);
    while (n && (got[n - 1] == '\n' || got[n - 1] == '\r')) got[--n] = '\0';
    if (strcmp(got, want) != 0) {
        printf("    scanned \"%s\"\n    wanted  \"%s\"\n", got, want);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- tests */

static void test_snapshot(void)
{
    ctx = "snapshot main/abandon-art/acct0";
    ZcashAccount a;
    account_from(&ZV_KEYS[0], &a);
    int n = zrecv_page_count(&a);
    for (int i = 0; i < n; i++) {
        ZRecvPage p;
        zrecv_render(&a, i, &p);
        if (p.kind == ZRECV_QR) {
            printf("    [QR v%u x%u: %s]\n", p.version, p.scale, p.qr);
            continue;
        }
        printf("    +---------------------+\n");
        for (int r = 0; r < ZRECV_ROWS; r++) printf("    |%-21s|\n", p.row[r]);
    }
    printf("    +---------------------+\n");

    ZRecvPage p;
    CHECK(zrecv_render(&a, 0, &p) && p.kind == ZRECV_TEXT, "page 0 is text");
    CHECK(strcmp(p.row[0], "UA 1/2  acct 0  MAIN") == 0, "title \"%s\"", p.row[0]);
    CHECK(strncmp(p.row[1], a.keys.ua, 21) == 0, "first address row");
    CHECK(strcmp(p.row[7], "BACK    UP/DN    QR") == 0, "footer \"%s\"", p.row[7]);
    CHECK(n == 5, "two UA pages, UA QR, t-addr, t-addr QR: %d", n);
    CHECK(!zrecv_render(&a, n, &p) && !zrecv_render(&a, -1, &p), "out of range");
}

static void test_every_vector(void)
{
    char ua[ZS_UA_MAX], t[ZCASH_TADDR_MAX];
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const zv_key_vector *v = &ZV_KEYS[i];
        ctx = v->name;
        ZcashAccount a;
        account_from(v, &a);
        int n = zrecv_page_count(&a);
        ua[0] = t[0] = '\0';
        int ua_qr = -1, t_text = -1, t_qr = -1;
        for (int pg = 0; pg < n; pg++) {
            ZRecvPage p;
            CHECK(zrecv_render(&a, pg, &p), "page %d renders", pg);
            if (p.kind == ZRECV_QR) {
                if (ua_qr < 0) ua_qr = pg; else t_qr = pg;
                const char *want = ua_qr == pg ? v->ua : v->transparent_address;
                CHECK(p.qr && strcmp(p.qr, want) == 0, "QR carries the address as encoded");
                CHECK(oled_qr_fits(p.version, p.scale), "QR v%u x%u fits", p.version, p.scale);
                CHECK(scans_to(&p, want), "QR page %d scans back", pg);
                continue;
            }
            for (int r = 0; r < ZRECV_ROWS; r++) CHECK(strlen(p.row[r]) <= ZRECV_COLS, "row width");
            CHECK(strstr(p.row[0], v->network == 0 ? "MAIN" : "TEST") != NULL, "network named");
            if (ua_qr < 0) {
                for (int r = 1; r <= 6; r++) strcat(ua, p.row[r]);
            } else {
                t_text = pg;
                strcat(t, p.row[2]);
                strcat(t, p.row[3]);
                CHECK(strstr(p.row[5], "PUBLIC") != NULL, "t-address called public");
            }
        }
        CHECK(strcmp(ua, v->ua) == 0, "UA reassembles:\n      %s\n      %s", ua, v->ua);
        CHECK(strcmp(t, v->transparent_address) == 0, "t-address reassembles");
        CHECK(ua_qr == n - 3 && t_text == n - 2 && t_qr == n - 1, "page order");
        CHECK(zrecv_toggle_qr(&a, 0) == ua_qr && zrecv_toggle_qr(&a, ua_qr) == 0 &&
              zrecv_toggle_qr(&a, ua_qr - 1) == ua_qr &&
              zrecv_toggle_qr(&a, t_text) == t_qr && zrecv_toggle_qr(&a, t_qr) == t_text,
              "OK toggles text and QR of the same address");
    }
}

static void test_qr_fit(void)
{
    ctx = "qr fit";
    uint8_t v, s;
    CHECK(zrecv_qr_fit(35, &v, &s) && v == 3 && s == 2, "t-address: v3 x2, got v%u x%u", v, s);
    CHECK(zrecv_qr_fit(145, &v, &s) && v == 7 && s == 1, "testnet UA: v7 x1, got v%u x%u", v, s);
    CHECK(zrecv_qr_fit(271, &v, &s) && v == 10 && s == 1, "v10 ceiling");
    CHECK(!zrecv_qr_fit(272, &v, &s), "beyond v10 refused");
}

static void test_fingerprint(void)
{
    uint8_t fp[32];
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const zv_key_vector *v = &ZV_KEYS[i];
        ctx = v->name;
        cur_mnemonic = v->mnemonic;
        cur_passphrase = v->passphrase;
        CHECK(zcash_seed_fingerprint(fp) == WALLET_OK, "derives");
        CHECK(memcmp(fp, v->seed_fingerprint, 32) == 0, "matches librustzcash");
    }

    /* Done-when of 1.5: different passphrases, different fingerprints. */
    ctx = "passphrases";
    char seen[3][ZCASH_FP_CHARS + 1];
    const char *pps[3] = { "", "TREZOR", "LeekWallet Zcash 2026!" };
    cur_mnemonic = ZV_KEYS[0].mnemonic;
    for (int i = 0; i < 3; i++) {
        cur_passphrase = pps[i];
        zcash_seed_fingerprint(fp);
        zcash_short_fp(fp, seen[i]);
        CHECK(strlen(seen[i]) == ZCASH_FP_CHARS, "eight characters");
        printf("    passphrase \"%s\": ZFP %s\n", pps[i], seen[i]);
    }
    CHECK(strcmp(seen[0], seen[1]) && strcmp(seen[0], seen[2]) && strcmp(seen[1], seen[2]),
          "short forms differ");
    zcash_short_fp(ZV_KEYS[0].seed_fingerprint, seen[0]);
    CHECK(strcmp(seen[0], "E62855DC") == 0, "short form is the first four bytes: %s", seen[0]);

    ctx = "locked";
    locked = true;
    memset(fp, 0xAA, sizeof fp);
    CHECK(zcash_seed_fingerprint(fp) == WALLET_ERROR_LOCKED, "refused");
    static const uint8_t zero[32];
    CHECK(memcmp(fp, zero, 32) == 0, "nothing left in the output");
    locked = false;
}

int main(void)
{
    have_zbar = system("command -v zbarimg >/dev/null 2>&1") == 0;
    printf("test_zcash_receive%s\n", have_zbar ? "" : " (zbarimg missing: QR scan SKIPPED)");
    test_snapshot();
    test_every_vector();
    test_qr_fit();
    test_fingerprint();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
