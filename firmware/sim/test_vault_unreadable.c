/**
 * An unreadable vault is never an empty one (docs/AUDIT-SD-WRITES.md, N1).
 *
 * The bug class, from another wallet's post-mortem: storage that no longer
 * reads is taken for storage that was never written, and the device carries
 * on as a new one. Here that had three doors. wallet_init() and app_main()
 * erased NVS when it would not mount; a vault record of an unknown version
 * fell back to the legacy layout, whose boot sweep then erased every
 * generation slot as stale; and a vault with seeds and no verifier reported
 * "no password", which is what offers PIN setup.
 *
 * Each case below breaks a provisioned vault one way, reboots, and checks
 * the same things: the seeds are still in flash, the device says it has a
 * PIN, nothing will set a new one or spend an attempt, and once the damage
 * is undone the old PIN opens every wallet again - proof nothing was lost.
 * The controls check the other direction: a new device, a legacy one, and
 * a first setup cut at every write are never called unreadable.
 *
 * Links the real components/leek-wallet.c and src/pin.c, as test_pin_change
 * does: the property is about their storage decisions, not a stub's.
 */

#include <stdio.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "pin.h"

void pin__reset_static_state_for_test(void);
void wallet__reset_static_state_for_test(void);

static int failures = 0;
static const char *ctx = "setup";

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
#define SEED_COUNT 2
#define NS "colibri"
#define OLD_PIN "112233"
#define NEW_PIN "445566"

static uint32_t expected_fp[SEED_COUNT];

static void reboot(void)
{
    fake_nvs_reboot();
    wallet__reset_static_state_for_test();
    pin__reset_static_state_for_test();
    wallet_init();
    pin_init();
}

static void provisioned_device(void)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    pin__reset_static_state_for_test();
    wallet_init();
    wallet_set_password(OLD_PIN, strlen(OLD_PIN));
    for (int i = 0; i < SEED_COUNT; i++) {
        wallet_add_mnemonic(SEEDS[i]);
    }
    pin_init();
    pin_set(OLD_PIN);
    for (int i = 0; i < SEED_COUNT; i++) {
        wallet_select_wallet((uint8_t)(i + 1));
        wallet_get_master_fingerprint(&expected_fp[i]);
    }
    wallet_lock();
}

static size_t get_blob(const char *key, uint8_t *buf, size_t cap)
{
    nvs_handle_t h;
    size_t len = cap;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_blob(h, key, buf, &len) != ESP_OK) len = 0;
    nvs_close(h);
    return len;
}

