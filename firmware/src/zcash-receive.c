/**
 * The receive pages and the seed fingerprint. See zcash-receive.h.
 */
#include "zcash-receive.h"

#include <stdio.h>
#include <string.h>

#include "blake2b.h"
#include "memzero.h"
#include "oled.h"

/* Address rows on a unified-address page: rows 1-6, title and footer apart. */
#define UA_ROWS_PER_PAGE 6
#define UA_CHARS_PER_PAGE (UA_ROWS_PER_PAGE * ZRECV_COLS)

#define FOOTER "BACK    UP/DN    QR"

/* QR capacity in bytes at ECC_LOW, versions 1-10 (ISO/IEC 18004 table 7). */
static const uint16_t BYTE_CAPACITY[10] = { 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 };

static int ua_text_pages(const ZcashAccount *a)
{
    int len = (int)strlen(a->keys.ua);
    return len == 0 ? 1 : (len + UA_CHARS_PER_PAGE - 1) / UA_CHARS_PER_PAGE;
}

int zrecv_page_count(const ZcashAccount *a)
{
    return ua_text_pages(a) + 3;
}

bool zrecv_qr_fit(int len, uint8_t *version, uint8_t *scale)
{
    /* Bigger modules read from further away, so scale 2 wins wherever the
     * panel has room for it; only then the smallest version that holds it. */
    for (uint8_t s = 2; s >= 1; s--) {
        for (uint8_t v = 1; v <= OLED_QR_MAX_VERSION && v <= 10; v++) {
            if (len <= BYTE_CAPACITY[v - 1] && oled_qr_fits(v, s)) {
                *version = v;
                *scale = s;
                return true;
            }
        }
    }
    return false;
}

static const char *net_label(const ZcashAccount *a)
{
    return a->network == ZCASH_NET_MAIN ? "MAIN" : "TEST";
}

static void put_row(ZRecvPage *out, int row, const char *s, int n)
{
    if (n > ZRECV_COLS) n = ZRECV_COLS;
    memcpy(out->row[row], s, (size_t)n);
    out->row[row][n] = '\0';
}

static bool qr_page(const char *text, ZRecvPage *out)
{
    out->kind = ZRECV_QR;
    out->qr = text;
    return zrecv_qr_fit((int)strlen(text), &out->version, &out->scale);
}

bool zrecv_render(const ZcashAccount *a, int page, ZRecvPage *out)
{
    memset(out, 0, sizeof *out);
    const int ua_pages = ua_text_pages(a);
    if (page < 0 || page >= zrecv_page_count(a)) {
        return false;
    }

    if (page < ua_pages) {
        out->kind = ZRECV_TEXT;
        char title[48];
        snprintf(title, sizeof title, "UA %d/%d  acct %u  %s", page + 1, ua_pages,
                 (unsigned)a->account, net_label(a));
        put_row(out, 0, title, (int)strlen(title));
        const char *ua = a->keys.ua;
        int len = (int)strlen(ua);
        int at = page * UA_CHARS_PER_PAGE;
        for (int r = 0; r < UA_ROWS_PER_PAGE && at < len; r++, at += ZRECV_COLS) {
            put_row(out, 1 + r, ua + at, len - at);
        }
        snprintf(out->row[7], sizeof out->row[7], "%s", FOOTER);
        return true;
    }
    if (page == ua_pages) {
        return qr_page(a->keys.ua, out);
    }
    if (page == ua_pages + 1) {
        out->kind = ZRECV_TEXT;
        char title[48];
        snprintf(title, sizeof title, "T-addr  acct %u  %s", (unsigned)a->account, net_label(a));
        put_row(out, 0, title, (int)strlen(title));
        int len = (int)strlen(a->t_address);
        put_row(out, 2, a->t_address, len);
        if (len > ZRECV_COLS) {
            put_row(out, 3, a->t_address + ZRECV_COLS, len - ZRECV_COLS);
        }
        /* Said where it is shown, not only in the manual: anyone can see who
         * paid this address and how much. */
        snprintf(out->row[5], sizeof out->row[5], "PUBLIC: payer and");
        snprintf(out->row[6], sizeof out->row[6], "amount are visible");
        snprintf(out->row[7], sizeof out->row[7], "%s", FOOTER);
        return true;
    }
    return qr_page(a->t_address, out);
}

int zrecv_toggle_qr(const ZcashAccount *a, int page)
{
    const int ua_pages = ua_text_pages(a);
    if (page < ua_pages) return ua_pages;
    if (page == ua_pages) return 0;
    if (page == ua_pages + 1) return ua_pages + 2;
    return ua_pages + 1;
}

void zrecv_short_fp(const uint8_t fp[32], char out[ZRECV_FP_CHARS + 1])
{
    snprintf(out, ZRECV_FP_CHARS + 1, "%02X%02X%02X%02X", fp[0], fp[1], fp[2], fp[3]);
}

static WalletError fingerprint_with_seed(const uint8_t seed[64], void *ctx)
{
    blake2b_state s;
    const uint8_t len = 64;
    blake2b_InitPersonal(&s, 32, "Zcash_HD_Seed_FP", 16);
    blake2b_Update(&s, &len, 1);
    blake2b_Update(&s, seed, 64);
    blake2b_Final(&s, ctx, 32);
    memzero(&s, sizeof s);
    return WALLET_OK;
}

WalletError zcash_seed_fingerprint(uint8_t out[32])
{
    WalletError err = wallet_with_seed(fingerprint_with_seed, out);
    if (err != WALLET_OK) {
        memset(out, 0, 32);
    }
    return err;
}
