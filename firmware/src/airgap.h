/**
 * The QR entrance, from scanned strings to a complete UR.
 *
 *   camera strings ─► airgap_scan_feed ─► ur-decoder ─► (type gate)
 *
 * ---------------------------------------------------------------------------
 * A stub, on purpose
 *
 * LeekWallet accepted one UR type here, `eth-sign-request`, and handed it to
 * the Ethereum signing path. That path is gone (roadmap 1.1), and nothing
 * else is signable yet, so every complete UR is refused as the wrong type.
 * The scanning machinery - camera, viewfinder, multi-part fountain assembly -
 * is kept and still runs, so a scan reports progress and then says plainly
 * that it cannot use what it read.
 *
 * Phase 5 adds the `zcash-pczt` type here: a complete PCZT goes to a worker
 * that asks the user and hands a signed PCZT back over SCREEN_QR_OUT. Until
 * then nothing scanned can reach a key.
 */

#ifndef LEEK_AIRGAP_H
#define LEEK_AIRGAP_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AIRGAP_SCAN_ACCEPTED,      /* a part was taken; keep scanning */
    AIRGAP_SCAN_REDUNDANT,     /* a repeat; keep scanning */
    AIRGAP_SCAN_UNREADABLE,    /* not a UR part this assembly can use */
    AIRGAP_SCAN_WRONG_TYPE,    /* a complete UR of a type this device refuses */
    AIRGAP_SCAN_PCZT,          /* a complete ur:leek-pczt: call airgap_load_pczt */
} AirgapScan;

/*
 * The QR air gap for Zcash (FORMATS.md): the companion shows the signer-view
 * PCZT as ur:leek-pczt (a CBOR byte string, multi-part), the device checks it
 * exactly as a USB upload, the user reviews the same pages, and the device
 * shows its signatures as ur:leek-zsig (a CBOR byte string holding a .zsig).
 */
#define AIRGAP_UR_PCZT "leek-pczt"
#define AIRGAP_UR_ZSIG "leek-zsig"

#include "zsign_rust.h"

/** Check the scanned PCZT for `account` on `network`; `*summary` is the review. */
int airgap_load_pczt(uint32_t account, uint8_t network, const zs_summary **summary);

/**
 * After the user approved: sign, and put the CBOR of ur:leek-zsig in `out`.
 * `*len` is its length. The scanned PCZT is consumed either way.
 */
int airgap_sign(uint8_t *out, size_t cap, size_t *len);

/** Drop a scanned PCZT without signing. */
void airgap_cancel(void);

/** Start the air gap. Once, at boot. */
void airgap_start(void);

/** Forget any partial assembly. The scan screen calls this on entry. */
void airgap_scan_reset(void);

/** Feed one decoded QR string. UI task only. */
AirgapScan airgap_scan_feed(const char *ur, size_t len);

/** Parts still missing from the current assembly, for a progress line. */
uint32_t airgap_scan_remaining(void);

#endif /* LEEK_AIRGAP_H */