static void set_blob(const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (buf) nvs_set_blob(h, key, buf, len);
    else nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static bool opens_all(const char *pin)
{
    if (wallet_unlock(pin, strlen(pin)) != WALLET_OK) return false;
    for (int i = 0; i < SEED_COUNT; i++) {
        uint32_t fp = 0;
        if (wallet_select_wallet((uint8_t)(i + 1)) != WALLET_OK ||
            wallet_get_master_fingerprint(&fp) != WALLET_OK || fp != expected_fp[i]) {
            return false;
        }
    }
    wallet_lock();
    return true;
}

/* What every unreadable vault must look like after a reboot. */
static void check_fault_state(const uint8_t *rec, size_t rec_len)
{
    CHECK(wallet_storage_unreadable(), "not reported unreadable");
    CHECK(wallet_get_status().password_set, "reported as having no password");
    CHECK(pin_is_set(), "pin_is_set() is false: PIN setup would be offered");
    CHECK(fake_nvs_has(NS, "m0_1") && fake_nvs_has(NS, "m0_2"), "a seed was erased at boot");

    uint8_t before = pin_get_remaining_attempts();
    CHECK(!pin_verify(OLD_PIN), "the PIN verified against a vault that cannot be read");
    CHECK(pin_get_remaining_attempts() == before, "an attempt was spent: %u -> %u",
          before, pin_get_remaining_attempts());
    CHECK(!pin_should_wipe(), "the device is set to wipe");

    CHECK(wallet_set_password(NEW_PIN, strlen(NEW_PIN)) == WALLET_ERROR_STORAGE_FAILED,
          "a new password was accepted over the vault");
    CHECK(!pin_set(NEW_PIN), "pin_set accepted a new PIN");
    CHECK(wallet_unlock(OLD_PIN, strlen(OLD_PIN)) == WALLET_ERROR_STORAGE_FAILED, "unlocked");
    CHECK(!pin_change(OLD_PIN, NEW_PIN), "pin_change ran");

    CHECK(fake_nvs_has(NS, "m0_1") && fake_nvs_has(NS, "m0_2"), "a seed was erased afterwards");
    uint8_t now[128];
    size_t now_len = get_blob("vault_rec", now, sizeof now);
    CHECK(now_len == rec_len && (rec_len == 0 || memcmp(now, rec, rec_len) == 0),
          "the record was rewritten (%zu bytes, was %zu)", now_len, rec_len);
}

static void test_damaged_records(void)
{
    printf("== a vault record this build cannot read: nothing erased, no PIN asked\n");
    static const struct { const char *name; int kind; } cases[] = {
        { "record version 2", 0 },
        { "record longer than this build's", 1 },
        { "record shorter than this build's", 2 },
        { "record generation 7", 3 },
        { "record lost, no verifier", 4 },
        { "record lost, a stale pwd_hash", 5 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        ctx = cases[c].name;
        provisioned_device();
        uint8_t good[128];
        size_t good_len = get_blob("vault_rec", good, sizeof good);
        CHECK(good_len > 36, "no record to break");

        uint8_t bad[160];
        size_t bad_len = good_len;
        memcpy(bad, good, good_len);
        switch (cases[c].kind) {
            case 0: bad[0] = 2; break;
            case 1: memset(bad + good_len, 0, 8); bad_len += 8; break;
            case 2: bad_len = 10; break;
            case 3: bad[1] = 7; break;
            default: bad_len = 0; break;
        }
        set_blob("vault_rec", bad_len ? bad : NULL, bad_len);
        if (cases[c].kind == 5) set_blob("pwd_hash", good + 4, 32);
        int erased = fake_nvs_erase_count();
        reboot();
        check_fault_state(bad, bad_len);
        CHECK(fake_nvs_erase_count() == erased, "NVS was erased");

        /* Put the record back: the same PIN opens every wallet, so the
         * fault cost the owner nothing. */
        set_blob("vault_rec", good, good_len);
        set_blob("pwd_hash", NULL, 0);
        reboot();
        CHECK(!wallet_storage_unreadable(), "still unreadable once repaired");
        CHECK(opens_all(OLD_PIN), "the repaired vault does not open with its PIN");
    }
}

static void test_nvs_will_not_mount(void)
{
    printf("== NVS that will not mount is not erased, and asks no PIN\n");
    static const struct { const char *name; esp_err_t err; } cases[] = {
        { "NO_FREE_PAGES", ESP_ERR_NVS_NO_FREE_PAGES },
        { "NEW_VERSION_FOUND", ESP_ERR_NVS_NEW_VERSION_FOUND },
        { "any other failure", ESP_FAIL },
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        ctx = cases[c].name;
        provisioned_device();
        int erased = fake_nvs_erase_count();
        fake_nvs_fail_init(cases[c].err);
        fake_nvs_reboot();
        wallet__reset_static_state_for_test();
        pin__reset_static_state_for_test();
        CHECK(wallet_init() == WALLET_ERROR_STORAGE_FAILED, "wallet_init succeeded");
        pin_init();
        CHECK(fake_nvs_erase_count() == erased, "NVS was erased");
        CHECK(wallet_storage_unreadable(), "not reported unreadable");
        CHECK(pin_is_set(), "PIN setup would be offered");
        CHECK(wallet_set_password(NEW_PIN, strlen(NEW_PIN)) == WALLET_ERROR_STORAGE_FAILED,
              "a new password was accepted");
        CHECK(!pin_verify(OLD_PIN), "a PIN verified");

        /* Mounts again (a newer firmware, a repaired partition): all there. */
        fake_nvs_fail_init(ESP_OK);
        reboot();
        CHECK(!wallet_storage_unreadable(), "still unreadable once it mounts");
        pin_reset_attempts();
        CHECK(opens_all(OLD_PIN), "the vault does not open once NVS mounts");
    }
}

static void test_readable_vaults_are_not_faults(void)
{
    printf("== new, legacy and half-set-up devices are never called unreadable\n");
    ctx = "new device";
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    pin__reset_static_state_for_test();
    wallet_init();
    pin_init();
    CHECK(!wallet_storage_unreadable(), "a blank device is a fault");
    CHECK(!wallet_get_status().password_set && !pin_is_set(), "a blank device has a PIN");

    ctx = "legacy layout";
    provisioned_device();
    uint8_t rec[128];
    size_t rec_len = get_blob("vault_rec", rec, sizeof rec);
    set_blob("pwd_hash", rec + 4, 32);
    set_blob("vault_rec", NULL, 0);
    for (int i = 1; i <= SEED_COUNT; i++) {
        char src[16], dst[16];
        uint8_t blob[512];
        snprintf(src, sizeof src, "m0_%d", i);
        snprintf(dst, sizeof dst, "m_%d", i);
        size_t len = get_blob(src, blob, sizeof blob);
        set_blob(dst, blob, len);
        set_blob(src, NULL, 0);
    }
    (void)rec_len;
    reboot();
    CHECK(!wallet_storage_unreadable(), "a pre-generation vault is a fault");
    CHECK(opens_all(OLD_PIN), "the legacy vault does not open");

    /* The first setup, cut at every write: whatever survives is a device
     * that can be set up or opened, never a fault. Relies on the vault
     * publishing its password before the first mnemonic. */
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    int before = fake_nvs_write_count();
    wallet_set_password(OLD_PIN, strlen(OLD_PIN));
    wallet_add_mnemonic(SEEDS[0]);
    int total = fake_nvs_write_count() - before;
    for (int cut = 0; cut <= total; cut++) {
        char name[64];
        snprintf(name, sizeof name, "first setup cut after write %d/%d", cut, total);
        ctx = name;
        fake_nvs_reset();
        wallet__reset_static_state_for_test();
        pin__reset_static_state_for_test();
        wallet_init();
        fake_nvs_crash_after(cut);
        wallet_set_password(OLD_PIN, strlen(OLD_PIN));
        wallet_add_mnemonic(SEEDS[0]);
        reboot();
        CHECK(!wallet_storage_unreadable(), "called unreadable");
    }
}

int main(void)
{
    printf("== test_vault_unreadable\n");
    test_readable_vaults_are_not_faults();
    test_damaged_records();
    test_nvs_will_not_mount();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
