/* App Storage Keys (docs/zcash/zip-draft-app-storage-keys.md) through the C
 * signer's zc_storage_key: the draft's four provisional vectors, computed
 * there with the zip32 crate, and the App identifier rules a Signer must
 * apply before it shows anything. The SDK (host/leek-zcash) checks the same
 * vectors with the zip32 crate itself.
 */
#include <stdio.h>
#include <string.h>

#include "zsign_c.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void unhex(const char *h, uint8_t *out)
{
    for (size_t i = 0; h[2 * i]; i++) {
        unsigned b;
        sscanf(h + 2 * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
}

static const struct {
    uint8_t net;
    uint32_t account;
    const char *app_id;
    const char *key;
} VECTORS[] = {
    { ZS_NET_MAIN, 0, "xyz.leekwallet.zcash",
      "f43c678ee392cc4552daa3bde21c3cb15b416811dfdacf64e7aceca6114bd0d0" },
    { ZS_NET_TEST, 0, "xyz.leekwallet.zcash",
      "854cfb1ecebae4b636bc37c1043370b16e658a79fd5402098ffd7d7ddea277fd" },
    { ZS_NET_MAIN, 1, "xyz.leekwallet.zcash",
      "26efbcd704c2229a500441fafc913394bd8b576a96593b77096aaf6fa2c894a7" },
    { ZS_NET_MAIN, 0, "com.example.wallet",
      "3977f965125080aed7b50e09ebbfc3b1760732e0b24ac8bb039514c36587d944" },
};

static int valid(const char *id)
{
    return zc_app_id_valid(id, strlen(id));
}

int main(void)
{
    printf("== storage key: the draft's vectors (ZIP_NUMBER 65535, seed 00..1f)\n");
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)i;
    for (size_t v = 0; v < sizeof VECTORS / sizeof VECTORS[0]; v++) {
        uint8_t want[32], got[32];
        unhex(VECTORS[v].key, want);
        int32_t rc = zc_storage_key(seed, sizeof seed, VECTORS[v].net, VECTORS[v].account,
                                    VECTORS[v].app_id, strlen(VECTORS[v].app_id), got);
        CHECK(rc == ZS_OK && memcmp(got, want, 32) == 0, "vector %zu (%s, account %u)", v + 1,
              VECTORS[v].app_id, (unsigned)VECTORS[v].account);
    }

    printf("== storage key: separation and argument checks\n");
    {
        uint8_t a[32], b[32], seed64[64];
        for (int i = 0; i < 64; i++) seed64[i] = (uint8_t)(0xA0 ^ i);
        CHECK(zc_storage_key(seed64, 64, ZS_NET_MAIN, 0, "a.b", 3, a) == ZS_OK, "a 64-byte seed");
        CHECK(zc_storage_key(seed64, 64, ZS_NET_MAIN, 0, "a.c", 3, b) == ZS_OK &&
              memcmp(a, b, 32) != 0, "two app ids gave one key");
        CHECK(zc_storage_key(seed64, 64, ZS_NET_TEST, 0, "a.b", 3, b) == ZS_OK &&
              memcmp(a, b, 32) != 0, "two networks gave one key");
        CHECK(zc_storage_key(seed64, 63, ZS_NET_MAIN, 0, "a.b", 3, b) == ZS_OK &&
              memcmp(a, b, 32) != 0, "the seed length is not bound");
        CHECK(zc_storage_key(seed64, 31, ZS_NET_MAIN, 0, "a.b", 3, b) == ZS_ERR_ARG, "a short seed");
        CHECK(zc_storage_key(seed64, 64, 2, 0, "a.b", 3, b) == ZS_ERR_ARG, "network 2");
        CHECK(zc_storage_key(seed64, 64, ZS_NET_MAIN, 0x80000000u, "a.b", 3, b) == ZS_ERR_ARG,
              "a hardened account index");
        CHECK(zc_storage_key(seed64, 64, ZS_NET_MAIN, 0, "A.b", 3, b) == ZS_E_PARSE,
              "derived for an invalid app id");
    }

    printf("== app id: the draft's grammar, nothing normalised\n");
    {
        static const char *good[] = {
            "a.b", "xyz.leekwallet.zcash", "com.example.wallet", "0.9", "a-.b_",
            "com.my-app.zcash_dev", "io.x1.y2",
            /* 64 bytes */
            "abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghi",
        };
        static const char *bad[] = {
            "", "a", "ab", "abc", "a.", ".a", "a..b", "Com.example", "com.Example",
            "com.example.", "-a.b", "a.-b", "_a.b", "a.b c", "a b.c", "a/b.c", "a.b\n",
            "com.ex\xc3\xa1mple", "a:b.c",
            /* 65 bytes */
            "abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghij.abcdefghij",
        };
        for (size_t i = 0; i < sizeof good / sizeof good[0]; i++)
            CHECK(valid(good[i]), "refused \"%s\"", good[i]);
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
            CHECK(!valid(bad[i]), "accepted \"%s\"", bad[i]);
        /* The grammar allows labels of 63 bytes, but with the dot and a
         * second label the 64-byte total caps them at 62. */
        char l62[65], l63[66];
        memset(l62, 'a', 62); memcpy(l62 + 62, ".b", 3);
        memset(l63, 'a', 63); memcpy(l63 + 63, ".b", 3);
        CHECK(valid(l62), "refused a 62-byte label");
        CHECK(!valid(l63), "accepted 65 bytes");
        /* A NUL inside the length is a byte like any other, and not allowed. */
        CHECK(!zc_app_id_valid("a.b\0c", 5), "accepted an embedded NUL");
        CHECK(!zc_app_id_valid(NULL, 3), "accepted NULL");
    }

    if (fails) {
        printf("test_storage_key: %d FAILED\n", fails);
        return 1;
    }
    printf("test_storage_key: all passed\n");
    return 0;
}
