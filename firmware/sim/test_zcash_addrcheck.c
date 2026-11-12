/* Check address (zcash-addrcheck.h), end to end through the real vault.
 *
 * A seed goes into the vault, the board's own derivation makes its accounts
 * (zcash_account_derive), and a scanned address must come out as the
 * account and index librustzcash says it is - or as not this wallet's, the
 * wrong network, or not an address. The addresses are librustzcash vectors
 * (vectors/keys_vectors.h, and the diversified ones test_zcash_account pins),
 * ZIP 320's own TEX example, and encodings made here with trezor-crypto's
 * Bech32/Base58 for the kinds no vector has (Sapling, P2SH).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "base58.h"
#include "bip32.h"
#include "bip39.h"
#include "curves.h"
#include "ecdsa.h"
#include "esp_stubs.h"
#include "fake_nvs.h"
#include "leek-wallet.h"
#include "segwit_addr.h"
#include "zcash-addrcheck.h"
#include "zc_encoding.h"
#include "../../vectors/keys_vectors.h"

void wallet__reset_static_state_for_test(void);

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s:%d: ", ctx, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static const char *PIN = "1234";
static const char *ctx = "setup";

static const zv_key_vector *find_vector(const char *name)
{
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        if (strcmp(ZV_KEYS[i].name, name) == 0) return &ZV_KEYS[i];
    }
    return NULL;
}

static void fresh_device_with(const char *mnemonic)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    CHECK(wallet_set_password(PIN, strlen(PIN)) == WALLET_OK, "no password");
    CHECK(wallet_unlock(PIN, strlen(PIN)) == WALLET_OK, "no unlock");
    CHECK(wallet_use_temporary_mnemonic(mnemonic) == WALLET_OK, "not adopted");
}

/* What the UI does (ui.c, service_check_address): the selected account
 * first, then the others below HD_ACCOUNT_COUNT for a UA; the selected
 * account's 0..ZAC_T_SEARCH-1 for a transparent address. */
#define ACCOUNTS 3
static ZacVerdict run_check(const char *qr, uint8_t board_net, uint32_t selected, ZacCheck *c)
{
    static ZcashAccount a;
    ZacVerdict v = zac_parse(qr, strlen(qr), board_net, c);
    if (v != ZAC_PENDING) return v;
    if (zac_is_transparent(c)) {
        CHECK(zcash_account_derive(selected, c->net, &a) == WALLET_OK, "derive");
        while (!zac_t_step(c, &a)) { }
        memset(&a, 0, sizeof a);
        return c->verdict;
    }
    for (uint32_t k = 0; k < ACCOUNTS && c->verdict == ZAC_PENDING; k++) {
        const uint32_t acct = k == 0 ? selected : (k - 1 < selected ? k - 1 : k);
        CHECK(zcash_account_derive(acct, c->net, &a) == WALLET_OK, "derive %u", (unsigned)acct);
        zac_check_ua(c, &a);
    }
    memset(&a, 0, sizeof a);
    if (c->verdict == ZAC_PENDING) c->verdict = ZAC_NOT_OURS, c->why = ZAC_WHY_FOREIGN;
    return c->verdict;
}

static uint64_t index_of(const ZacCheck *c)
{
    uint64_t v = 0;
    for (int k = 7; k >= 0; k--) v = (v << 8) | c->index_le[k];
    return v;
}

static bool page_has(const ZacCheck *c, int page, const char *needle)
{
    ZacPage p;
    if (!zac_render(c, page, "HINT", &p)) return false;
    for (int r = 0; r < ZAC_ROWS; r++) {
        if (strstr(p.row[r], needle)) return true;
    }
    return false;
}

static void bytes_to_bech32(char *out, const char *hrp, const uint8_t *b, size_t n,
                            bech32_encoding enc)
{
    uint8_t five[128];
    size_t k = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | b[i];
        bits += 8;
        while (bits >= 5) { bits -= 5; five[k++] = (acc >> bits) & 31; }
    }
    if (bits) five[k++] = (acc << (5 - bits)) & 31;
    bech32_encode(out, hrp, five, k, enc);
}

