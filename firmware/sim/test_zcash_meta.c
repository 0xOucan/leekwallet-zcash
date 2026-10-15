/* Per-account metadata (src/zcash-meta.c) through the real vault.
 *
 * - a stored wallet's record survives a reboot;
 * - a passphrase, temporary or SD seed's record stays in RAM: not its
 *   fingerprint, not its birthday reach flash, and a lock forgets it;
 * - records follow the seed: another seed does not see them;
 * - a power cut at any write leaves the old record or the new one;
 * - a damaged record reads as no record.
 */
#include <stdio.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "nvs.h"
#include "zcash-meta.h"
#include "zcash-fp.h"
#include "../../vectors/keys_vectors.h"

void wallet__reset_static_state_for_test(void);

static int failures;
static const char *ctx = "setup";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static const char *PIN = "1234";

static void boot(bool wipe)
{
    if (wipe) fake_nvs_reset(); else fake_nvs_reboot();
    wallet__reset_static_state_for_test();
    zmeta__reset_for_test();
    wallet_init();
    if (wipe) CHECK(wallet_set_password(PIN, strlen(PIN)) == WALLET_OK, "password");
    CHECK(wallet_unlock(PIN, strlen(PIN)) == WALLET_OK, "unlock");
}

static ZcashMeta meta(uint32_t birthday, const char *label)
{
    ZcashMeta m;
    memset(&m, 0, sizeof m);
    m.birthday = birthday;
    snprintf(m.label, sizeof m.label, "%s", label);
    return m;
}

static void test_stored_wallet_persists(void)
{
    printf("== a stored wallet's record survives a reboot, per account\n");
    ctx = "stored";
    boot(true);
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    CHECK(wallet_select_wallet(slot) == WALLET_OK, "select");

    ZcashMeta m = meta(2900000, "Main"), got;
    CHECK(!zmeta_get(0, &got), "a record before any was set");
    CHECK(zmeta_set(0, &m) == ZMETA_OK_STORED, "set acct 0");
    ZcashMeta m1 = meta(3100000, "Other");
    m1.flags = ZMETA_FLAG_USE_QSK;
    CHECK(zmeta_set(1, &m1) == ZMETA_OK_STORED, "set acct 1");

    boot(false);
    wallet_select_wallet(slot);
    CHECK(zmeta_get(0, &got) && memcmp(&got, &m, sizeof m) == 0, "acct 0 after reboot");
    CHECK(zmeta_get(1, &got) && memcmp(&got, &m1, sizeof m1) == 0, "acct 1 after reboot");
    CHECK(!zmeta_get(2, &got), "acct 2 invented");

    ZcashMeta m2 = meta(3200000, "");
    CHECK(zmeta_set(0, &m2) == ZMETA_OK_STORED && zmeta_get(0, &got) && got.birthday == 3200000 &&
          got.label[0] == '\0', "replace");

    /* Another seed sees none of it. */
    uint8_t other = wallet_add_mnemonic(ZV_KEYS[12].mnemonic);   /* zoo ... vote */
    CHECK(wallet_select_wallet(other) == WALLET_OK, "select other");
    CHECK(!zmeta_get(0, &got), "another seed read the first seed's record");
}

