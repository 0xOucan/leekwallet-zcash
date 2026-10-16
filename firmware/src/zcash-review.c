/**
 * Transaction review pages. See zcash-review.h.
 */
#include "zcash-review.h"

#include <stdio.h>
#include <string.h>

#define BODY_FIRST 1
#define BODY_LAST  6          /* row 7 is the footer */
#define BODY_ROWS  (BODY_LAST - BODY_FIRST + 1)

/* The address rows on a recipient's first page: amount and pool come first. */
#define ADDR_FIRST_ROWS 4

static const char *ticker(uint8_t network)
{
    return network == ZS_NET_MAIN ? "ZEC" : "TAZ";
}

void zreview_amount(uint64_t zat, uint8_t network, char *out, int cap)
{
    char frac[9];
    snprintf(frac, sizeof frac, "%08llu", (unsigned long long)(zat % 100000000ull));
    int n = 8;
    while (n > 0 && frac[n - 1] == '0') frac[--n] = '\0';
    if (n == 0) {
        snprintf(out, (size_t)cap, "%llu %s", (unsigned long long)(zat / 100000000ull), ticker(network));
    } else {
        snprintf(out, (size_t)cap, "%llu.%s %s", (unsigned long long)(zat / 100000000ull), frac,
                 ticker(network));
    }
}

static const char *pool_line(uint8_t pool)
{
    switch (pool) {
        case ZS_POOL_IRONWOOD:    return "Ironwood (shielded)";
        case ZS_POOL_ORCHARD:     return "Orchard (shielded)";
        case ZS_POOL_TRANSPARENT: return "!TRANSPARENT: public";
    }
    return "Unknown pool";
}

static const char *branch_name(uint32_t id)
{
    switch (id) {
        case 0x37a5165b: return "NU6.3";
        case 0x77190ad9: return "NU7";
    }
    return "unknown";
}

static bool is_recipient(const zs_output *o)
{
    return o->kind != ZS_KIND_CHANGE;
}

static int text_pages(size_t len, int rows_first, int rows_next)
{
    size_t first = (size_t)rows_first * ZREVIEW_COLS;
    if (len <= first) return 1;
    size_t per = (size_t)rows_next * ZREVIEW_COLS;
    return 1 + (int)((len - first + per - 1) / per);
}

static int memo_pages(const zs_output *o)
{
    switch (o->memo_kind) {
        case ZS_MEMO_TEXT:        return o->memo_len ? text_pages(o->memo_len, BODY_ROWS, BODY_ROWS) : 0;
        case ZS_MEMO_BINARY:      return 1;
        case ZS_MEMO_UNRECOVERED: return o->pool == ZS_POOL_TRANSPARENT ? 0 : 1;
        default:                  return 0;
    }
}

static int output_pages(const zs_output *o)
{
    if (!is_recipient(o)) return 1;
    size_t alen = strnlen(o->address, sizeof o->address);
    return text_pages(alen, ADDR_FIRST_ROWS, BODY_ROWS) + memo_pages(o);
}

static unsigned n_tinputs(const zs_summary *s)
{
    return s->n_tinputs < ZS_MAX_T_INPUTS ? s->n_tinputs : ZS_MAX_T_INPUTS;
}

int zreview_page_count(const zs_summary *s, uint8_t network)
{
    (void)network;
    int n = 2 + (int)n_tinputs(s);  /* each input, fee and network, final question */
    for (unsigned i = 0; i < s->n_outputs && i < ZS_MAX_OUTPUTS; i++) {
        n += output_pages(&s->outputs[i]);
    }
    return n > ZREVIEW_MAX_PAGES ? ZREVIEW_MAX_PAGES : n;
}

/* ------------------------------------------------------------ drawing */

static void put(ZReviewPage *p, int row, const char *text)
{
    snprintf(p->row[row], sizeof p->row[row], "%s", text);
}

static void header(ZReviewPage *p, const char *label, int page, int total)
{
    /* Label on the left, cut to fit; "page/total" right-aligned. */
    char pn[24];
    int pl = snprintf(pn, sizeof pn, "%d/%d", page + 1, total);
    if (pl < 0 || pl > ZREVIEW_COLS - 2) pl = 0;
    char *row = p->row[0];
    memset(row, ' ', ZREVIEW_COLS);
    row[ZREVIEW_COLS] = '\0';
    memcpy(row, label, strnlen(label, (size_t)(ZREVIEW_COLS - pl - 1)));
    memcpy(row + ZREVIEW_COLS - pl, pn, (size_t)pl);
}

