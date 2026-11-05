/**
 * SD card airgap signing. See sd-sign.h.
 */
#include "sd-sign.h"

#include <stdlib.h>
#include <string.h>

#include "memzero.h"
#include "sdcard.h"
#include "zcash-sign.h"

#define CHUNK 4096

static bool    loaded;
static uint8_t loaded_sha[32];

bool sdsign_available(void)
{
    size_t size = 0;
    return sdcard_mount() == SDCARD_OK && sdcard_size(SDSIGN_IN, &size) == SDCARD_OK && size > 0;
}

static int from_sdcard(int rc)
{
    switch (rc) {
        case SDCARD_OK:          return 0;
        case SDCARD_E_NOCARD:    return SDSIGN_E_NOCARD;
        case SDCARD_E_NOTFOUND:  return SDSIGN_E_NOFILE;
        default:                 return SDSIGN_E_IO;
    }
}

/*
 * Clear the way for this PCZT's answer, before the review starts.
 *
 * A signed.zsig names the SHA-256 of the PCZT it signs (FORMATS.md), and
 * every reader checks that (zsig.rs parse_for, leek-zcash apply_zsig,
 * leek-android apply_signatures_to). So an old one cannot be applied to the
 * wrong transaction; but left beside a new unsigned.pczt it is a file that
 * looks like the answer, for a user whose signing was then rejected or
 * failed. It goes now, so that what is on the card is only ever this PCZT's
 * signatures or nothing. One that names this very file stays: it is still
 * a good answer if this review is rejected.
 *
 * signed.zsig.tmp is what a cut inside sdcard_write_replace() can leave. It
 * may be whole, but nothing on the card says so, so it is never promoted.
 */
static int drop_stale_output(const uint8_t sha[32])
{
    if (sdcard_remove(SDSIGN_OUT ".tmp") != SDCARD_OK) return SDSIGN_E_IO;
    uint8_t head[37];
    size_t got = 0;
    int rc = sdcard_read_at(SDSIGN_OUT, 0, head, sizeof head, &got);
    if (rc == SDCARD_E_NOTFOUND) return 0;
    if (rc != SDCARD_OK) return SDSIGN_E_IO;
    if (got == sizeof head && memcmp(head, "ZSIG", 4) == 0 && memcmp(head + 5, sha, 32) == 0) {
        return 0;
    }
    return sdcard_remove(SDSIGN_OUT) == SDCARD_OK ? 0 : SDSIGN_E_IO;
}

int sdsign_load(uint32_t account, uint8_t network, const zs_summary **summary)
{
    sdsign_cancel();
    *summary = NULL;
    if (sdcard_mount() != SDCARD_OK) return SDSIGN_E_NOCARD;

    size_t size = 0;
    int rc = from_sdcard(sdcard_sha256(SDSIGN_IN, loaded_sha, &size));
    if (rc) return rc;
    if (size == 0 || size > ZCASH_PCZT_MAX) return ZSIGN_E_TOO_LARGE;

    /* The hash the signer checks the bytes against is the one just taken:
     * the same discipline as a USB upload, where the host declares it. */
    rc = zcash_sign_begin((uint32_t)size, loaded_sha, account, network);
    if (rc) return rc;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        zcash_sign_reset();
        return ZSIGN_E_NOMEM;
    }
    for (size_t off = 0; off < size && rc == 0;) {
        size_t got = 0;
        rc = from_sdcard(sdcard_read_at(SDSIGN_IN, off, buf, CHUNK, &got));
        if (rc == 0 && got == 0) rc = SDSIGN_E_IO;     /* shrank under us */
        if (rc == 0) {
            if (got > size - off) got = size - off;
            rc = zcash_sign_chunk((uint32_t)off, buf, got);
            off += got;
        }
    }
    free(buf);
    if (rc == 0) rc = zcash_sign_check(summary);
    /* A card that cannot lose its old answer cannot take a new one either:
     * refuse now rather than after the user has read every page. */
    if (rc == 0) rc = drop_stale_output(loaded_sha);
    if (rc != 0) {
        zcash_sign_reset();
        *summary = NULL;
        return rc;
    }
    loaded = true;
    return 0;
}

