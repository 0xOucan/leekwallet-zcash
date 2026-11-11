/* The seed's backup copies (docs/BACKUP.md): the SE container copy
 * (src/se-backup.c) and the SD restore into the vault (src/sd-seed.c),
 * through the real vault (leek-wallet.c on the fake NVS), the real .zkv
 * format, a directory as the card, and the real LKSC container format over
 * a fake chip (fake_se_store.c).
 *
 * What it holds the code to:
 *   - copy -> compare -> erase -> restore, with the fingerprint the Zcash
 *     vectors give for the phrase;
 *   - the copy is sealed: no phrase, entropy or fingerprint readable in the
 *     object, and it opens only with the PIN it was made under;
 *   - restoring to the vault never overwrites or deletes a vault seed: a seed
 *     already there is a no-op with no flash write, a different one is added
 *     after the others, a full vault and an unreadable slot refuse;
 *   - an empty, foreign, damaged, cut or tampered container is named as such,
 *     and a foreign one is never written over;
 *   - a write that does not read back, or reads back as something else, is
 *     reported, not trusted;
 *   - SD -> SE and SD -> vault under the same rules.
 * Built with ASan and UBSan.
 */
#define _DEFAULT_SOURCE   /* mkdtemp */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bip39.h"
#include "esp_stubs.h"
#include "fake_nvs.h"
#include "fake_se_store.h"
#include "leek-wallet.h"
#include "nvs.h"
#include "optiga-container.h"
#include "sd-seed.h"
#include "se-backup.h"
#include "zcash-fp.h"
#include "../../vectors/keys_vectors.h"

void wallet__reset_static_state_for_test(void);

static int failures;
static const char *ctx = "setup";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static const char *PIN = "1234";
static const char *PASS = "card passphrase 1";
static const char *A, *B;          /* two phrases from the key vectors */
static uint8_t A_FP[32];           /* A's fingerprint, no passphrase, from the vectors */
static char card[256];

static void fresh_device_pin(const char *pin)
{
    fake_nvs_reset();
    wallet__reset_static_state_for_test();
    wallet_init();
    CHECK(wallet_set_password(pin, strlen(pin)) == WALLET_OK, "no password");
    CHECK(wallet_unlock(pin, strlen(pin)) == WALLET_OK, "no unlock");
}

static void fresh_device(void) { fresh_device_pin(PIN); }

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

static bool open_phrase_is(const char *m)
{
    char p[256];
    bool same = wallet_get_mnemonic(p, sizeof p) == WALLET_OK && strcmp(p, m) == 0;
    memset(p, 0, sizeof p);
    return same;
}

static bool slot_is(uint8_t slot, const char *m)
{
    return wallet_select_wallet(slot) == WALLET_OK && open_phrase_is(m);
}

static bool image_contains(const void *needle, size_t n)
{
    const uint8_t *h = fake_se_image();
    for (size_t i = 0; i + n <= OPTIGA_CONTAINER_OBJ_SIZE; i++) {
        if (memcmp(h + i, needle, n) == 0) return true;
    }
    return false;
}

static ZkvContents contents_of(const char *m)
{
    ZkvContents c;
    memset(&c, 0, sizeof c);
    zkv_entropy_from_phrase(m, &c);
    return c;
}

/* ------------------------------------------------------------ the format */

