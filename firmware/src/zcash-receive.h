/**
 * The receive screens (roadmap 1.6) and the seed fingerprint (1.5), as text.
 *
 * Pure apart from zcash_seed_fingerprint(): an account goes in, pages of
 * eight 21-character rows or one QR come out. The UI draws them and the host
 * suite reads them, so what is tested is exactly what the panel shows.
 *
 * Pages, in order:
 *   - the unified address, whole, over as many pages as it needs;
 *   - the unified address as a QR;
 *   - the transparent address, with a warning that it is public;
 *   - the transparent address as a QR.
 *
 * QR codes carry the address exactly as encoded, lowercase, in byte mode.
 * Upper case would reach alphanumeric mode and a smaller code, but
 * zcash_address (and so Zodl) takes the HRP as written and does not know
 * "UTEST", so an uppercase UA would scan and then fail to parse.
 */
#ifndef LEEK_ZCASH_RECEIVE_H
#define LEEK_ZCASH_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "zcash-account.h"

#define ZRECV_ROWS 8
#define ZRECV_COLS 21

/* Short form of the ZIP 32 seed fingerprint: its first four bytes in hex. */
#define ZRECV_FP_CHARS 8

typedef enum {
    ZRECV_TEXT = 0,
    ZRECV_QR   = 1,
} ZRecvKind;

typedef struct {
    ZRecvKind   kind;
    char        row[ZRECV_ROWS][ZRECV_COLS + 1];   /* ZRECV_TEXT */
    const char *qr;                                /* ZRECV_QR: points into the account */
    uint8_t     version;                           /* ZRECV_QR */
    uint8_t     scale;                             /* ZRECV_QR */
} ZRecvPage;

/** Pages for this account. */
int zrecv_page_count(const ZcashAccount *a);

/** Fill `out` with page `page` (0-based). False past the last page. */
bool zrecv_render(const ZcashAccount *a, int page, ZRecvPage *out);

/**
 * The page ACCEPT goes to: from an address's text to its QR, and from the QR
 * back to the first page of its text.
 */
int zrecv_toggle_qr(const ZcashAccount *a, int page);

/**
 * The smallest QR version (1-10) that holds `len` bytes at ECC_LOW in byte
 * mode and fits the 64-row panel, at the largest scale that still fits.
 * False if none does.
 */
bool zrecv_qr_fit(int len, uint8_t *version, uint8_t *scale);

/** "1A2B3C4D": the short form of a 32-byte seed fingerprint. */
void zrecv_short_fp(const uint8_t fp[32], char out[ZRECV_FP_CHARS + 1]);

/**
 * The ZIP 32 seed fingerprint of the unlocked wallet's seed (passphrase
 * applied): BLAKE2b-256, personalisation "Zcash_HD_Seed_FP", over
 * len(seed) || seed. Cheap once the seed is cached; runs on the caller's
 * stack.
 */
WalletError zcash_seed_fingerprint(uint8_t out[32]);

#endif /* LEEK_ZCASH_RECEIVE_H */
