/**
 * SD card storage mode. See sd-seed.h.
 */
#include "sd-seed.h"

#include <stdio.h>
#include <string.h>

#include "bip39.h"
#include "leek-wallet.h"
#include "memzero.h"

/* Long enough for 24 words of the English list. */
#define PHRASE_MAX 256

static SdSeedResult from_zkv(int rc)
{
    switch (rc) {
        case ZKV_OK:     return SDSEED_OK;
        case ZKV_E_AUTH: return SDSEED_E_AUTH;
        case ZKV_E_PASS: return SDSEED_E_PASS;
        default:         return SDSEED_E_FORMAT;
    }
}

int sdseed_list(char names[][SDCARD_NAME_MAX + 1], int max)
{
    if (sdcard_mount() != SDCARD_OK) return -1;
    int n = sdcard_list(".ZKV", names, max);
    return n < 0 ? -1 : n;
}

SdSeedResult sdseed_load(const char *name, const char *pass, size_t pass_len, ZkvContents *meta)
{
    memset(meta, 0, sizeof *meta);
    if (sdcard_mount() != SDCARD_OK) return SDSEED_E_NOCARD;

    uint8_t file[ZKV_FILE_MAX];
    size_t len = 0;
    int rc = sdcard_read(name, file, sizeof file, &len);
    if (rc == SDCARD_E_TOOBIG) return SDSEED_E_FORMAT;
    if (rc != SDCARD_OK) return SDSEED_E_IO;

    ZkvContents c;
    SdSeedResult r = from_zkv(zkv_open(file, len, pass, pass_len, &c));
    if (r != SDSEED_OK) return r;

    const char *phrase = mnemonic_from_data(c.entropy, c.entropy_len);
    r = SDSEED_E_WALLET;
    if (phrase && wallet_use_temporary_mnemonic(phrase) == WALLET_OK) {
        r = SDSEED_OK;
        *meta = c;
        memzero(meta->entropy, sizeof meta->entropy);
    }
    /* trezor-crypto keeps the phrase in a static buffer until cleared. */
    mnemonic_clear();
    memzero(&c, sizeof c);
    return r;
}

SdSeedResult sdseed_save(const char *pass, size_t pass_len, const char *label, uint32_t birthday,
                         uint32_t account, uint32_t iterations, char out_name[SDCARD_NAME_MAX + 1])
{
    out_name[0] = '\0';
    if (!zkv_pass_ok(pass, pass_len)) return SDSEED_E_PASS;
    if (sdcard_mount() != SDCARD_OK) return SDSEED_E_NOCARD;

    char phrase[PHRASE_MAX];
    if (wallet_get_mnemonic(phrase, sizeof phrase) != WALLET_OK) {
        memzero(phrase, sizeof phrase);
        return SDSEED_E_WALLET;
    }
    ZkvContents c;
    memset(&c, 0, sizeof c);
    uint8_t bits[33];
    int nbits = mnemonic_to_bits(phrase, bits);
    memzero(phrase, sizeof phrase);
    /* ENT + ENT/32 bits: 132 for 12 words up to 264 for 24. */
    if (nbits < 132 || nbits > 264 || nbits % 33 != 0) {
        memzero(bits, sizeof bits);
        return SDSEED_E_WALLET;
    }
    c.entropy_len = (uint8_t)(nbits * 32 / 33 / 8);
    memcpy(c.entropy, bits, c.entropy_len);
    memzero(bits, sizeof bits);
    c.birthday = birthday;
    c.account = account;
    snprintf(c.label, sizeof c.label, "%s", label ? label : "");

    uint8_t file[ZKV_FILE_MAX];
    size_t len = 0;
    SdSeedResult r = from_zkv(zkv_seal(&c, pass, pass_len,
                                       iterations ? iterations : ZKV_ITERATIONS_DEFAULT, file,
                                       sizeof file, &len));
    if (r == SDSEED_OK) {
        char name[SDCARD_NAME_MAX + 1];
        if (!sdcard_free_name("SEED", "ZKV", name)) {
            r = SDSEED_E_FULL;
        } else if (sdcard_write_new(name, file, len) != SDCARD_OK) {
            r = SDSEED_E_IO;
        } else {
            /* Read back and open before saying it worked: a card that cannot
             * be opened again is not a backup. */
            uint8_t back[ZKV_FILE_MAX];
            size_t blen = 0;
            ZkvContents check;
            if (sdcard_read(name, back, sizeof back, &blen) != SDCARD_OK || blen != len ||
                memcmp(back, file, len) != 0 ||
                zkv_open(back, blen, pass, pass_len, &check) != ZKV_OK ||
                memcmp(check.entropy, c.entropy, c.entropy_len) != 0) {
                r = SDSEED_E_VERIFY;
            } else {
                snprintf(out_name, SDCARD_NAME_MAX + 1, "%s", name);
            }
            memzero(&check, sizeof check);
            memzero(back, sizeof back);
        }
    }
    memzero(&c, sizeof c);
    return r;
}

const char *sdseed_strerror(SdSeedResult r)
{
    switch (r) {
        case SDSEED_OK:       return "OK";
        case SDSEED_E_NOCARD: return "No SD card";
        case SDSEED_E_IO:     return "Card read/write error";
        case SDSEED_E_FULL:   return "Card has 99 seeds";
        case SDSEED_E_FORMAT: return "Not a seed file";
        case SDSEED_E_AUTH:   return "Wrong passphrase";
        case SDSEED_E_PASS:   return "Pass: 8-64 chars";
        case SDSEED_E_WALLET: return "No seed to use";
        case SDSEED_E_VERIFY: return "Did not read back";
        default:              return "Error";
    }
}