/* `rows` rows of `text` starting at character `from`, one per row from `row`.
 * Control characters are shown as '.', so a memo cannot draw over the
 * screen's own lines. */
static void text_rows(ZReviewPage *p, int row, int rows, const char *text, size_t len, size_t from)
{
    for (int r = 0; r < rows && from < len; r++, from += ZREVIEW_COLS) {
        size_t n = len - from < ZREVIEW_COLS ? len - from : ZREVIEW_COLS;
        for (size_t k = 0; k < n; k++) {
            unsigned char c = (unsigned char)text[from + k];
            p->row[row + r][k] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
        }
        p->row[row + r][n] = '\0';
    }
}

/* Which recipient this is, counted the way the user sees them: "Send 2 of 3". */
static void recipient_ordinal(const zs_summary *s, unsigned idx, int *nth, int *of)
{
    *nth = 0;
    *of = 0;
    for (unsigned i = 0; i < s->n_outputs && i < ZS_MAX_OUTPUTS; i++) {
        if (!is_recipient(&s->outputs[i])) continue;
        (*of)++;
        if (i <= idx) *nth = *of;
    }
}

static void render_output(const zs_summary *s, uint8_t net, unsigned idx, int sub,
                          int page, int total, ZReviewPage *p)
{
    const zs_output *o = &s->outputs[idx];
    char label[48], amount[32];

    if (!is_recipient(o)) {
        header(p, "Change", page, total);
        zreview_amount(o->value, net, amount, sizeof amount);
        put(p, 1, amount);
        put(p, 2, pool_line(o->pool));
        put(p, 3, "Back to this wallet");
        return;
    }

    int nth, of;
    recipient_ordinal(s, idx, &nth, &of);
    const char *verb = o->kind == ZS_KIND_SELF ? "To self" : "Send";
    size_t alen = strnlen(o->address, sizeof o->address);
    int apages = text_pages(alen, ADDR_FIRST_ROWS, BODY_ROWS);

    if (sub == 0) {
        snprintf(label, sizeof label, "%s %d of %d", verb, nth, of);
        header(p, label, page, total);
        zreview_amount(o->value, net, amount, sizeof amount);
        put(p, 1, amount);
        put(p, 2, pool_line(o->pool));
        text_rows(p, 3, ADDR_FIRST_ROWS, o->address, alen, 0);
    } else if (sub < apages) {
        snprintf(label, sizeof label, "%s %d addr %d/%d", verb, nth, sub + 1, apages);
        header(p, label, page, total);
        size_t from = (size_t)ADDR_FIRST_ROWS * ZREVIEW_COLS +
                      (size_t)(sub - 1) * BODY_ROWS * ZREVIEW_COLS;
        text_rows(p, BODY_FIRST, BODY_ROWS, o->address, alen, from);
    } else {
        int m = sub - apages, mp = memo_pages(o);
        snprintf(label, sizeof label, "%s %d memo %d/%d", verb, nth, m + 1, mp);
        header(p, label, page, total);
        if (o->memo_kind == ZS_MEMO_TEXT) {
            size_t mlen = o->memo_len > sizeof o->memo ? sizeof o->memo : o->memo_len;
            text_rows(p, BODY_FIRST, BODY_ROWS, (const char *)o->memo, mlen,
                      (size_t)m * BODY_ROWS * ZREVIEW_COLS);
        } else if (o->memo_kind == ZS_MEMO_BINARY) {
            put(p, 2, "Binary memo");
            snprintf(label, sizeof label, "%u bytes", (unsigned)o->memo_len);
            put(p, 3, label);
        } else {
            put(p, 2, "Memo not readable");
            put(p, 3, "by this device");
        }
    }
}

static const char *scope_name(uint32_t scope)
{
    switch (scope) {
        case 0: return "receive";
        case 1: return "change";
        case 2: return "ephemeral";
    }
    return "?";
}

/* A transparent input: the device derived its key at this path and checked
 * that the coin's script pays to it, so it is shown as this wallet's. */