static void test_format(void)
{
    printf("== the copy's format: seal, open, wrong PIN, tamper, limits\n");
    ctx = "format";
    ZkvContents c = contents_of(A), out;
    c.birthday = 2900000;
    c.account = 3;
    snprintf(c.label, sizeof c.label, "savings");
    uint8_t blob[SEBK_BLOB_MAX], fp[32];
    size_t len = 0;
    CHECK(sebk_seal(PIN, 4, &c, A_FP, blob, sizeof blob, &len) == ZKV_OK, "seal");
    CHECK(len == (size_t)(SEBK_HEADER_SIZE + VAULT_CRYPT_OVERHEAD + 33 + 2 + c.entropy_len + 10 + 7),
          "length %zu", len);
    CHECK(len <= SEBK_BLOB_MAX && len <= OPTIGA_CONTAINER_CAPACITY, "fits");
    CHECK(memcmp(blob, "LKSV", 4) == 0 && blob[4] == 1, "header");
    CHECK(sebk_open(blob, len, PIN, 4, &out, fp) == ZKV_OK, "open");
    CHECK(out.entropy_len == c.entropy_len && memcmp(out.entropy, c.entropy, c.entropy_len) == 0,
          "entropy");
    CHECK(out.birthday == 2900000 && out.account == 3 && strcmp(out.label, "savings") == 0, "meta");
    CHECK(memcmp(fp, A_FP, 32) == 0, "fingerprint");

    /* The same PIN twice gives two different blobs: fresh salt and nonce. */
    uint8_t blob2[SEBK_BLOB_MAX];
    size_t len2 = 0;
    sebk_seal(PIN, 4, &c, A_FP, blob2, sizeof blob2, &len2);
    CHECK(len2 == len && memcmp(blob + 25, blob2 + 25, len - 25) != 0, "salt/nonce reused");

    CHECK(sebk_open(blob, len, "1235", 4, &out, fp) == ZKV_E_AUTH, "wrong PIN");
    static const uint8_t zero[32];
    CHECK(out.entropy_len == 0 && memcmp(fp, zero, 32) == 0, "wiped on failure");
    CHECK(sebk_open(blob, len, PIN, 0, &out, fp) == ZKV_E_AUTH, "empty PIN");

    /* Every byte after the header, flipped: never opens. The header's own
     * bytes: a different key (salt, params) or a refusal. */
    int opened = 0;
    for (size_t i = 0; i < len; i++) {
        blob[i] ^= 0x40;
        int rc = sebk_open(blob, len, PIN, 4, &out, fp);
        if (rc == ZKV_OK) opened++;
        if (i < 4) CHECK(rc == ZKV_E_FORMAT, "magic byte %zu: %d", i, rc);
        blob[i] ^= 0x40;
    }
    CHECK(opened == 0, "%d tampered blobs opened", opened);
    CHECK(sebk_open(blob, len - 1, PIN, 4, &out, fp) != ZKV_OK, "truncated");
    CHECK(sebk_open(blob, 10, PIN, 4, &out, fp) == ZKV_E_FORMAT, "short");

    /* A work factor outside the range is refused before any KDF. */
    uint8_t save[4];
    memcpy(save, blob + 7, 4);
    blob[7] = 0x10; blob[8] = 0x00; blob[9] = 0x00; blob[10] = 0x00;   /* 16 */
    CHECK(sebk_open(blob, len, PIN, 4, &out, fp) == ZKV_E_KDF, "tiny work factor");
    blob[7] = 0x00; blob[8] = 0x00; blob[9] = 0x00; blob[10] = 0x01;   /* 2^24 */
    CHECK(sebk_open(blob, len, PIN, 4, &out, fp) == ZKV_E_KDF, "huge work factor");
    memcpy(blob + 7, save, 4);
    blob[4] = 2;
    CHECK(sebk_open(blob, len, PIN, 4, &out, fp) == ZKV_E_VERSION, "newer version");
    blob[4] = 1;
    CHECK(sebk_open(blob, len, PIN, 4, &out, fp) == ZKV_OK, "restored");
    memset(&out, 0, sizeof out);
}

/* --------------------------------------------- copy, compare, erase */

