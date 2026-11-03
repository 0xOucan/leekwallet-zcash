/**
 * The KDF migration, cut at every write (docs/AUDIT-SD-WRITES.md, N2).
 *
 * A v1 or v2 vault is moved to v3 by the first unlock after an upgrade. NVS
 * writes each value as it is set - nvs_commit() groups nothing - so the
 * migration is a sequence of separately durable writes, and a power cut can
 * land between any two. The old code wrote the version marker before the
 * verifier and, for v2, replaced the only salt the blobs could be read with:
 * a cut in that window refused the right PIN (spending attempts towards a
 * wipe) and could leave seeds no key would ever open again.
 *
 * Here a v1 and a v2 vault are built byte for byte as those firmwares wrote
 * them - CBC blobs, unsalted or salted - and the first unlock is cut after
 * every write, and then cut again during the resumed migration. After each,
 * the same PIN must verify (so pin.c would not charge an attempt), unlock,
 * and open every wallet onto its own phrase; and a clean unlock must leave
 * the vault at v3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "mbedtls/aes.h"
#include "vault-kdf.h"

void wallet__reset_static_state_for_test(void);

static int failures = 0;
static char ctx[96] = "setup";

#define CHECK(cond, ...) do {                                 \
    if (!(cond)) {                                            \
        printf("  FAIL %s: ", ctx); printf(__VA_ARGS__);      \
        printf("\n"); failures++;                             \
    }                                                         \
} while (0)

static const char *SEEDS[] = {
    "legal winner thank year wave sausage worth useful legal winner thank yellow",
    "letter advice cage absurd amount doctor acoustic avoid letter advice cage above",
};
#define N_SEEDS 2
#define NS "colibri"
#define PIN "112233"

static const uint8_t SALT_V2[VAULT_SALT_SIZE] = {
    0x51, 0x7a, 0x02, 0x9c, 0x44, 0xe1, 0x3b, 0x80, 0x0d, 0x6f, 0x27, 0xa5, 0x19, 0xc8, 0x73, 0xbe,
};

static void put_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    CHECK(nvs_set_blob(h, key, v, len) == ESP_OK, "setup: %s", key);
}

/* A mnemonic as the CBC firmwares stored it: NUL-terminated, zero-padded to
 * the block, AES-256-CBC under `key`, with its IV under a key of its own. */
static void put_cbc(nvs_handle_t h, const char *mkey, const char *ivkey, const char *mnemonic,
                    const uint8_t key[32], uint8_t ivseed)
{
    size_t len = strlen(mnemonic) + 1, padded = (len + 15) / 16 * 16;
    uint8_t *plain = calloc(1, padded), *ct = calloc(1, padded);
    memcpy(plain, mnemonic, len);
    uint8_t iv[16], ivc[16];
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(ivseed + i);
    memcpy(ivc, iv, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 256);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, ivc, plain, ct);
    mbedtls_aes_free(&aes);
    put_blob(h, mkey, ct, padded);
    put_blob(h, ivkey, iv, 16);
    free(plain);
    free(ct);
}

/* v1: no salt, no marker, an unsalted verifier under pwd_hash, m_N/iv_N. */
static void make_v1(void)
{
    fake_nvs_reset();
    uint8_t nosalt[VAULT_SALT_SIZE] = {0}, ver[VAULT_HASH_SIZE], key[VAULT_KEY_SIZE];
    vault_derive_verifier(VAULT_KDF_V1_LEGACY, PIN, strlen(PIN), nosalt, ver);
    vault_derive_key(VAULT_KDF_V1_LEGACY, PIN, strlen(PIN), nosalt, key);
    nvs_handle_t h;
    nvs_open(NS, NVS_READWRITE, &h);
    put_blob(h, "pwd_hash", ver, sizeof ver);
    for (int i = 1; i <= N_SEEDS; i++) {
        char m[16], iv[16];
        snprintf(m, sizeof m, "m_%d", i);
        snprintf(iv, sizeof iv, "iv_%d", i);
        put_cbc(h, m, iv, SEEDS[i - 1], key, (uint8_t)(i * 16));
    }
    nvs_set_u8(h, "wallet_cnt", N_SEEDS);
    nvs_set_u8(h, "active_idx", 1);
    nvs_close(h);
}

/* v2: a salt and kdf_ver 2, no parameter blob (the defaults), a generation
 * record holding the salted verifier, CBC blobs under m0_N/iv0_N. */
