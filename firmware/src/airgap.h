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
} AirgapScan;

/** Start the air gap. Once, at boot. Nothing to start until Phase 5. */
void airgap_start(void);

/** Forget any partial assembly. The scan screen calls this on entry. */
void airgap_scan_reset(void);

/** Feed one decoded QR string. UI task only. */
AirgapScan airgap_scan_feed(const char *ur, size_t len);

/** Parts still missing from the current assembly, for a progress line. */
uint32_t airgap_scan_remaining(void);

#endif /* LEEK_AIRGAP_H */
