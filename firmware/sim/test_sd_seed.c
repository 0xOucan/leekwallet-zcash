/* SD card storage mode (roadmap 1.4), end to end through the real vault, the
 * real Zcash derivation and a host directory standing in for the card.
 *
 * Done-when of 1.4:
 *   - vault, stateless and SD give the same UFVK for the same phrase (every
 *     key vector, passphrases applied on top as on the device);
 *   - the sim has a fake SD backend (sdcard.c on a directory);
 *   - a power cut during a card write leaves no half file under a real name;
 *   - a wrong passphrase fails cleanly and loads nothing.
 * Plus: loading from a card leaves no trace of the phrase in flash, and the
 * golden file written by the C suite (and reproduced by zecz) loads here.
 */
#define _DEFAULT_SOURCE   /* mkdtemp */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bip39.h"
#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "sd-seed.h"
#include "zcash-account.h"
#include "../../vectors/keys_vectors.h"

void wallet__reset_static_state_for_test(void);

static int failures;
static const char *ctx = "setup";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static const char *PIN = "1234";
static const char *PASS = "card passphrase 1";
#define IT ZKV_ITERATIONS_MIN

static char card[256];

static void empty_card(void)
{
    DIR *d = opendir(card);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", card, e->d_name);
        unlink(p);
    }
    if (d) closedir(d);
    sdcard__set_root_for_test(card);
}

static void fresh_device(void)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    CHECK(wallet_set_password(PIN, strlen(PIN)) == WALLET_OK, "no password");
    CHECK(wallet_unlock(PIN, strlen(PIN)) == WALLET_OK, "no unlock");
}

static void derive_and_check(const zv_key_vector *v, const char *mode)
{
    if (v->passphrase[0]) {
        wallet_set_passphrase(v->passphrase, strlen(v->passphrase));
    } else {
        wallet_clear_passphrase();
    }
    static ZcashAccount a;
    CHECK(zcash_account_derive(v->account, v->network, &a) == WALLET_OK, "%s: derive", mode);
    CHECK(strcmp(a.keys.ufvk, v->ufvk) == 0, "%s: UFVK differs", mode);
    CHECK(strcmp(a.keys.ua, v->ua) == 0, "%s: UA differs", mode);
    CHECK(memcmp(a.keys.seed_fingerprint, v->seed_fingerprint, 32) == 0, "%s: fingerprint", mode);
}

static void test_three_modes_agree(void)
{
    printf("== vault, stateless and SD give the same keys for every vector\n");
    int phrases = 0;
    for (size_t i = 0; i < ZV_KEYS_COUNT;) {
        const char *m = ZV_KEYS[i].mnemonic;
        size_t j = i;
        while (j < ZV_KEYS_COUNT && strcmp(ZV_KEYS[j].mnemonic, m) == 0) j++;
        ctx = ZV_KEYS[i].name;
        phrases++;

        /* Vault: stored in a slot. Then saved from there to the card. */
        fresh_device();
        empty_card();
        uint8_t slot = wallet_add_mnemonic(m);
        CHECK(slot >= 1 && wallet_select_wallet(slot) == WALLET_OK, "store");
        for (size_t k = i; k < j; k++) derive_and_check(&ZV_KEYS[k], "vault");
        wallet_clear_passphrase();
        char name[SDCARD_NAME_MAX + 1];
        SdSeedResult r = sdseed_save(PASS, strlen(PASS), "vec", 2900000, 0, IT, name);
        CHECK(r == SDSEED_OK && strcmp(name, "SEED1.ZKV") == 0, "save: %s %s", sdseed_strerror(r), name);

        /* Stateless: typed for the session. */
        fresh_device();
        CHECK(wallet_use_temporary_mnemonic(m) == WALLET_OK, "temporary");
        for (size_t k = i; k < j; k++) derive_and_check(&ZV_KEYS[k], "stateless");

        /* SD: a different device, nothing stored, the card and its passphrase. */
        fresh_device();
        sdcard__set_root_for_test(card);
        ZkvContents meta;
        r = sdseed_load("SEED1.ZKV", PASS, strlen(PASS), &meta);
        CHECK(r == SDSEED_OK, "load: %s", sdseed_strerror(r));
        CHECK(wallet_has_temporary_mnemonic(), "an SD seed is a session seed");
        CHECK(meta.birthday == 2900000 && strcmp(meta.label, "vec") == 0, "metadata");
        static const uint8_t zero[ZKV_ENTROPY_MAX];
        CHECK(memcmp(meta.entropy, zero, sizeof zero) == 0, "entropy handed back to the caller");
        for (size_t k = i; k < j; k++) derive_and_check(&ZV_KEYS[k], "sd");

        CHECK(!fake_nvs_contains_bytes(m, strlen(m)), "the phrase is in flash");
        i = j;
    }
    printf("  %d phrases, %zu vectors, three modes each\n", phrases, ZV_KEYS_COUNT);
}