size_t sdsign_zsig_encode(const uint8_t sha256[32], const zs_sig *sigs, uint32_t n,
                          uint8_t *out, size_t cap)
{
    /* Version 1 unless an entry is transparent: a shielded-only file stays
     * byte for byte what version 1 readers expect. */
    uint8_t version = 1;
    for (uint32_t i = 0; i < n; i++)
        if (sigs[i].pool == ZS_POOL_TRANSPARENT) version = 2;
    if (n > 0xFFFF || cap < 39) return 0;
    memcpy(out, "ZSIG", 4);
    out[4] = version;
    memcpy(out + 5, sha256, 32);
    out[37] = (uint8_t)n;
    out[38] = (uint8_t)(n >> 8);
    size_t at = 39;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t der[ZCASH_TSIG_MAX];
        size_t dl = 0, need = 69;
        if (sigs[i].pool == ZS_POOL_TRANSPARENT) {
            dl = zcash_tsig_encode(&sigs[i], der);
            need = 1 + 4 + 1 + dl + 33;
        }
        if (cap - at < need) return 0;
        uint8_t *e = out + at;
        e[0] = sigs[i].pool;
        e[1] = (uint8_t)sigs[i].index;
        e[2] = (uint8_t)(sigs[i].index >> 8);
        e[3] = (uint8_t)(sigs[i].index >> 16);
        e[4] = (uint8_t)(sigs[i].index >> 24);
        if (sigs[i].pool == ZS_POOL_TRANSPARENT) {
            e[5] = (uint8_t)dl;
            memcpy(e + 6, der, dl);
            memcpy(e + 6 + dl, sigs[i].pubkey, 33);
        } else {
            memcpy(e + 5, sigs[i].sig, 64);
        }
        at += need;
    }
    return at;
}

int sdsign_finish(uint32_t *n_sigs)
{
    *n_sigs = 0;
    if (!loaded) return SDSIGN_E_STATE;
    loaded = false;

    uint8_t now[32];
    int rc = from_sdcard(sdcard_sha256(SDSIGN_IN, now, NULL));
    if (rc == 0 && memcmp(now, loaded_sha, 32) != 0) rc = SDSIGN_E_CHANGED;
    if (rc != 0) {
        /* Nothing is signed for a card that changed or went away. */
        zcash_sign_reset();
        return rc;
    }

    static zs_sig sigs[ZS_MAX_SIGS];
    uint32_t n = 0;
    rc = zcash_sign_sign(sigs, ZS_MAX_SIGS, &n);
    if (rc != 0) return rc;

    static uint8_t zsig[SDSIGN_ZSIG_MAX];
    size_t len = sdsign_zsig_encode(loaded_sha, sigs, n, zsig, sizeof zsig);
    memzero(sigs, sizeof sigs);
    /* Through a temporary and read back (sdcard.h): on any error the card
     * holds no signed.zsig, or the old one for this same file, never a short
     * one. Signatures are not secrets, so nothing here needs wiping. */
    rc = len ? from_sdcard(sdcard_write_replace(SDSIGN_OUT, zsig, len)) : SDSIGN_E_IO;
    if (rc == 0) *n_sigs = n;
    return rc;
}

void sdsign_cancel(void)
{
    if (loaded) zcash_sign_reset();
    loaded = false;
}

const char *sdsign_strerror(int code)
{
    switch (code) {
        case SDSIGN_E_NOCARD:  return "No SD card";
        case SDSIGN_E_NOFILE:  return "No LEEKZ/unsigned.pczt";
        case SDSIGN_E_IO:      return "Card read/write error";
        case SDSIGN_E_CHANGED: return "File changed: not signed";
        case SDSIGN_E_STATE:   return "Nothing to sign";
        default:               return zcash_sign_strerror(code);
    }
}
