/* The QR entrance. See airgap.h: a stub that assembles URs and refuses them
   all until Phase 5 adds the zcash-pczt type. */

#include "airgap.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "memzero.h"
#include "ur-decoder.h"

static const char *TAG = "airgap";

/* ------------------------------------------------------------ assembly */

/* Sized as the first QR entrance had it - 2 KB of fragments and
   UR_DECODER_MIXED fragments of up to 512 bytes, larger than any frame a phone
   animates. The decoder refuses anything that would not fit at the first part,
   so a sender cannot walk it past these. Phase 5 resizes them for a PCZT. */
static uint8_t  scan_fragments[2048];
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

    /* Complete. No type is accepted yet - the zcash-pczt gate lands here in
       Phase 5 - so every message is refused, and the assembly is dropped so
       the next scan starts clean rather than pinned to this message. */
    ESP_LOGW(TAG, "refusing a complete ur:%s; no UR type is signable yet",
             scan.type);
    airgap_scan_reset();
    return AIRGAP_SCAN_WRONG_TYPE;
}