/* The vectors' entropies are runs of one byte (abandon is all zeros), which
 * flash holds anyway; a random phrase is the meaningful search. */
static void test_nothing_reaches_flash(void)
{
    printf("== a seed loaded from the card leaves nothing in flash\n");
    ctx = "no trace";
    fresh_device();
    empty_card();
    const char *m = mnemonic_generate(256);
    char phrase[256];
    snprintf(phrase, sizeof phrase, "%s", m);
    mnemonic_clear();
    CHECK(wallet_use_temporary_mnemonic(phrase) == WALLET_OK, "temporary");
    char name[SDCARD_NAME_MAX + 1];
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_OK, "save");

    fresh_device();
    sdcard__set_root_for_test(card);
    ZkvContents meta;
    CHECK(sdseed_load(name, PASS, strlen(PASS), &meta) == SDSEED_OK, "load");
    static ZcashAccount a;
    CHECK(zcash_account_derive(0, ZCASH_NET_TEST, &a) == WALLET_OK, "derive");

    uint8_t bits[33];
    int nbits = mnemonic_to_bits(phrase, bits);
    CHECK(nbits == 264, "setup");
    CHECK(!fake_nvs_contains_bytes(phrase, strlen(phrase)), "the phrase is in flash");
    CHECK(!fake_nvs_contains_bytes(bits, 32), "the entropy is in flash");
    CHECK(!fake_nvs_contains_bytes(bits, 8), "eight bytes of the entropy are in flash");
}

static void test_wrong_passphrase_loads_nothing(void)
{
    printf("== a wrong passphrase fails cleanly\n");
    ctx = "wrong passphrase";
    fresh_device();
    empty_card();
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    wallet_select_wallet(slot);
    char name[SDCARD_NAME_MAX + 1];
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_OK, "save");

    /* The stored wallet stays the open one. */
    uint32_t before = 0, after = 0;
    wallet_get_master_fingerprint(&before);
    ZkvContents meta;
    memset(&meta, 0xA5, sizeof meta);
    CHECK(sdseed_load(name, "card passphrase 2", 17, &meta) == SDSEED_E_AUTH, "refused");
    CHECK(!wallet_has_temporary_mnemonic(), "something was loaded");
    wallet_get_master_fingerprint(&after);
    CHECK(before == after && before != 0, "the open wallet changed");
    static const ZkvContents zero;
    CHECK(memcmp(&meta, &zero, sizeof meta) == 0, "metadata not cleared");
    CHECK(sdseed_load(name, "short", 5, &meta) == SDSEED_E_AUTH, "short passphrase");
    CHECK(sdseed_load("NOPE.ZKV", PASS, strlen(PASS), &meta) == SDSEED_E_IO, "missing file");
}

static void test_power_cut_and_no_card(void)
{
    printf("== a power cut mid-write leaves no seed file; no card is said plainly\n");
    ctx = "power cut";
    fresh_device();
    empty_card();
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    wallet_select_wallet(slot);
    char name[SDCARD_NAME_MAX + 1];
    sdcard__fail_next_rename_for_test();
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_E_IO, "write reported");
    char names[SDCARD_FILES_MAX][SDCARD_NAME_MAX + 1];
    CHECK(sdseed_list(names, SDCARD_FILES_MAX) == 0, "a seed file appeared");
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s/ZKVWRITE.TMP", card);
    struct stat st;
    CHECK(stat(tmp, &st) == 0, "setup: the cut should leave the temporary");
    sdcard__set_root_for_test(card);   /* reinserted, or rebooted */
    CHECK(sdseed_list(names, SDCARD_FILES_MAX) == 0 && stat(tmp, &st) != 0,
          "the unfinished write survived a remount");
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_OK &&
          strcmp(name, "SEED1.ZKV") == 0, "the next save takes the first name");
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_OK &&
          strcmp(name, "SEED2.ZKV") == 0, "and never overwrites: %s", name);
    CHECK(sdseed_list(names, SDCARD_FILES_MAX) == 2, "two files listed");

    ctx = "no card";
    sdcard__set_root_for_test(NULL);
    CHECK(sdseed_list(names, SDCARD_FILES_MAX) == -1, "listed without a card");
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_E_NOCARD, "saved without a card");
    ZkvContents meta;
    CHECK(sdseed_load("SEED1.ZKV", PASS, strlen(PASS), &meta) == SDSEED_E_NOCARD, "loaded without a card");

    ctx = "locked";
    sdcard__set_root_for_test(card);
    wallet_lock();
    CHECK(sdseed_load("SEED1.ZKV", PASS, strlen(PASS), &meta) == SDSEED_E_WALLET, "loaded while locked");
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_E_WALLET, "saved while locked");
}