static void make_v2(void)
{
    fake_nvs_reset();
    uint8_t ver[VAULT_HASH_SIZE], key[VAULT_KEY_SIZE];
    vault_derive_verifier(VAULT_KDF_V2, PIN, strlen(PIN), SALT_V2, ver);
    vault_derive_key(VAULT_KDF_V2, PIN, strlen(PIN), SALT_V2, key);
    uint8_t rec[69] = { 1, 0, 0, 0 };   /* version 1, generation 0 */
    memcpy(rec + 4, ver, sizeof ver);
    nvs_handle_t h;
    nvs_open(NS, NVS_READWRITE, &h);
    nvs_set_u8(h, "kdf_ver", 2);
    put_blob(h, "kdf_salt", SALT_V2, sizeof SALT_V2);
    put_blob(h, "vault_rec", rec, sizeof rec);
    for (int i = 1; i <= N_SEEDS; i++) {
        char m[16], iv[16];
        snprintf(m, sizeof m, "m0_%d", i);
        snprintf(iv, sizeof iv, "iv0_%d", i);
        put_cbc(h, m, iv, SEEDS[i - 1], key, (uint8_t)(i * 16));
    }
    nvs_set_u8(h, "wallet_cnt", N_SEEDS);
    nvs_set_u8(h, "active_idx", 1);
    nvs_close(h);
}

static void reboot(void)
{
    fake_nvs_reboot();
    wallet__reset_static_state_for_test();
    wallet_init();
}

static uint8_t marker(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "kdf_ver", &v) != ESP_OK) v = 1;
        nvs_close(h);
    }
    return v;
}

/* The PIN is accepted the way pin.c asks first, then unlocks, then every
 * wallet opens onto its own phrase. */
static bool opens_all(void)
{
    if (wallet_storage_unreadable() || !wallet_verify_password(PIN, strlen(PIN))) return false;
    if (wallet_unlock(PIN, strlen(PIN)) != WALLET_OK) return false;
    for (int i = 1; i <= N_SEEDS; i++) {
        char got[256];
        if (wallet_select_wallet((uint8_t)i) != WALLET_OK ||
            wallet_get_mnemonic(got, sizeof got) != WALLET_OK || strcmp(got, SEEDS[i - 1]) != 0) {
            return false;
        }
    }
    wallet_lock();
    return true;
}

static void sweep(const char *name, void (*make)(void))
{
    printf("== %s -> v3, cut after every write, then again on the retry\n", name);
    make();
    reboot();
    int before = fake_nvs_write_count();
    CHECK(wallet_unlock(PIN, strlen(PIN)) == WALLET_OK, "%s: clean unlock", name);
    int total = fake_nvs_write_count() - before;
    reboot();
    snprintf(ctx, sizeof ctx, "%s clean", name);
    CHECK(marker() == VAULT_KDF_CURRENT, "not at v3 after a clean unlock: v%u", marker());
    CHECK(opens_all(), "the migrated vault does not open");

    int runs = 0;
    for (int a = 0; a <= total; a++) {
        for (int b = -1; b <= total; b++) {
            snprintf(ctx, sizeof ctx, "%s, cut after write %d/%d, retry cut %d", name, a, total, b);
            make();
            reboot();
            fake_nvs_crash_after(a);
            wallet_unlock(PIN, strlen(PIN));
            reboot();
            if (b >= 0) {
                /* The retry is cut too: the resumed migration must be just
                 * as safe as the first. */
                fake_nvs_crash_after(b);
                wallet_unlock(PIN, strlen(PIN));
                reboot();
            }
            CHECK(opens_all(), "the PIN no longer opens every wallet");
            reboot();
            CHECK(opens_all(), "the second boot does not open every wallet");
            CHECK(marker() == VAULT_KDF_CURRENT, "not at v3 after a clean unlock: v%u", marker());
            runs++;
        }
    }
    printf("  %d writes, %d cut runs\n", total, runs);
}

int main(void)
{
    printf("== test_vault_migrate\n");
    snprintf(ctx, sizeof ctx, "v1 baseline");
    make_v1();
    reboot();
    CHECK(marker() == 1 && opens_all(), "a v1 vault built here does not open");
    snprintf(ctx, sizeof ctx, "v2 baseline");
    make_v2();
    reboot();
    CHECK(marker() == 2, "a v2 vault built here is not v2");
    sweep("v1", make_v1);
    sweep("v2", make_v2);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