static void upper(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

/* librustzcash's test/abandon-art/pp0/acct0 at other indices
 * (test_zcash_account.c, host/zecz-core/tests/diversified.rs). */
static const char *UA_IDX1 =
    "utest1nggn4c6e0gmlr6fjlt4sel2wnph2r5ytca36uffhetvjpshjm3zy4xzeedhu56sj5qkyaafm6xq5xc75st8nj959mzwvz6qca2gzllzgfuxapwdxmcrafvpj5vm7tmed83mku2umc9g";
static const char *T_IDX1 = "tmVNYZhAQMp61GkauCYV5R4eVKpxzzMZBJF";
static const char *UA_IDX7 =
    "utest183eh57gnncle3r4258239e5r0hdwzsalftrwrsajxe30xggcu8h66d56nwllzcn3h2npt6edmtnx4ecrxtfrhtwy7gqx8jjd2ga528d4";
static const char *UA_BIG =
    "utest17jlmywqwc7rvr5agvxwmjhst0uumfs3yc4207ka9adxycdrykkmy3fcmsl8q03k76vr5ff4sfy5agta3h2d9dfnax20dq5f73sapazp0";
static const char *WITH_SAPLING =
    "utest1dumh7z6x5xf4ay3wnkvc60uru4hu54y55u0t3xm78v2cqs5nk0zhsqspa4dqcpcj82hmtt049f30kx5z58yzxg3d7y3lpy69pg9vsdy7hdh2qfw8jj2u6vd46ztw7tzf88lydf8p3w7nre6zenfqeayq4e4njagajwvrmelywq76akhk";

/* An index's t-address made the long way, from the seed and the full private
 * path: the search reaches it from the account xpub. */
static void t_address_at(const uint8_t seed[64], uint32_t coin, uint32_t index, char *out,
                         size_t cap)
{
    HDNode n;
    hdnode_from_seed(seed, 64, SECP256K1_NAME, &n);
    hdnode_private_ckd(&n, 0x80000000u | 44);
    hdnode_private_ckd(&n, 0x80000000u | coin);
    hdnode_private_ckd(&n, 0x80000000u);
    hdnode_private_ckd(&n, 0);
    hdnode_private_ckd(&n, index);
    hdnode_fill_public_key(&n);
    hdnode_get_address(&n, coin == 133 ? 0x1CB8 : 0x1D25, out, (int)cap);
}

static void test_testnet(void)
{
    printf("== testnet board, abandon-art seed\n");
    ctx = "testnet";
    const zv_key_vector *v = find_vector("test/abandon-art/pp0/acct0");
    const zv_key_vector *v1 = find_vector("test/abandon-art/pp0/acct1");
    const zv_key_vector *main0 = find_vector("main/abandon-art/pp0/acct0");
    const zv_key_vector *zoo = find_vector("test/zoo-vote/pp0/acct0");
    CHECK(v && v1 && main0 && zoo, "vectors missing");
    if (!v || !v1 || !main0 || !zoo) return;
    fresh_device_with(v->mnemonic);
    static ZacCheck c;

    /* Own address at index 0: the UA, Orchard only, the t-address. */
    CHECK(run_check(v->ua, ZCASH_NET_TEST, 0, &c) == ZAC_OURS, "default UA");
    CHECK(c.account == 0 && index_of(&c) == 0, "acct %u idx %llu", (unsigned)c.account,
          (unsigned long long)index_of(&c));
    CHECK(c.receivers == (ZAC_R_ORCHARD | ZAC_R_TRANSPARENT), "receivers %x", c.receivers);
    CHECK(page_has(&c, 0, "YOURS") && page_has(&c, 0, "Account 0") &&
          page_has(&c, 0, "Address #0") && page_has(&c, 0, "Checked: Orchard") &&
          page_has(&c, 0, "Checked: transparent") && !page_has(&c, 0, "Sapling"),
          "verdict page");
    CHECK(run_check(v->ua_orchard_only, ZCASH_NET_TEST, 0, &c) == ZAC_OURS &&
          c.receivers == ZAC_R_ORCHARD && page_has(&c, 0, "Checked: Orchard") &&
          !page_has(&c, 0, "Checked: transparent"), "Orchard-only UA");
    CHECK(run_check(v->transparent_address, ZCASH_NET_TEST, 0, &c) == ZAC_OURS &&
          index_of(&c) == 0 && page_has(&c, 0, "Checked: transparent"), "t-address #0");

    /* Diversified indices: recovered, not searched. */
    CHECK(run_check(UA_IDX1, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 1,
          "UA #1 -> %llu", (unsigned long long)index_of(&c));
    CHECK(run_check(UA_IDX7, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 7 &&
          page_has(&c, 0, "Address #7"), "UA #7");
    CHECK(run_check(UA_BIG, ZCASH_NET_TEST, 0, &c) == ZAC_OURS &&
          c.index_le[9] == 0x01 && c.index_le[8] == 0x02 && c.index_le[0] == 0x0a,
          "UA at 0x0102030405060708090a");
    CHECK(page_has(&c, 0, "Address index") && page_has(&c, 0, "0x102030405060708090a"),
          "88-bit index in hex");
    CHECK(run_check(T_IDX1, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 1,
          "t-address #1 -> %llu", (unsigned long long)index_of(&c));

    /* The edge of the search: the last index found, the next one not. */
    char t[64];
    t_address_at(v->seed, 1, ZAC_T_SEARCH - 1, t, sizeof t);
    CHECK(run_check(t, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == ZAC_T_SEARCH - 1,
          "the last t index");
    {
        /* The search alone, without the account's derivation. */
        static ZcashAccount a;
        CHECK(zcash_account_derive(0, ZCASH_NET_TEST, &a) == WALLET_OK, "derive");
        CHECK(zac_parse(t, strlen(t), ZCASH_NET_TEST, &c) == ZAC_PENDING, "parse");
        const clock_t t0 = clock();
        while (!zac_t_step(&c, &a)) { }
        const double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        printf("  t-address search, %u indices: %.1f ms on this host\n", ZAC_T_SEARCH, ms);
        memset(&a, 0, sizeof a);
    }
    t_address_at(v->seed, 1, ZAC_T_SEARCH, t, sizeof t);
    char range[32];
    snprintf(range, sizeof range, "#0-%u", ZAC_T_SEARCH - 1);
    CHECK(run_check(t, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS && c.why == ZAC_WHY_T_RANGE &&
          page_has(&c, 0, "NOT from this wallet") && page_has(&c, 0, range), "one past the range");

    /* Another account of this seed: found, and named. */
    CHECK(run_check(v1->ua, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && c.account == 1 &&
          page_has(&c, 0, "Account 1"), "account 1's UA (acct %u)", (unsigned)c.account);
    CHECK(run_check(v1->ua, ZCASH_NET_TEST, 1, &c) == ZAC_OURS && c.account == 1,
          "account 1 selected");
    /* The transparent search is the selected account's only. */
    CHECK(run_check(v1->transparent_address, ZCASH_NET_TEST, 1, &c) == ZAC_OURS &&
          c.account == 1, "account 1's t-address, account 1 selected");
    CHECK(run_check(v1->transparent_address, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS &&
          c.why == ZAC_WHY_T_RANGE && page_has(&c, 0, "Not account 0's"),
          "account 1's t-address, account 0 selected");

    /* Foreign: another seed's UA and t-address. */
    CHECK(run_check(zoo->ua, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS && c.why == ZAC_WHY_FOREIGN &&
          page_has(&c, 0, "NOT from this wallet"), "zoo-vote UA");
    CHECK(run_check(zoo->transparent_address, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS,
          "zoo-vote t-address");
    /* Our Orchard receiver next to a foreign transparent one, and our index 7
     * receiver next to our transparent #0: every receiver must agree. */
    char mixed[512];
    CHECK(zc_encode_ua(mixed, sizeof mixed, 1, v->orchard_address_raw, zoo->transparent_receiver) > 0,
          "encode");
    CHECK(run_check(mixed, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS, "foreign t-receiver in our UA");
    zc_ua_items it;
    CHECK(zc_ua_parse(UA_IDX7, strlen(UA_IDX7), 1, &it), "parse");
    CHECK(zc_encode_ua(mixed, sizeof mixed, 1, it.orchard, v->transparent_receiver) > 0, "encode");
    CHECK(run_check(mixed, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS, "t-receiver at another index");
    /* A UA with a Sapling receiver: the board has no Sapling keys, so the
     * rest is checked and the screen says Sapling was not. */
    {
        const ZacVerdict sv = run_check(WITH_SAPLING, ZCASH_NET_TEST, 0, &c);
        /* librustzcash's default address of the full UFVK (Orchard + Sapling)
         * for this account: the Orchard half is ours at index 0. */
        CHECK(sv == ZAC_OURS && c.account == 0 && index_of(&c) == 0, "Orchard + Sapling, ours");
        CHECK(c.receivers == (ZAC_R_ORCHARD | ZAC_R_SAPLING), "receivers %x", c.receivers);
        CHECK(page_has(&c, 0, "YOURS") && page_has(&c, 0, "Sapling NOT checked") &&
              page_has(&c, 0, "Checked: Orchard"), "an unchecked Sapling receiver said plainly");
        char up[300];
        snprintf(up, sizeof up, "%s", WITH_SAPLING);
        upper(up);
        CHECK(run_check(up, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && strcmp(c.addr, WITH_SAPLING) == 0,
              "the same, upper case (alphanumeric QR)");
    }

    /* Wrong network: the mainnet twin, and a mainnet t-address. */
    CHECK(run_check(main0->ua, ZCASH_NET_TEST, 0, &c) == ZAC_WRONG_NET &&
          page_has(&c, 0, "Wrong network") && page_has(&c, 0, "Mainnet address,") &&
          page_has(&c, 0, "board is on testnet"), "mainnet UA");
    CHECK(run_check(main0->transparent_address, ZCASH_NET_TEST, 0, &c) == ZAC_WRONG_NET,
          "mainnet t-address");

    /* URIs and case. */
    char uri[600];
    snprintf(uri, sizeof uri, "zcash:%s?amount=1.5&memo=SGk", UA_IDX7);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 7, "zcash:<ua>?...");
    CHECK(strcmp(c.addr, UA_IDX7) == 0, "the address, not the URI, is shown");
    /* The forms address_check_payload() (host/leek-zcash/src/ua.rs) takes:
     * one payment, unnumbered; several payments are refused. */
    snprintf(uri, sizeof uri, "ZCASH:?amount=2&address=%s", T_IDX1);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 1,
          "ZCASH:?amount=...&address=...");
    snprintf(uri, sizeof uri, "ZCASH:%s", UA_IDX1);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 1, "ZCASH:<ua>");
    snprintf(uri, sizeof uri, "zcash:?address=%s&address.1=%s", T_IDX1, zoo->ua);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "two payments refused");
    snprintf(uri, sizeof uri, "zcash:?address.1=%s&amount.1=1", UA_IDX7);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "a numbered payment refused");
    snprintf(uri, sizeof uri, "zcash:?address=%s&address=%s", T_IDX1, T_IDX1);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "address twice refused");
    snprintf(uri, sizeof uri, " %s\r\n", v->ua);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_OURS, "surrounding whitespace");
    snprintf(uri, sizeof uri, "%s", UA_IDX7);
    upper(uri);
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 7 &&
          strcmp(c.addr, UA_IDX7) == 0, "upper-case UA, shown in lower case");
    uri[10] = (char)(uri[10] - 'A' + 'a');
    CHECK(run_check(uri, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "mixed case is not Bech32m");

    /* TEX (ZIP 320): the account's own key hash, and its index 1. */
    char tex[128];
    bytes_to_bech32(tex, "textest", v->transparent_receiver, 20, BECH32_ENCODING_BECH32M);
    CHECK(run_check(tex, ZCASH_NET_TEST, 0, &c) == ZAC_OURS && index_of(&c) == 0 &&
          page_has(&c, 0, "Checked: TEX"), "TEX #0 (%s)", tex);
    bytes_to_bech32(tex, "textest", v->transparent_receiver, 20, BECH32_ENCODING_BECH32);
    CHECK(run_check(tex, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "TEX in Bech32, not Bech32m");

    /* Sapling and P2SH: well formed, never this wallet's. */
    uint8_t raw[43];
    for (int i = 0; i < 43; i++) raw[i] = (uint8_t)(i * 7 + 1);
    char zs[128];
    bytes_to_bech32(zs, "ztestsapling", raw, 43, BECH32_ENCODING_BECH32);
    CHECK(run_check(zs, ZCASH_NET_TEST, 0, &c) == ZAC_CANNOT_CHECK && c.why == ZAC_WHY_SAPLING &&
          page_has(&c, 0, "Cannot check") && page_has(&c, 0, "no Sapling"),
          "Sapling address (%s)", zs);
    zs[strlen(zs) - 1] = zs[strlen(zs) - 1] == 'q' ? 'p' : 'q';
    CHECK(run_check(zs, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "Sapling with a bad checksum");
    bytes_to_bech32(zs, "zs", raw, 43, BECH32_ENCODING_BECH32);
    CHECK(run_check(zs, ZCASH_NET_TEST, 0, &c) == ZAC_WRONG_NET, "mainnet Sapling");
    uint8_t p2sh[22] = { 0x1C, 0xBA };
    memcpy(p2sh + 2, v->transparent_receiver, 20);
    char t2[64];
    base58_encode_check(p2sh, 22, HASHER_SHA2D, t2, sizeof t2);
    CHECK(t2[0] == 't' && t2[1] == '2', "t2 prefix (%s)", t2);
    CHECK(run_check(t2, ZCASH_NET_TEST, 0, &c) == ZAC_NOT_OURS && c.why == ZAC_WHY_P2SH,
          "P2SH, even over our own key hash");

    /* Malformed. */
    static const char *const BAD[] = {
        "", "   ", "hello world", "https://z.cash", "ur:leek-pczt/1-3/lpadaxcs",
        "utest1notanaddress", "u1", "zcash:", "zcash:?amount=1", "zcash:?address=",
        "tmVNYZhAQMp61GkauCYV5R4eVKpxzzMZBJG",   /* t-address, last char changed */
        "zcash:utest1%20abc",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        CHECK(run_check(BAD[i], ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED && c.addr[0] == 0,
              "malformed \"%s\"", BAD[i]);
    }
    char longer[400];
    memset(longer, 'a', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    CHECK(run_check(longer, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "too long");
    char bad_ua[300];
    snprintf(bad_ua, sizeof bad_ua, "%s", v->ua);
    bad_ua[20] = bad_ua[20] == 'q' ? 'p' : 'q';
    CHECK(run_check(bad_ua, ZCASH_NET_TEST, 0, &c) == ZAC_MALFORMED, "UA with a typo");
}

static void test_mainnet(void)
{
    printf("== mainnet board\n");
    ctx = "mainnet";
    const zv_key_vector *m = find_vector("main/abandon-art/pp0/acct0");
    const zv_key_vector *t = find_vector("test/abandon-art/pp0/acct0");
    CHECK(m && t, "vectors missing");
    if (!m || !t) return;
    fresh_device_with(m->mnemonic);
    static ZacCheck c;
    CHECK(run_check(m->ua, ZCASH_NET_MAIN, 0, &c) == ZAC_OURS && page_has(&c, 0, "Mainnet"),
          "u1 default");
    CHECK(run_check(m->transparent_address, ZCASH_NET_MAIN, 0, &c) == ZAC_OURS, "t1 #0");
    char tex[128];
    bytes_to_bech32(tex, "tex", m->transparent_receiver, 20, BECH32_ENCODING_BECH32M);
    CHECK(run_check(tex, ZCASH_NET_MAIN, 0, &c) == ZAC_OURS, "tex1 #0");
    CHECK(run_check(t->ua, ZCASH_NET_MAIN, 0, &c) == ZAC_WRONG_NET &&
          page_has(&c, 0, "Testnet address,") && page_has(&c, 0, "board is on mainnet"),
          "utest on mainnet");
    CHECK(run_check(t->transparent_address, ZCASH_NET_MAIN, 0, &c) == ZAC_WRONG_NET, "tm on mainnet");

    /* ZIP 320's example: the TEX address and its t1 decode to one key hash. */
    ZacCheck a, b;
    CHECK(zac_parse("t1VmmGiyjVNeCjxDZzg7vZmd99WyzVby9yC", 35, ZCASH_NET_MAIN, &a) == ZAC_PENDING,
          "ZIP 320 t1");
    const char *zip320 = "tex1s2rt77ggv6q989lr49rkgzmh5slsksa9khdgte";
    CHECK(zac_parse(zip320, strlen(zip320), ZCASH_NET_MAIN, &b) == ZAC_PENDING &&
          b.kind == ZAC_KIND_TEX, "ZIP 320 tex1");
    CHECK(memcmp(a.p2pkh, b.p2pkh, 20) == 0, "same key hash");
    CHECK(run_check("t1VmmGiyjVNeCjxDZzg7vZmd99WyzVby9yC", ZCASH_NET_MAIN, 0, &c) == ZAC_NOT_OURS,
          "someone else's t1");
}

static void test_pages(void)
{
    printf("== address pages: groups of four, every character once\n");
    ctx = "pages";
    const zv_key_vector *m = find_vector("main/abandon-art/pp0/acct0");
    if (!m) return;
    static ZacCheck c;
    CHECK(zac_parse(m->ua, strlen(m->ua), ZCASH_NET_MAIN, &c) == ZAC_PENDING, "parse");
    const int pages = zac_page_count(&c);
    CHECK(pages == 1 + (int)((strlen(m->ua) + 95) / 96), "%d pages for %zu chars", pages,
          strlen(m->ua));
    char joined[400] = "";
    for (int p = 1; p < pages; p++) {
        ZacPage pg;
        CHECK(zac_render(&c, p, "X:BACK", &pg), "page %d", p);
        CHECK(strncmp(pg.row[0], "Address ", 8) == 0, "title \"%s\"", pg.row[0]);
        CHECK(strcmp(pg.row[7], "X:BACK") == 0, "footer");
        for (int r = 1; r < 7; r++) {
            CHECK(strlen(pg.row[r]) <= ZAC_COLS, "row width");
            if (pg.row[r][0]) {
                CHECK(pg.row[r][4] == ' ' || strlen(pg.row[r]) <= 4, "grouped: \"%s\"", pg.row[r]);
            }
            for (const char *s = pg.row[r]; *s; s++) {
                if (*s != ' ') strncat(joined, s, 1);
            }
        }
    }
    CHECK(strcmp(joined, m->ua) == 0, "the pages spell the address");
    ZacPage pg;
    CHECK(!zac_render(&c, pages, "", &pg) && !zac_render(&c, -1, "", &pg), "out of range");
    for (int p = 0; p < pages; p++) {
        zac_render(&c, p, "X:BACK  TURN  OK:SCAN", &pg);
        for (int r = 0; r < ZAC_ROWS; r++) {
            CHECK(strlen(pg.row[r]) <= ZAC_COLS, "page %d row %d too wide", p, r);
        }
    }
}

int main(void)
{
    printf("== test_zcash_addrcheck\n");
    test_testnet();
    test_mainnet();
    test_pages();
    if (failures) {
        printf("\nFAILED (%d failures)\n", failures);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