static void test_copy_compare_erase(void)
{
    printf("== copy -> compare -> erase, through the real vault\n");
    ctx = "copy";
    fresh_device();
    fake_se_reset();
    uint8_t a = wallet_add_mnemonic(A);
    CHECK(a == 1 && wallet_select_wallet(a) == WALLET_OK, "store A");

    SeBackupState st;
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_EMPTY, "starts empty");
    SeBackupPeek peek;
    CHECK(sebk_peek(PIN, &peek) == SEBK_OK && !peek.opened && peek.state == SEBK_STATE_EMPTY,
          "peek empty");

    char zfp[9];
    SeBackupResult r = sebk_copy_open_seed(PIN, "main", 2900000, 0, false, zfp);
    CHECK(r == SEBK_OK, "copy: %s", sebk_strerror(r));
    char want[9];
    zcash_short_fp(A_FP, want);
    CHECK(strcmp(zfp, want) == 0, "copy's ZFP %s, vectors say %s", zfp, want);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_COPY, "now a copy");

    /* Sealed: nothing about the seed is in the object in clear. The vector
     * phrases' entropies are runs of one byte (abandon's is all zeros, which
     * an erased object is too), so this is asked of a random phrase. */
    CHECK(!image_contains(A_FP, 8), "fingerprint readable in the SE");
    CHECK(!image_contains("main", 4), "label readable in the SE");
    {
        char rnd[256];
        snprintf(rnd, sizeof rnd, "%s", mnemonic_generate(256));
        mnemonic_clear();
        CHECK(wallet_use_temporary_mnemonic(rnd) == WALLET_OK, "random temporary");
        CHECK(sebk_copy_open_seed(PIN, "secret-label", 0, 0, true, NULL) == SEBK_OK, "random copy");
        ZkvContents cr = contents_of(rnd);
        uint8_t rfp[32];
        zcash_fp_of_phrase(rnd, rfp);
        char word[16];
        snprintf(word, sizeof word, "%.*s", (int)strcspn(rnd, " "), rnd);
        CHECK(!image_contains(cr.entropy, 6), "entropy readable in the SE");
        CHECK(!image_contains(rfp, 6), "fingerprint readable in the SE");
        CHECK(!image_contains("secret-label", 12), "label readable in the SE");
        CHECK(!image_contains(rnd, strlen(word) + 1), "phrase readable in the SE");
        memset(&cr, 0, sizeof cr);
        memset(rnd, 0, sizeof rnd);
        CHECK(wallet_select_wallet(a) == WALLET_OK, "back to A");
        CHECK(sebk_copy_open_seed(PIN, "main", 2900000, 0, true, zfp) == SEBK_OK, "A again");
    }

    r = sebk_peek(PIN, &peek);
    CHECK(r == SEBK_OK && peek.opened && peek.matches_open && strcmp(peek.zfp, want) == 0 &&
          peek.birthday == 2900000 && strcmp(peek.label, "main") == 0, "compare: match");
    CHECK(sebk_peek("9999", &peek) == SEBK_E_PIN && !peek.opened, "compare: wrong PIN");

    /* Copying the same seed again needs no confirmation. */
    const int w = fake_se_writes();
    CHECK(sebk_copy_open_seed(PIN, "main", 2900000, 0, false, zfp) == SEBK_OK, "same seed again");
    CHECK(fake_se_writes() == w + 1, "rewritten");

    /* A different open seed: compare says no, copy refuses without consent. */
    uint8_t b = wallet_add_mnemonic(B);
    CHECK(b == 2 && wallet_select_wallet(b) == WALLET_OK, "store B");
    r = sebk_peek(PIN, &peek);
    CHECK(r == SEBK_OK && peek.opened && !peek.matches_open && strcmp(peek.zfp, want) == 0,
          "compare: no match");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, zfp) == SEBK_E_OTHER, "B over A unasked");
    CHECK(fake_se_writes() == w + 1, "refusal wrote");
    CHECK(sebk_peek(PIN, &peek) == SEBK_OK && !peek.matches_open, "A still there");
    /* A copy under another PIN counts as another seed too. */
    CHECK(sebk_copy_open_seed("5555", "", 0, 0, false, zfp) == SEBK_E_OTHER, "other PIN unasked");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, true, zfp) == SEBK_OK, "B over A, confirmed");
    CHECK(sebk_peek(PIN, &peek) == SEBK_OK && peek.matches_open, "B now");

    /* Erase: the container only. */
    const int nvs_writes = fake_nvs_write_count();
    CHECK(sebk_erase() == SEBK_OK, "erase");
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_EMPTY, "erased reads empty");
    CHECK(fake_nvs_write_count() == nvs_writes, "erase wrote flash");
    CHECK(wallet_get_count() == 2 && slot_is(1, A) && slot_is(2, B), "vault touched by erase");

    /* A temporary seed can be copied as well. */
    CHECK(wallet_use_temporary_mnemonic(A) == WALLET_OK, "temporary");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, zfp) == SEBK_OK && strcmp(zfp, want) == 0,
          "copy temporary");
    /* No seed open: nothing to copy. */
    wallet_lock();
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, true, zfp) == SEBK_E_NOSEED, "locked");
}

/* ----------------------------------------------------------- restore */