/* The bytes of a file on the card, or 0 if it is not there. */
static size_t card_bytes(const char *name, uint8_t *buf, size_t cap)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", card, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* What a reboot must find after any failed save: the earlier backup byte for
 * byte, no temporary, and no other seed file at all (a save that reported
 * an error must not leave a name behind, whole or not). */
static void check_after_failed_save(const uint8_t *old, size_t old_len, const char *what)
{
    sdcard__set_root_for_test(card);   /* rebooted, card back in */
    char names[SDCARD_FILES_MAX][SDCARD_NAME_MAX + 1];
    int n = sdseed_list(names, SDCARD_FILES_MAX);
    CHECK(n == 1 && strcmp(names[0], "SEED1.ZKV") == 0, "%s: %d files listed", what, n);
    uint8_t now[ZKV_FILE_MAX + 1];
    size_t len = card_bytes("SEED1.ZKV", now, sizeof now);
    CHECK(len == old_len && memcmp(now, old, len) == 0, "%s: the earlier backup changed", what);
    CHECK(card_bytes("ZKVWRITE.TMP", now, sizeof now) == 0, "%s: a temporary survived", what);
    ZkvContents meta;
    CHECK(sdseed_load("SEED1.ZKV", PASS, strlen(PASS), &meta) == SDSEED_OK,
          "%s: the earlier backup no longer opens", what);
}

/* docs/AUDIT-SD-WRITES.md: every step of a save, broken every way the fake
 * card knows. None may cost the backup already on the card, and none may
 * leave a seed file that is not a whole, openable one. */
static void test_save_broken_at_every_step(void)
{
    printf("== a save broken at any step: the old backup stands, no half file\n");
    ctx = "every step";
    fresh_device();
    empty_card();
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    wallet_select_wallet(slot);
    char name[SDCARD_NAME_MAX + 1];
    CHECK(sdseed_save(PASS, strlen(PASS), "old", 0, 0, IT, name) == SDSEED_OK, "first save");
    uint8_t old[ZKV_FILE_MAX + 1];
    size_t old_len = card_bytes("SEED1.ZKV", old, sizeof old);

    /* How many steps a clean save takes, so every one of them is tried. */
    sdcard__set_root_for_test(card);
    sdcard__fault_at_step_for_test(0, SDCARD_FAULT_NONE);
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_OK, "counting save");
    int steps = sdcard__steps_for_test();
    CHECK(steps >= 5, "a save is only %d steps", steps);
    char p[512];
    snprintf(p, sizeof p, "%s/%s", card, name);
    unlink(p);

    static const struct { SdcardFault kind; const char *name; SdSeedResult expect; } kinds[] = {
        { SDCARD_FAULT_FAIL, "fail", SDSEED_E_IO },
        { SDCARD_FAULT_CUT,  "cut",  SDSEED_E_IO },
        { SDCARD_FAULT_TEAR, "tear", SDSEED_E_IO },
    };
    int tried = 0;
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        for (int step = 1; step <= steps; step++) {
            char what[48];
            snprintf(what, sizeof what, "%s at step %d", kinds[k].name, step);
            sdcard__set_root_for_test(card);
            sdcard__fault_at_step_for_test(step, kinds[k].kind);
            SdSeedResult r = sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name);
            CHECK(r == kinds[k].expect, "%s: %s", what, sdseed_strerror(r));
            check_after_failed_save(old, old_len, what);
            tried++;
        }
    }

    /* A card that stores a flipped bit and says it worked: caught by the
     * read-back, and the name it was given is taken away again. */
    for (int step = 1; step <= steps; step++) {
        char what[48];
        snprintf(what, sizeof what, "flip at step %d", step);
        sdcard__set_root_for_test(card);
        sdcard__fault_at_step_for_test(step, SDCARD_FAULT_FLIP);
        SdSeedResult r = sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name);
        if (r == SDSEED_OK) {
            /* Not a write step: the save is whole. Take it off again. */
            snprintf(p, sizeof p, "%s/%s", card, name);
            unlink(p);
            continue;
        }
        CHECK(r == SDSEED_E_VERIFY, "%s: %s", what, sdseed_strerror(r));
        check_after_failed_save(old, old_len, what);
        tried++;
    }

    /* A full card, at sizes short of the file (label "" is three bytes
     * shorter than the "old" one above). */
    const long need = (long)old_len - 3;
    const long room[] = { 0, 1, 37, 38, need / 2, need - 1 };
    for (size_t i = 0; i < sizeof room / sizeof *room; i++) {
        char what[48];
        snprintf(what, sizeof what, "full card at %ld bytes", room[i]);
        sdcard__set_root_for_test(card);
        sdcard__card_space_for_test(room[i]);
        SdSeedResult r = sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name);
        CHECK(r == SDSEED_E_IO, "%s: %s", what, sdseed_strerror(r));
        check_after_failed_save(old, old_len, what);
        tried++;
    }
    printf("  %d broken saves, %d steps each\n", tried, steps);
}

