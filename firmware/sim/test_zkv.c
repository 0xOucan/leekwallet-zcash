/* The .zkv SD seed file (src/zkv.c).
 *
 * - every key-vector phrase seals and opens back to the same phrase;
 * - a wrong passphrase fails cleanly: ZKV_E_AUTH and nothing in the output;
 * - every single-bit flip anywhere in a file, and every truncation, is
 *   refused, the header included (it is GCM associated data);
 * - the format's limits hold (work factor, passphrase, label, flags);
 * - a fixed-salt, fixed-nonce file is byte-identical to the golden vector
 *   in vectors/zkv/, which zecz (Rust, independent) must also reproduce.
 *
 * Iterations are the format's floor here: the property under test is the
 * format, and 10,000 PBKDF2 rounds per open is already the slow part.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bip39.h"
#include "zkv.h"
#include "../../vectors/keys_vectors.h"

static int failures;
static const char *ctx = "";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static const char *PASS = "correct horse battery";
#define IT ZKV_ITERATIONS_MIN

static bool all_zero(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i]) return false;
    return true;
}

static void contents_for(const char *mnemonic, ZkvContents *c)
{
    memset(c, 0, sizeof *c);
    uint8_t bits[33];   /* entropy, then the checksum bits */
    int nbits = mnemonic_to_bits(mnemonic, bits);
    c->entropy_len = (uint8_t)(nbits * 32 / 33 / 8);
    memcpy(c->entropy, bits, c->entropy_len);
    c->birthday = 3100000;
    c->account = 1;
    c->flags = 0;
    snprintf(c->label, sizeof c->label, "Savings");
}

static void test_roundtrip_every_phrase(void)
{
    const char *seen[64];
    int nseen = 0;
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const char *m = ZV_KEYS[i].mnemonic;
        bool dup = false;
        for (int j = 0; j < nseen; j++) dup |= strcmp(seen[j], m) == 0;
        if (dup) continue;
        seen[nseen++] = m;
        ctx = ZV_KEYS[i].name;

        ZkvContents c, back;
        contents_for(m, &c);
        uint8_t file[ZKV_FILE_MAX];
        size_t len = 0;
        CHECK(zkv_seal(&c, PASS, strlen(PASS), IT, file, sizeof file, &len) == ZKV_OK, "seal");
        CHECK(len == ZKV_HEADER_SIZE + 2 + c.entropy_len + 10 + strlen(c.label) + ZKV_TAG_SIZE,
              "size %zu", len);
        CHECK(zkv_open(file, len, PASS, strlen(PASS), &back) == ZKV_OK, "open");
        CHECK(memcmp(&c, &back, sizeof c) == 0, "contents differ after the round trip");
        const char *again = mnemonic_from_data(back.entropy, back.entropy_len);
        CHECK(again && strcmp(again, m) == 0, "phrase from entropy: %s", again ? again : "(null)");
        mnemonic_clear();

        /* Two seals of the same contents share nothing but the format. */
        uint8_t file2[ZKV_FILE_MAX];
        size_t len2 = 0;
        zkv_seal(&c, PASS, strlen(PASS), IT, file2, sizeof file2, &len2);
        CHECK(len2 == len && memcmp(file + 10, file2 + 10, 28) != 0, "salt and nonce are fresh");
        CHECK(memcmp(file + ZKV_HEADER_SIZE, file2 + ZKV_HEADER_SIZE, len - ZKV_HEADER_SIZE),
              "ciphertexts repeat");
    }
    printf("  %d distinct phrases round-tripped\n", nseen);
}

static void test_wrong_passphrase_fails_cleanly(void)
{
    ctx = "wrong passphrase";
    ZkvContents c, out;
    contents_for(ZV_KEYS[0].mnemonic, &c);
    uint8_t file[ZKV_FILE_MAX];
    size_t len;
    zkv_seal(&c, PASS, strlen(PASS), IT, file, sizeof file, &len);

    const char *wrong[] = { "correct horse battery ", "Correct horse battery", "correct horse batter",
                            "12345678" };
    for (size_t i = 0; i < sizeof wrong / sizeof *wrong; i++) {
        memset(&out, 0xA5, sizeof out);
        CHECK(zkv_open(file, len, wrong[i], strlen(wrong[i]), &out) == ZKV_E_AUTH, "\"%s\"", wrong[i]);
        CHECK(all_zero(&out, sizeof out), "output not wiped after \"%s\"", wrong[i]);
    }
    /* A passphrase that could not have sealed anything fails the same way,
     * and before the KDF. */
    memset(&out, 0xA5, sizeof out);
    CHECK(zkv_open(file, len, "short", 5, &out) == ZKV_E_AUTH && all_zero(&out, sizeof out),
          "short passphrase");
}

static void test_every_bit_and_length(void)
{
    ctx = "tamper";
    ZkvContents c, out;
    contents_for(ZV_KEYS[0].mnemonic, &c);
    uint8_t file[ZKV_FILE_MAX], t[ZKV_FILE_MAX];
    size_t len;
    zkv_seal(&c, PASS, strlen(PASS), IT, file, sizeof file, &len);

    int ran = 0;
    for (size_t byte = 0; byte < len; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(t, file, len);
            t[byte] ^= (uint8_t)(1u << bit);
            /* The work factor's high bytes would send this to the ceiling
             * check (refused before any KDF) - also a refusal. */
            int rc = zkv_open(t, len, PASS, strlen(PASS), &out);
            CHECK(rc != ZKV_OK, "flip at byte %zu bit %d accepted", byte, bit);
            CHECK(all_zero(&out, sizeof out), "flip at %zu.%d left output", byte, bit);
            ran++;
        }
    }
    for (size_t n = 0; n < len; n++) {
        CHECK(zkv_open(file, n, PASS, strlen(PASS), &out) != ZKV_OK, "truncated to %zu accepted", n);
    }
    memcpy(t, file, len);
    t[len] = 0;
    CHECK(zkv_open(t, len + 1, PASS, strlen(PASS), &out) != ZKV_OK, "trailing byte accepted");
    printf("  %d bit flips and %zu truncations refused\n", ran, len);
}

