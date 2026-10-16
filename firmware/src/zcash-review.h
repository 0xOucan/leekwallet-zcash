/**
 * The transaction review pages (roadmap 3.4), as text.
 *
 * Pure: a summary the signers produced goes in, eight rows of at most 21
 * characters come out for each page. The UI draws them and the host suite
 * reads them, so what is tested is exactly what the panel shows.
 *
 * Pages, in order:
 *   - each transparent input, one page: amount, "your t-address" in full
 *     and its BIP 44 path (the signers re-derived its key);
 *   - each recipient: pool, amount, the whole address (continued over as
 *     many pages as it needs), a warning for a transparent (public) output;
 *     its memo on pages of its own;
 *   - each change or self output, one page;
 *   - fee, network, network upgrade and spend count;
 *   - the final question. Only there does ACCEPT sign.
 */
#ifndef LEEK_ZCASH_REVIEW_H
#define LEEK_ZCASH_REVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "zsign_rust.h"

#define ZREVIEW_ROWS 8
#define ZREVIEW_COLS 21
#define ZREVIEW_MAX_PAGES 160

typedef struct {
    char row[ZREVIEW_ROWS][ZREVIEW_COLS + 1];
} ZReviewPage;

/** Pages for this summary, final question included. */
int zreview_page_count(const zs_summary *s, uint8_t network);

/** Fill `out` with page `page` (0-based). False past the last page. */
bool zreview_render(const zs_summary *s, uint8_t network, int page, ZReviewPage *out);

/** Zatoshis as "1.2345 ZEC" / "TAZ" on testnet, no trailing zeros. */
void zreview_amount(uint64_t zat, uint8_t network, char *out, int cap);

#endif /* LEEK_ZCASH_REVIEW_H */