static void test_restore(void)
{
    printf("== restore: session, vault, never over a different seed\n");
    ctx = "restore";
    /* The copy, made on one board... */
    fresh_device();
    fake_se_reset();
    CHECK(wallet_select_wallet(wallet_add_mnemonic(A)) == WALLET_OK, "A");
    CHECK(sebk_copy_open_seed(PIN, "lbl", 123, 2, false, NULL) == SEBK_OK, "copy");

    /* ...restored on a board whose vault is new (same PIN), into the vault. */
    fresh_device();
    SeBackupPeek peek;
    uint8_t slot = 9;
    bool already = true;
    SeBackupResult r = sebk_restore_vault(PIN, &peek, &slot, &already);
    CHECK(r == SEBK_OK && slot == 1 && !already, "into empty vault: %s slot %u", sebk_strerror(r),
          slot);
    CHECK(peek.birthday == 123 && peek.account == 2 && strcmp(peek.label, "lbl") == 0, "meta");
    CHECK(wallet_get_count() == 1 && slot_is(1, A), "A in slot 1");

    /* Again: a no-op, not a second slot, and no flash write at all. */
    int nvs_writes = fake_nvs_write_count();
    r = sebk_restore_vault(PIN, &peek, &slot, &already);
    CHECK(r == SEBK_OK && slot == 1 && already, "already there");
    CHECK(fake_nvs_write_count() == nvs_writes && wallet_get_count() == 1, "no-op wrote");

    /* A vault holding a different seed: added after it, B untouched. */
    fresh_device();
    CHECK(wallet_select_wallet(wallet_add_mnemonic(B)) == WALLET_OK, "B");
    r = sebk_restore_vault(PIN, &peek, &slot, &already);
    CHECK(r == SEBK_OK && slot == 2 && !already, "beside B");
    CHECK(wallet_get_count() == 2 && slot_is(1, B) && slot_is(2, A), "B kept, A added");

    /* A full vault: refused, nothing written. */
    fresh_device();
    for (int i = 0; i < MAX_WALLETS; i++) CHECK(wallet_add_mnemonic(B) > 0, "fill %d", i);
    nvs_writes = fake_nvs_write_count();
    r = sebk_restore_vault(PIN, &peek, &slot, &already);
    CHECK(r == SEBK_E_VAULT_FULL && slot == 0, "full: %s", sebk_strerror(r));
    CHECK(fake_nvs_write_count() == nvs_writes, "full vault written");

    /* A slot that will not decrypt: refused, nothing written. */
    fresh_device();
    CHECK(wallet_add_mnemonic(B) == 1 && fake_nvs_has("colibri", "m0_1"), "layout");
    nvs_handle_t h;
    uint8_t junk[64];
    memset(junk, 0x5A, sizeof junk);
    CHECK(nvs_open("colibri", NVS_READWRITE, &h) == ESP_OK, "nvs");
    nvs_set_blob(h, "m0_1", junk, sizeof junk);
    nvs_commit(h);
    nvs_close(h);
    nvs_writes = fake_nvs_write_count();
    r = sebk_restore_vault(PIN, &peek, &slot, &already);
    CHECK(r == SEBK_E_VAULT && wallet_get_count() == 1, "unreadable slot: %s", sebk_strerror(r));
    CHECK(fake_nvs_write_count() == nvs_writes, "unreadable slot written");

    /* Into the session: a temporary seed, nothing in flash. */
    fresh_device();
    nvs_writes = fake_nvs_write_count();
    r = sebk_restore_session(PIN, &peek);
    CHECK(r == SEBK_OK && wallet_has_temporary_mnemonic() && open_phrase_is(A), "session");
    CHECK(fake_nvs_write_count() == nvs_writes && wallet_get_count() == 0, "session wrote flash");
    CHECK(!fake_nvs_contains_bytes(A, strlen(A)), "phrase in flash");

    /* A board with another PIN: the copy needs its own. */
    fresh_device_pin("5678");
    r = sebk_restore_vault("5678", &peek, &slot, &already);
    CHECK(r == SEBK_E_PIN && wallet_get_count() == 0, "board PIN does not open it");
    r = sebk_restore_vault(PIN, &peek, &slot, &already);
    CHECK(r == SEBK_OK && slot == 1 && slot_is(1, A), "with the copy's PIN");

    /* No chip, or a link that fails: said, nothing changes. */
    fake_se_present(false);
    SeBackupState st;
    CHECK(sebk_status(&st) == SEBK_E_NOCHIP, "no chip");
    CHECK(sebk_restore_vault(PIN, &peek, &slot, &already) == SEBK_E_NOCHIP, "no chip restore");
    CHECK(sebk_erase() == SEBK_E_NOCHIP, "no chip erase");
    fake_se_present(true);
    fake_se_fail_io(true);
    CHECK(sebk_restore_session(PIN, &peek) == SEBK_E_IO, "io");
    fake_se_fail_io(false);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_COPY, "copy survived");
}

/* -------------------------------------- foreign, damaged, cut, tampered */