static void test_limits(void)
{
    ctx = "limits";
    ZkvContents c;
    contents_for(ZV_KEYS[0].mnemonic, &c);
    uint8_t file[ZKV_FILE_MAX];
    size_t len;
    CHECK(zkv_seal(&c, PASS, strlen(PASS), ZKV_ITERATIONS_MIN - 1, file, sizeof file, &len) == ZKV_E_KDF,
          "below the work-factor floor");
    CHECK(zkv_seal(&c, PASS, strlen(PASS), ZKV_ITERATIONS_MAX + 1, file, sizeof file, &len) == ZKV_E_KDF,
          "above the ceiling");
    CHECK(zkv_seal(&c, "1234567", 7, IT, file, sizeof file, &len) == ZKV_E_PASS, "7 chars");
    CHECK(zkv_seal(&c, "caf\xc3\xa9 caf\xc3\xa9", 12, IT, file, sizeof file, &len) == ZKV_E_PASS,
          "non-ASCII passphrase");
    char long_pass[66];
    memset(long_pass, 'a', 65);
    long_pass[65] = 0;
    CHECK(zkv_seal(&c, long_pass, 65, IT, file, sizeof file, &len) == ZKV_E_PASS, "65 chars");
    CHECK(zkv_seal(&c, long_pass, 64, IT, file, sizeof file, &len) == ZKV_OK, "64 chars");

    ZkvContents bad = c;
    bad.entropy_len = 15;
    CHECK(zkv_seal(&bad, PASS, strlen(PASS), IT, file, sizeof file, &len) == ZKV_E_ARG, "entropy 15");
    bad = c;
    bad.account = 0x80000000u;
    CHECK(zkv_seal(&bad, PASS, strlen(PASS), IT, file, sizeof file, &len) == ZKV_E_ARG, "hardened account");
    bad = c;
    bad.flags = 0x02;
    CHECK(zkv_seal(&bad, PASS, strlen(PASS), IT, file, sizeof file, &len) == ZKV_E_ARG, "unknown flag");
    bad = c;
    snprintf(bad.label, sizeof bad.label, "tab\there");
    CHECK(zkv_seal(&bad, PASS, strlen(PASS), IT, file, sizeof file, &len) == ZKV_E_ARG, "control in label");
    CHECK(zkv_seal(&c, PASS, strlen(PASS), IT, file, 40, &len) == ZKV_E_ARG, "small buffer");

    ctx = "peek";
    zkv_seal(&c, PASS, strlen(PASS), 12345, file, sizeof file, &len);
    uint32_t it = 0;
    CHECK(zkv_peek(file, len, &it) == ZKV_OK && it == 12345, "work factor read back: %u", it);
    uint8_t t[ZKV_FILE_MAX];
    memcpy(t, file, len);
    t[4] = 2;
    CHECK(zkv_peek(t, len, NULL) == ZKV_E_VERSION, "version 2 unknown");
    memcpy(t, file, len);
    t[0] = 'X';
    CHECK(zkv_peek(t, len, NULL) == ZKV_E_FORMAT, "bad magic");
}

/* The golden file. Regenerate only on a deliberate format change: with
 *   ZKV_WRITE_GOLDEN=1 build/test_zkv */
static void test_golden(void)
{
    ctx = "golden";
    ZkvContents c;
    memset(&c, 0, sizeof c);
    contents_for(ZV_KEYS[0].mnemonic, &c);   /* abandon x23 art */
    c.birthday = 2900000;
    c.account = 0;
    c.flags = ZKV_FLAG_USE_QSK;
    snprintf(c.label, sizeof c.label, "LeekWallet test");
    uint8_t salt[16], nonce[12];
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0xA0 + i);
    uint8_t file[ZKV_FILE_MAX];
    size_t len;
    CHECK(zkv__seal_fixed_for_test(&c, "LeekWallet zkv test", 19, ZKV_ITERATIONS_MIN, salt, nonce, file,
                                   sizeof file, &len) == ZKV_OK, "seal");

    const char *path = "../../vectors/zkv/golden-v1.zkv";
    if (getenv("ZKV_WRITE_GOLDEN")) {
        FILE *f = fopen(path, "wb");
        fwrite(file, 1, len, f);
        fclose(f);
        printf("  wrote %s (%zu bytes)\n", path, len);
    }
    uint8_t want[ZKV_FILE_MAX + 1];
    FILE *f = fopen(path, "rb");
    size_t wl = f ? fread(want, 1, sizeof want, f) : 0;
    if (f) fclose(f);
    CHECK(wl == len && memcmp(want, file, len) == 0, "differs from %s (%zu vs %zu bytes)", path, wl, len);

    ZkvContents back;
    CHECK(zkv_open(want, wl, "LeekWallet zkv test", 19, &back) == ZKV_OK &&
          memcmp(&back, &c, sizeof c) == 0, "golden opens to its contents");
}

int main(void)
{
    printf("test_zkv\n");
    test_roundtrip_every_phrase();
    test_wrong_passphrase_fails_cleanly();
    test_every_bit_and_length();
    test_limits();
    test_golden();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
