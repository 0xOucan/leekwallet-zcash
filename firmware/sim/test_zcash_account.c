/* The device's Zcash account, end to end through the real vault.
 *
 * A seed goes into the vault (stored, or temporary), a passphrase is set the
 * way the UI sets it, and zcash_account_derive() must produce exactly the
 * UFVK, UA, t-address and seed fingerprint librustzcash derives for that
 * mnemonic + passphrase + account + network (vectors/keys_vectors.h).
 */
#include <stdio.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "memzero.h"
#include "zcash-account.h"
#include "../../vectors/keys_vectors.h"

void wallet__reset_static_state_for_test(void);

static int failures;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s: %s\n", ctx, msg); failures++; } } while (0)

static const char *PIN = "1234";
static const char *ctx = "setup";

static void fresh_device(void)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    CHECK(wallet_set_password(PIN, strlen(PIN)) == WALLET_OK, "no password");
    CHECK(wallet_unlock(PIN, strlen(PIN)) == WALLET_OK, "no unlock");
}

static void check_vector(const zv_key_vector *v)
{
    ctx = v->name;
    if (v->passphrase[0]) {
        CHECK(wallet_set_passphrase(v->passphrase, strlen(v->passphrase)) == WALLET_OK,
              "passphrase not set");
    } else {
        wallet_clear_passphrase();
    }
    static ZcashAccount a;
    CHECK(zcash_account_derive(v->account, v->network, &a) == WALLET_OK, "derive failed");
    CHECK(memcmp(a.keys.seed_fingerprint, v->seed_fingerprint, 32) == 0, "seed fingerprint");
    CHECK(strcmp(a.keys.ufvk, v->ufvk) == 0, "UFVK");
    CHECK(strcmp(a.keys.ua, v->ua) == 0, "UA");
    CHECK(strcmp(a.t_address, v->transparent_address) == 0, "t-address");
    CHECK(memcmp(a.t_account, v->transparent_account_pubkey, 65) == 0, "transparent account key");
}

int main(void)
{
    printf("== test_zcash_account\n== %zu vectors through the real vault\n", ZV_KEYS_COUNT);

    /* Stored wallets: one vault slot per distinct mnemonic. */
    const char *last = NULL;
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const zv_key_vector *v = &ZV_KEYS[i];
        if (!last || strcmp(last, v->mnemonic) != 0) {
            fresh_device();
            ctx = v->name;
            uint8_t slot = wallet_add_mnemonic(v->mnemonic);
            CHECK(slot >= 1, "could not store the seed");
            CHECK(wallet_select_wallet(slot) == WALLET_OK, "could not select it");
            last = v->mnemonic;
        }
        check_vector(v);
    }

    printf("== temporary seed: same account, nothing stored\n");
    fresh_device();
    ctx = "temp";
    CHECK(wallet_use_temporary_mnemonic(ZV_KEYS[0].mnemonic) == WALLET_OK, "not adopted");
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        if (strcmp(ZV_KEYS[i].mnemonic, ZV_KEYS[0].mnemonic) == 0) check_vector(&ZV_KEYS[i]);
    }

    printf("== locked: no account, and nothing left in the output\n");
    ctx = "locked";
    wallet_lock();
    static ZcashAccount a;
    memset(&a, 0xAA, sizeof a);
    CHECK(zcash_account_derive(0, ZCASH_NET_TEST, &a) == WALLET_ERROR_LOCKED, "derived while locked");
    static const ZcashAccount zero;
    CHECK(memcmp(&a, &zero, sizeof a) == 0, "output not cleared on failure");

    printf("== bad arguments are refused\n");
    ctx = "args";
    fresh_device();
    CHECK(wallet_use_temporary_mnemonic(ZV_KEYS[0].mnemonic) == WALLET_OK, "not adopted");
    CHECK(zcash_account_derive(0x80000000u, ZCASH_NET_TEST, &a) != WALLET_OK, "hardened account accepted");
    CHECK(zcash_account_derive(0, 7, &a) != WALLET_OK, "unknown network accepted");

    if (failures) {
        printf("\nFAILED (%d failures)\n", failures);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