static void test_bad_containers(void)
{
    printf("== empty, foreign, damaged, cut and tampered containers\n");
    ctx = "containers";
    fresh_device();
    fake_se_reset();
    CHECK(wallet_select_wallet(wallet_add_mnemonic(A)) == WALLET_OK, "A");
    SeBackupState st;
    SeBackupPeek peek;
    uint8_t slot;
    bool already;

    /* Someone else's data in the object: named, never written over. */
    uint8_t *img = fake_se_image();
    for (int i = 0; i < 300; i++) img[i] = (uint8_t)(i * 7 + 1);
    uint8_t before[OPTIGA_CONTAINER_OBJ_SIZE];
    memcpy(before, img, sizeof before);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_FOREIGN, "foreign object");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, true, NULL) == SEBK_E_FOREIGN, "copy over foreign");
    CHECK(memcmp(before, fake_se_image(), sizeof before) == 0, "foreign data changed");
    CHECK(sebk_restore_vault(PIN, &peek, &slot, &already) == SEBK_E_FOREIGN, "restore foreign");

    /* Our container with someone else's payload: also foreign. */
    fake_se_put_container((const uint8_t *)"not a seed copy", 15);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_FOREIGN, "LKSC, not LKSV");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, true, NULL) == SEBK_E_FOREIGN, "copy over it");
    /* A container larger than any copy: foreign, not an error. */
    static uint8_t big[1000];
    memcpy(big, "LKSV", 4);
    big[4] = 1;
    fake_se_put_container(big, sizeof big);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_FOREIGN, "oversized");

    /* Erase makes it usable again. */
    CHECK(sebk_erase() == SEBK_OK, "erase");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, NULL) == SEBK_OK, "copy after erase");

    /* Damaged: a payload byte decays, the CRC catches it. Writable. */
    img = fake_se_image();
    img[OPTIGA_CONTAINER_HEADER + 50] ^= 0x01;
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_DAMAGED, "damaged");
    CHECK(sebk_peek(PIN, &peek) == SEBK_OK && !peek.opened && peek.state == SEBK_STATE_DAMAGED,
          "peek damaged");
    CHECK(sebk_restore_session(PIN, &peek) == SEBK_E_DAMAGED, "restore damaged");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, NULL) == SEBK_OK, "copy over damaged");

    /* Cut: the magic never written. */
    memset(fake_se_image(), 0, 4);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_DAMAGED, "incomplete");
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, NULL) == SEBK_OK, "copy over incomplete");

    /* Tampered with care: CRC recomputed, so the container is "valid" and
     * only the GCM tag notices. Same answer as a wrong PIN. */
    uint8_t payload[SEBK_BLOB_MAX];
    img = fake_se_image();
    size_t plen = ((size_t)img[6] << 8) | img[7];
    memcpy(payload, img + OPTIGA_CONTAINER_HEADER, plen);
    payload[plen - 20] ^= 0x80;
    fake_se_put_container(payload, plen);
    CHECK(sebk_status(&st) == SEBK_OK && st == SEBK_STATE_COPY, "still looks like a copy");
    CHECK(sebk_restore_vault(PIN, &peek, &slot, &already) == SEBK_E_PIN, "tamper caught");
    CHECK(wallet_get_count() == 1, "tampered copy restored");
}

/* ------------------------------------------------------ write faults */

static void test_write_faults(void)
{
    printf("== a write that does not read back is not a copy\n");
    ctx = "faults";
    fresh_device();
    fake_se_reset();
    CHECK(wallet_select_wallet(wallet_add_mnemonic(A)) == WALLET_OK, "A");
    fake_se_verify_fails(true);
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, NULL) == SEBK_E_VERIFY, "chip read-back");
    fake_se_verify_fails(false);
    fake_se_reset();
    fake_se_corrupt_writes(true);
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, false, NULL) == SEBK_E_VERIFY, "silent corruption");
    fake_se_corrupt_writes(false);
    fake_se_fail_io(true);
    CHECK(sebk_copy_open_seed(PIN, "", 0, 0, true, NULL) == SEBK_E_IO, "link down");
    fake_se_fail_io(false);
}

/* -------------------------------------------------------------- SD */