static void test_hidden_and_session_seeds_stay_in_ram(void)
{
    printf("== passphrase and temporary seeds: RAM only, gone on lock\n");
    ctx = "passphrase";
    boot(true);
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    wallet_select_wallet(slot);
    CHECK(wallet_set_passphrase("TREZOR", 6) == WALLET_OK, "passphrase");
    uint8_t fp[32];
    CHECK(zcash_seed_fingerprint(fp) == WALLET_OK, "fp");
    char short_fp[9];
    zcash_short_fp(fp, short_fp);

    int writes = fake_nvs_write_count();
    ZcashMeta m = meta(0x0BADF00D, "Hidden"), got;
    CHECK(zmeta_set(0, &m) == ZMETA_OK_SESSION, "a hidden wallet's record went to flash");
    CHECK(fake_nvs_write_count() == writes, "a flash write happened");
    CHECK(!fake_nvs_contains_bytes(short_fp, 8), "the hidden wallet's fingerprint is in flash");
    CHECK(!fake_nvs_contains_bytes(fp, 4), "its raw fingerprint is in flash");
    CHECK(zmeta_get(0, &got) && got.birthday == 0x0BADF00D, "readable this session");

    /* The base wallet does not see the hidden one's record. */
    wallet_clear_passphrase();
    CHECK(!zmeta_get(0, &got), "the base wallet read the hidden record");
    wallet_set_passphrase("TREZOR", 6);
    zmeta_forget_session();   /* what lock_device() calls */
    CHECK(!zmeta_get(0, &got), "a lock did not forget it");

    ctx = "temporary";
    boot(true);
    CHECK(wallet_use_temporary_mnemonic(ZV_KEYS[24].mnemonic) == WALLET_OK, "temp");  /* legal winner */
    writes = fake_nvs_write_count();
    CHECK(zmeta_set(0, &m) == ZMETA_OK_SESSION, "a session seed's record went to flash");
    CHECK(fake_nvs_write_count() == writes, "a flash write happened");
    CHECK(zmeta_get(0, &got) && strcmp(got.label, "Hidden") == 0, "readable this session");
    uint8_t needle[4] = { 0x0D, 0xF0, 0xAD, 0x0B };
    CHECK(!fake_nvs_contains_bytes(needle, 4), "the birthday is in flash");

    ctx = "no seed";
    boot(true);
    CHECK(zmeta_set(0, &m) == ZMETA_E_NO_SEED, "set with no seed");
    CHECK(!zmeta_get(0, &got), "get with no seed");
}

static void test_power_cut_and_damage(void)
{
    printf("== a power cut leaves old or new; a damaged record is none\n");
    ctx = "power cut";
    boot(true);
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    wallet_select_wallet(slot);
    ZcashMeta old = meta(1000, "old"), neu = meta(2000, "new"), got;
    CHECK(zmeta_set(0, &old) == ZMETA_OK_STORED, "setup");
    for (int cut = 0; cut <= 2; cut++) {
        CHECK(zmeta_set(0, &old) == ZMETA_OK_STORED, "reset");
        fake_nvs_crash_after(cut);
        zmeta_set(0, &neu);
        boot(false);
        wallet_select_wallet(slot);
        CHECK(zmeta_get(0, &got) &&
              (memcmp(&got, &old, sizeof got) == 0 || memcmp(&got, &neu, sizeof got) == 0),
              "cut after %d writes: neither old nor new", cut);
    }

    ctx = "damaged";
    uint8_t fp[32];
    zcash_seed_fingerprint(fp);
    char sfp[9], key[16];
    zcash_short_fp(fp, sfp);
    snprintf(key, sizeof key, "%s.0", sfp);
    nvs_handle_t h;
    nvs_open("zc_meta", NVS_READWRITE, &h);
    const uint8_t bad[][9] = {
        { 2, 1, 0, 0, 0, 0, 0 },          /* unknown version */
        { 1, 1, 0, 0, 0, 0, 5, 'a', 'b' }, /* label longer than the blob */
        { 1, 1, 0, 0, 0, 4, 0 },          /* unknown flag */
        { 1, 1, 0, 0, 0, 0, 2, 'a', '\n' }, /* control character */
    };
    const size_t lens[] = { 7, 9, 7, 9 };
    for (size_t i = 0; i < 4; i++) {
        nvs_set_blob(h, key, bad[i], lens[i]);
        nvs_commit(h);
        CHECK(!zmeta_get(0, &got), "damaged record %zu accepted", i);
        static const ZcashMeta zero;
        CHECK(memcmp(&got, &zero, sizeof got) == 0, "damaged record %zu left output", i);
    }
    nvs_close(h);

    ctx = "args";
    ZcashMeta m = meta(1, "tab\there");
    CHECK(zmeta_set(0, &m) == ZMETA_E_ARG, "control character in label");
    m = meta(1, "");
    m.flags = 0x80;
    CHECK(zmeta_set(0, &m) == ZMETA_E_ARG, "unknown flag");
    CHECK(zmeta_set(0x80000000u, &old) == ZMETA_E_ARG, "hardened account");
    CHECK(zmeta_set(100000, &old) == ZMETA_OK_SESSION, "an account too large for a key is RAM only");
}

int main(void)
{
    printf("== test_zcash_meta\n");
    test_stored_wallet_persists();
    test_hidden_and_session_seeds_stay_in_ram();
    test_power_cut_and_damage();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