/* A file that is damaged on the card is an error the user sees, never an
 * empty card and never a different seed. */
static void test_damaged_file_is_an_error(void)
{
    printf("== a truncated, empty or garbage seed file is refused, not skipped\n");
    ctx = "damaged";
    fresh_device();
    empty_card();
    uint8_t slot = wallet_add_mnemonic(ZV_KEYS[0].mnemonic);
    wallet_select_wallet(slot);
    char name[SDCARD_NAME_MAX + 1];
    CHECK(sdseed_save(PASS, strlen(PASS), "", 0, 0, IT, name) == SDSEED_OK, "save");
    uint8_t good[ZKV_FILE_MAX + 1];
    size_t len = card_bytes(name, good, sizeof good);

    char p[512];
    snprintf(p, sizeof p, "%s/%s", card, name);
    static const size_t cuts[] = { 0, 1, 4, 37, 38, 54, 0 };
    for (size_t i = 0; i < sizeof cuts / sizeof *cuts; i++) {
        size_t keep = i + 1 == sizeof cuts / sizeof *cuts ? len - 1 : cuts[i];
        FILE *f = fopen(p, "wb");
        fwrite(good, 1, keep, f);
        fclose(f);
        char names[SDCARD_FILES_MAX][SDCARD_NAME_MAX + 1];
        CHECK(sdseed_list(names, SDCARD_FILES_MAX) == 1, "a %zu-byte file is not listed", keep);
        ZkvContents meta;
        SdSeedResult r = sdseed_load(name, PASS, strlen(PASS), &meta);
        CHECK(r == SDSEED_E_FORMAT || r == SDSEED_E_AUTH, "%zu bytes: %s", keep, sdseed_strerror(r));
        CHECK(!wallet_has_temporary_mnemonic(), "%zu bytes: something was loaded", keep);
    }
    /* Garbage of the right length: the tag fails. */
    uint8_t junk[ZKV_FILE_MAX];
    memcpy(junk, good, len);
    junk[len - 20] ^= 0x40;
    FILE *f = fopen(p, "wb");
    fwrite(junk, 1, len, f);
    fclose(f);
    ZkvContents meta;
    CHECK(sdseed_load(name, PASS, strlen(PASS), &meta) == SDSEED_E_AUTH, "a flipped bit opened");
    CHECK(!wallet_has_temporary_mnemonic(), "garbage loaded something");
}

static void test_golden_from_the_other_side(void)
{
    printf("== the golden file (also written by zecz) loads on the device\n");
    ctx = "golden";
    fresh_device();
    empty_card();
    FILE *in = fopen("../../vectors/zkv/golden-v1.zkv", "rb");
    char p[512];
    snprintf(p, sizeof p, "%s/GOLDEN.ZKV", card);
    FILE *out = fopen(p, "wb");
    uint8_t buf[256];
    size_t n = in ? fread(buf, 1, sizeof buf, in) : 0;
    if (out) fwrite(buf, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);

    ZkvContents meta;
    CHECK(sdseed_load("GOLDEN.ZKV", "LeekWallet zkv test", 19, &meta) == SDSEED_OK, "load");
    CHECK(meta.flags == ZKV_FLAG_USE_QSK && strcmp(meta.label, "LeekWallet test") == 0, "metadata");
    derive_and_check(&ZV_KEYS[1], "golden");   /* test/abandon-art/pp0/acct0 */
}

int main(void)
{
    printf("== test_sd_seed\n");
    snprintf(card, sizeof card, "/tmp/leek-sdcard-XXXXXX");
    if (!mkdtemp(card)) {
        printf("FAILED: no card directory\n");
        return 1;
    }
    test_three_modes_agree();
    test_nothing_reaches_flash();
    test_wrong_passphrase_loads_nothing();
    test_power_cut_and_no_card();
    test_save_broken_at_every_step();
    test_damaged_file_is_an_error();
    test_golden_from_the_other_side();
    empty_card();
    rmdir(card);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