static void test_sd(void)
{
    printf("== SD -> SE and SD -> vault\n");
    ctx = "sd";
    fresh_device();
    fake_se_reset();
    empty_card();
    CHECK(wallet_select_wallet(wallet_add_mnemonic(A)) == WALLET_OK, "A");
    char name[SDCARD_NAME_MAX + 1];
    CHECK(sdseed_save(PASS, strlen(PASS), "card", 777, 1, ZKV_ITERATIONS_MIN, name) == SDSEED_OK,
          "save to card");

    /* A new board: card -> SE, then SE -> vault. */
    fresh_device();
    sdcard__set_root_for_test(card);
    ZkvContents c;
    CHECK(sdseed_open(name, "wrong passphrase", 16, &c) == SDSEED_E_AUTH, "wrong passphrase");
    CHECK(sdseed_open(name, PASS, strlen(PASS), &c) == SDSEED_OK, "open card");
    char zfp[9], want[9];
    zcash_short_fp(A_FP, want);
    CHECK(sebk_copy_contents(PIN, &c, false, zfp) == SEBK_OK && strcmp(zfp, want) == 0,
          "card -> SE");
    memset(&c, 0, sizeof c);
    CHECK(wallet_get_count() == 0 && !wallet_has_temporary_mnemonic(), "card -> SE loaded a seed");
    SeBackupPeek peek;
    CHECK(sebk_peek(PIN, &peek) == SEBK_OK && peek.opened && peek.birthday == 777 &&
          peek.account == 1 && strcmp(peek.label, "card") == 0, "metadata carried");

    /* Card -> vault: empty, again, beside another seed, wrong passphrase. */
    ZkvContents meta;
    uint8_t slot;
    bool already;
    CHECK(sdseed_restore_to_vault(name, PASS, strlen(PASS), &meta, &slot, &already) == SDSEED_OK &&
          slot == 1 && !already, "card -> empty vault");
    static const uint8_t zero[ZKV_ENTROPY_MAX];
    CHECK(memcmp(meta.entropy, zero, sizeof zero) == 0 && meta.birthday == 777, "meta");
    int nvs_writes = fake_nvs_write_count();
    CHECK(sdseed_restore_to_vault(name, PASS, strlen(PASS), &meta, &slot, &already) == SDSEED_OK &&
          slot == 1 && already, "card -> vault again");
    CHECK(fake_nvs_write_count() == nvs_writes, "no-op wrote");

    fresh_device();
    CHECK(wallet_select_wallet(wallet_add_mnemonic(B)) == WALLET_OK, "B");
    sdcard__set_root_for_test(card);
    CHECK(sdseed_restore_to_vault(name, "wrong passphrase", 16, &meta, &slot, &already) ==
              SDSEED_E_AUTH && wallet_get_count() == 1, "wrong passphrase restored");
    CHECK(sdseed_restore_to_vault(name, PASS, strlen(PASS), &meta, &slot, &already) == SDSEED_OK &&
          slot == 2 && !already, "card beside B");
    CHECK(slot_is(1, B) && slot_is(2, A), "B kept");
    CHECK(!fake_nvs_contains_bytes(A, strlen(A)), "phrase in flash in clear");

    fresh_device();
    sdcard__set_root_for_test(card);
    for (int i = 0; i < MAX_WALLETS; i++) wallet_add_mnemonic(B);
    CHECK(sdseed_restore_to_vault(name, PASS, strlen(PASS), &meta, &slot, &already) ==
              SDSEED_E_VAULT_FULL, "full vault");
}

/* Picks a vector's phrase with no passphrase; its fingerprint is the copy's. */
static void pick_vectors(void)
{
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        if (ZV_KEYS[i].passphrase[0]) continue;
        if (!A) {
            A = ZV_KEYS[i].mnemonic;
            memcpy(A_FP, ZV_KEYS[i].seed_fingerprint, 32);
        } else if (strcmp(ZV_KEYS[i].mnemonic, A) != 0) {
            B = ZV_KEYS[i].mnemonic;
            break;
        }
    }
    /* zcash_fp_of_phrase() agrees with the vectors. */
    uint8_t fp[32];
    zcash_fp_of_phrase(A, fp);
    ctx = "vectors";
    CHECK(A && B && memcmp(fp, A_FP, 32) == 0, "fingerprint of the phrase");
}

int main(void)
{
    printf("== test_se_backup\n");
    snprintf(card, sizeof card, "/tmp/leek-sebk-XXXXXX");
    if (!mkdtemp(card)) {
        perror("mkdtemp");
        return 1;
    }
    sdcard__set_root_for_test(card);
    pick_vectors();
    test_format();
    test_copy_compare_erase();
    test_restore();
    test_bad_containers();
    test_write_faults();
    test_sd();
    empty_card();
    rmdir(card);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
