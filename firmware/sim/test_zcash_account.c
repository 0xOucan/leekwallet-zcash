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
#include "zc_encoding.h"
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

/* librustzcash's addresses for test/abandon-art/pp0/acct0 at other diversifier
 * indices (host/zecz-core/tests/diversified.rs pins the same values). */
static const struct {
    uint64_t lo; uint32_t hi;    /* index = hi << 64 | lo */
    const char *t_address, *ua;
} DIVERSIFIED[] = {
    { 0x1, 0, "tmVNYZhAQMp61GkauCYV5R4eVKpxzzMZBJF",
      "utest1nggn4c6e0gmlr6fjlt4sel2wnph2r5ytca36uffhetvjpshjm3zy4xzeedhu56sj5qkyaafm6xq5xc75st8nj959mzwvz6qca2gzllzgfuxapwdxmcrafvpj5vm7tmed83mku2umc9g" },
    { 0x7, 0, "",
      "utest183eh57gnncle3r4258239e5r0hdwzsalftrwrsajxe30xggcu8h66d56nwllzcn3h2npt6edmtnx4ecrxtfrhtwy7gqx8jjd2ga528d4" },
    { 0x80000005, 0, "",
      "utest1l99gwjde6achwrewlh59cxhyup79fff5ytzmq0kw02rmcrzrtde4xcpvsqqhsj7aua6wt32z4f2fcz47r808rexmgtgj7l5pe596pzru" },
    { 0x030405060708090aull, 0x0102, "",
      "utest17jlmywqwc7rvr5agvxwmjhst0uumfs3yc4207ka9adxycdrykkmy3fcmsl8q03k76vr5ff4sfy5agta3h2d9dfnax20dq5f73sapazp0" },
};
static const char *WITH_SAPLING =
    "utest1dumh7z6x5xf4ay3wnkvc60uru4hu54y55u0t3xm78v2cqs5nk0zhsqspa4dqcpcj82hmtt049f30kx5z58yzxg3d7y3lpy69pg9vsdy7hdh2qfw8jj2u6vd46ztw7tzf88lydf8p3w7nre6zenfqeayq4e4njagajwvrmelywq76akhk";

static const zv_key_vector *find_vector(const char *name)
{
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        if (strcmp(ZV_KEYS[i].name, name) == 0) return &ZV_KEYS[i];
    }
    return NULL;
}

static void test_address_check(void)
{
    printf("== zShowAddress: any of the account's addresses, nobody else's\n");
    ctx = "address check";
    const zv_key_vector *v = find_vector("test/abandon-art/pp0/acct0");
    const zv_key_vector *other = find_vector("test/abandon-art/pp0/acct1");
    const zv_key_vector *main0 = find_vector("main/abandon-art/pp0/acct0");
    CHECK(v && other && main0, "vectors missing");
    if (!v || !other || !main0) return;
    fresh_device();
    CHECK(wallet_use_temporary_mnemonic(v->mnemonic) == WALLET_OK, "not adopted");
    static ZcashAccount a;
    CHECK(zcash_account_derive(0, ZCASH_NET_TEST, &a) == WALLET_OK, "derive failed");

    uint8_t j[11];
    char t[ZCASH_TADDR_MAX];
    static const uint8_t j0[11];
    CHECK(zcash_ua_check(&a, v->ua, j, t) == ZCASH_UA_OURS, "the default address");
    CHECK(memcmp(j, j0, 11) == 0, "the default address is not index 0");
    CHECK(strcmp(t, v->transparent_address) == 0, "default t-address");
    CHECK(zcash_ua_check(&a, v->ua_orchard_only, j, t) == ZCASH_UA_OURS && t[0] == 0,
          "the Orchard-only default address");

    for (size_t i = 0; i < sizeof DIVERSIFIED / sizeof DIVERSIFIED[0]; i++) {
        uint8_t want[11] = {0};
        for (int k = 0; k < 8; k++) want[k] = (uint8_t)(DIVERSIFIED[i].lo >> (8 * k));
        want[8] = (uint8_t)DIVERSIFIED[i].hi;
        want[9] = (uint8_t)(DIVERSIFIED[i].hi >> 8);
        want[10] = (uint8_t)(DIVERSIFIED[i].hi >> 16);
        memset(j, 0xEE, sizeof j);
        CHECK(zcash_ua_check(&a, DIVERSIFIED[i].ua, j, t) == ZCASH_UA_OURS, DIVERSIFIED[i].ua);
        CHECK(memcmp(j, want, 11) == 0, "wrong diversifier index recovered");
        CHECK(strcmp(t, DIVERSIFIED[i].t_address) == 0, "wrong t-address for the index");
    }

    CHECK(zcash_ua_check(&a, other->ua, j, t) == ZCASH_UA_NOT_OURS, "another account's address");
    CHECK(zcash_ua_check(&a, main0->ua, j, t) == ZCASH_UA_MALFORMED, "a mainnet address");
    CHECK(zcash_ua_check(&a, WITH_SAPLING, j, t) == ZCASH_UA_UNSUPPORTED, "a Sapling receiver");
    CHECK(zcash_ua_check(&a, "utest1notanaddress", j, t) == ZCASH_UA_MALFORMED, "garbage");

    /* Our Orchard receiver with someone else's transparent one: a sender's
     * wallet could pay the transparent half. */
    char mixed[512];
    CHECK(zc_encode_ua(mixed, sizeof mixed, 1, v->orchard_address_raw, other->transparent_receiver) > 0,
          "encode");
    CHECK(zcash_ua_check(&a, mixed, j, t) == ZCASH_UA_NOT_OURS, "a foreign t-receiver");
    /* ...and the account's own transparent key 0 next to a diversified
     * Orchard receiver: index mismatch. */
    uint8_t orchard7[43];
    zc_ua_items it;
    CHECK(zc_ua_parse(DIVERSIFIED[1].ua, strlen(DIVERSIFIED[1].ua), 1, &it) && it.has_orchard,
          "parse");
    memcpy(orchard7, it.orchard, 43);
    CHECK(zc_encode_ua(mixed, sizeof mixed, 1, orchard7, v->transparent_receiver) > 0, "encode");
    CHECK(zcash_ua_check(&a, mixed, j, t) == ZCASH_UA_NOT_OURS, "t-receiver at another index");
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

    test_address_check();

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
