/* The QR entrance. See airgap.h: ur:leek-pczt in, ur:leek-zsig out. */

#include "airgap.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "board.h"
#include "memzero.h"
#include "ur-decoder.h"
#if LEEK_HAS_ZCASH && LEEK_HAS_SDCARD
#define AIRGAP_SIGNS 1
#include "sd-sign.h"
#include "sha2.h"
#include "zcash-sign.h"
#else
#define AIRGAP_SIGNS 0
#endif

static const char *TAG = "airgap";

/* ------------------------------------------------------------ assembly */

/* A signer-view PCZT is a few kilobytes for an ordinary send (2.8 KB for one
   spend with a memo); 16 KB of fragments leaves room for many actions. The
   decoder refuses anything that would not fit at the first part, so a sender
   cannot walk it past these. Mixed parts as before: up to 512-byte fragments,
   larger than any frame a phone animates. */
static uint8_t  scan_fragments[16384];
static uint8_t  scan_mixed[UR_DECODER_MIXED * 512];
static UrDecoder scan;
static bool     scan_ready;

void airgap_start(void)
{
    /* No worker yet: nothing scanned is signable. Phase 5 starts one here. */
}

void airgap_scan_reset(void)
{
    if (!scan_ready) {
        ur_decoder_init(&scan, scan_fragments, sizeof scan_fragments,
                        scan_mixed, sizeof scan_mixed);
        scan_ready = true;
    } else {
        ur_decoder_reset(&scan);
    }
    memzero(scan_fragments, sizeof scan_fragments);
    memzero(scan_mixed, sizeof scan_mixed);
}

uint32_t airgap_scan_remaining(void)
{
    return scan_ready ? ur_decoder_remaining(&scan) : 0;
}

AirgapScan airgap_scan_feed(const char *ur, size_t len)
{
    if (!scan_ready) {
        airgap_scan_reset();
    }
    const UrPartResult r = ur_decoder_receive(&scan, ur, len);
    if (r == UR_PART_ACCEPTED) {
        return AIRGAP_SCAN_ACCEPTED;
    }
    if (r == UR_PART_REDUNDANT) {
        return AIRGAP_SCAN_REDUNDANT;
    }
    if (r == UR_PART_REJECTED) {
        return AIRGAP_SCAN_UNREADABLE;
    }

    /* Complete. One type is signable; anything else is refused and the
       assembly dropped, so the next scan starts clean. */
    if (strcmp(scan.type, AIRGAP_UR_PCZT) == 0) {
        return AIRGAP_SCAN_PCZT;
    }
    ESP_LOGW(TAG, "refusing a complete ur:%s", scan.type);
    airgap_scan_reset();
    return AIRGAP_SCAN_WRONG_TYPE;
}

#if AIRGAP_SIGNS
/* The bytes of a CBOR byte string that is the whole message, or NULL. */
static const uint8_t *cbor_bstr(const uint8_t *m, size_t len, size_t *out_len)
{
    if (len < 1 || (m[0] >> 5) != 2) return NULL;
    const uint8_t ai = m[0] & 0x1f;
    size_t hdr, n;
    if (ai < 24) { hdr = 1; n = ai; }
    else if (ai == 24 && len >= 2) { hdr = 2; n = m[1]; }
    else if (ai == 25 && len >= 3) { hdr = 3; n = (size_t)m[1] << 8 | m[2]; }
    else if (ai == 26 && len >= 5) {
        hdr = 5;
        n = (size_t)m[1] << 24 | (size_t)m[2] << 16 | (size_t)m[3] << 8 | m[4];
    } else return NULL;
    if (hdr + n != len) return NULL;     /* exactly one byte string, nothing after */
    *out_len = n;
    return m + hdr;
}

static uint8_t loaded_sha[32];
static bool    loaded;

int airgap_load_pczt(uint32_t account, uint8_t network, const zs_summary **summary)
{
    *summary = NULL;
    size_t mlen = 0, plen = 0;
    const uint8_t *m = ur_decoder_message(&scan, &mlen);
    const uint8_t *pczt = m ? cbor_bstr(m, mlen, &plen) : NULL;
    int rc;
    if (!pczt || plen == 0) {
        rc = ZS_E_PARSE;
    } else {
        sha256_Raw(pczt, plen, loaded_sha);
        rc = zcash_sign_begin((uint32_t)plen, loaded_sha, account, network);
        if (rc == 0) rc = zcash_sign_chunk(0, pczt, plen);
        if (rc == 0) rc = zcash_sign_check(summary);
    }
    /* The bytes now live in the signer's buffer; the scan's copy goes. */
    airgap_scan_reset();
    if (rc != 0) {
        zcash_sign_reset();
        *summary = NULL;
        return rc;
    }
    loaded = true;
    return 0;
}

int airgap_sign(uint8_t *out, size_t cap, size_t *len)
{
    *len = 0;
    if (!loaded) return ZSIGN_E_STATE;
    loaded = false;
    static zs_sig sigs[ZS_MAX_SIGS];
    uint32_t n = 0;
    int rc = zcash_sign_sign(sigs, ZS_MAX_SIGS, &n);
    if (rc != 0) return rc;
    /* CBOR byte string around the .zsig, so ur:leek-zsig decodes like any UR. */
    static uint8_t zsig[4 + 1 + 32 + 2 + ZS_MAX_SIGS * 69];
    size_t zl = sdsign_zsig_encode(loaded_sha, sigs, n, zsig, sizeof zsig);
    memzero(sigs, sizeof sigs);
    if (zl == 0 || zl + 3 > cap) return ZSIGN_E_TOO_LARGE;
    size_t h;
    if (zl < 24) { out[0] = (uint8_t)(0x40 | zl); h = 1; }
    else if (zl < 256) { out[0] = 0x58; out[1] = (uint8_t)zl; h = 2; }
    else { out[0] = 0x59; out[1] = (uint8_t)(zl >> 8); out[2] = (uint8_t)zl; h = 3; }
    memcpy(out + h, zsig, zl);
    *len = h + zl;
    return 0;
}

void airgap_cancel(void)
{
    if (loaded) zcash_sign_reset();
    loaded = false;
}
#else
/* No Zcash signer on this board (the C3): a scanned PCZT is refused. */
int airgap_load_pczt(uint32_t account, uint8_t network, const zs_summary **summary)
{
    (void)account; (void)network;
    *summary = NULL;
    airgap_scan_reset();
    return -1;
}
int airgap_sign(uint8_t *out, size_t cap, size_t *len)
{
    (void)out; (void)cap;
    *len = 0;
    return -1;
}
void airgap_cancel(void) {}
#endif