static void render_tinput(const zs_summary *s, uint8_t net, unsigned idx, int page, int total,
                          ZReviewPage *p)
{
    const zs_tinput *t = &s->tinputs[idx];
    char label[40], amount[32];
    snprintf(label, sizeof label, "From %u of %u", idx + 1, n_tinputs(s));
    header(p, label, page, total);
    zreview_amount(t->value, net, amount, sizeof amount);
    put(p, 1, amount);
    put(p, 2, "Your t-address:");
    text_rows(p, 3, 2, t->address, strnlen(t->address, sizeof t->address), 0);
    snprintf(label, sizeof label, "Path .../%lu/%lu", (unsigned long)t->scope,
             (unsigned long)t->index);
    put(p, 5, label);
    snprintf(label, sizeof label, "(%s address)", scope_name(t->scope));
    put(p, 6, label);
}

static void render_fee(const zs_summary *s, uint8_t net, int page, int total, ZReviewPage *p)
{
    char line[40], amount[32];
    header(p, "Fee & network", page, total);
    zreview_amount(s->fee, net, amount, sizeof amount);
    snprintf(line, sizeof line, "Fee %s", amount);
    put(p, 1, line);
    put(p, 2, net == ZS_NET_MAIN ? "Network: Mainnet" : "Network: Testnet");
    snprintf(line, sizeof line, "Upgrade: %s", branch_name(s->branch_id));
    put(p, 3, line);
    snprintf(line, sizeof line, "Spends: %u", (unsigned)s->n_spends);
    put(p, 4, line);
    if (n_tinputs(s)) {
        snprintf(line, sizeof line, "From t-addr: %u", n_tinputs(s));
        put(p, 5, line);
    }
    if (s->warnings & ZS_WARN_TRANSPARENT_OUT) put(p, 6, "!Has public output");
}

static void render_final(const zs_summary *s, uint8_t net, int page, int total, ZReviewPage *p)
{
    uint64_t sent = 0;
    int n = 0;
    for (unsigned i = 0; i < s->n_outputs && i < ZS_MAX_OUTPUTS; i++) {
        if (s->outputs[i].kind == ZS_KIND_EXTERNAL) {
            sent += s->outputs[i].value;
            n++;
        }
    }
    char line[40], amount[32];
    header(p, "Sign?", page, total);
    if (n == 0 && n_tinputs(s)) {
        /* Nothing leaves the wallet: transparent funds move to its
         * shielded pool. */
        zreview_amount(s->t_in, net, amount, sizeof amount);
        snprintf(line, sizeof line, "Shield %s", amount);
        put(p, 2, line);
        zreview_amount(s->fee, net, amount, sizeof amount);
        snprintf(line, sizeof line, "fee %s", amount);
        put(p, 3, line);
        put(p, 4, "into this wallet");
        return;
    }
    zreview_amount(sent, net, amount, sizeof amount);
    snprintf(line, sizeof line, "Send %s", amount);
    put(p, 2, line);
    zreview_amount(s->fee, net, amount, sizeof amount);
    snprintf(line, sizeof line, "+fee %s", amount);
    put(p, 3, line);
    snprintf(line, sizeof line, "to %d recipient%s", n, n == 1 ? "" : "s");
    put(p, 4, line);
}

bool zreview_render(const zs_summary *s, uint8_t network, int page, ZReviewPage *out)
{
    memset(out, 0, sizeof *out);
    int total = zreview_page_count(s, network);
    if (page < 0 || page >= total) return false;

    put(out, 7, page == total - 1 ? "REJECT           SIGN" : "REJECT   ^v      NEXT");
    if (page == total - 1) {
        render_final(s, network, page, total, out);
        return true;
    }
    if (page == total - 2) {
        render_fee(s, network, page, total, out);
        return true;
    }

    /* Where the money comes from first (transparent inputs; shielded
     * spends are counted on the fee page), then recipients, then change,
     * as the user thinks of them. */
    if (page < (int)n_tinputs(s)) {
        render_tinput(s, network, (unsigned)page, page, total, out);
        return true;
    }
    int at = (int)n_tinputs(s);
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < s->n_outputs && i < ZS_MAX_OUTPUTS; i++) {
            const zs_output *o = &s->outputs[i];
            if (is_recipient(o) != (pass == 0)) continue;
            int k = output_pages(o);
            if (page < at + k) {
                render_output(s, network, i, page - at, page, total, out);
                return true;
            }
            at += k;
        }
    }
    return false;
}
