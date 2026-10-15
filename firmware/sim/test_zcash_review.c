/* The transaction review pages (src/zcash-review.c): exactly what the panel
 * shows for a summary.
 *
 * One snapshot of the common send, then properties every summary must have:
 * no row wider than the panel, every address reassembles in full from its
 * pages, a memo cannot draw control characters, ACCEPT-to-sign appears on
 * the last page only, and a transparent output is called public twice.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "zcash-review.h"

static int failures;
static const char *ctx = "";
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL %s: ", ctx); printf(__VA_ARGS__); \
                                        printf("\n"); failures++; } } while (0)

static const char *UA =
    "utest1lpcp9q0703pkz3g8cqn7gnedx35pfcps0uwm4a84cwz8sxpsm8yrgez2g0ncw9tf5a96fpazsftx8dqaqqaq0qdcwlmzsvds9uc4r9z3";

static void dump(const zs_summary *s, uint8_t net)
{
    int n = zreview_page_count(s, net);
    for (int i = 0; i < n; i++) {
        ZReviewPage p;
        zreview_render(s, net, i, &p);
        printf("    +---------------------+\n");
        for (int r = 0; r < ZREVIEW_ROWS; r++) printf("    |%-21s|\n", p.row[r]);
    }
    printf("    +---------------------+\n");
}

static zs_output *add(zs_summary *s, uint8_t pool, uint8_t kind, uint64_t v, const char *addr)
{
    zs_output *o = &s->outputs[s->n_outputs++];
    o->pool = pool;
    o->kind = kind;
    o->value = v;
    snprintf(o->address, sizeof o->address, "%s", addr);
    return o;
}

static void set_memo(zs_output *o, const char *m, size_t len)
{
    o->memo_kind = ZS_MEMO_TEXT;
    o->memo_len = (uint16_t)len;
    memcpy(o->memo, m, len);
}

static bool page_is(const ZReviewPage *p, const char *const want[ZREVIEW_ROWS])
{
    for (int r = 0; r < ZREVIEW_ROWS; r++) {
        if (strcmp(p->row[r], want[r]) != 0) {
            printf("    row %d: got \"%s\", want \"%s\"\n", r, p->row[r], want[r]);
            return false;
        }
    }
    return true;
}

/* Every summary: widths, final-only SIGN, and each recipient's address
 * reassembles from the rows shown under its header. */
static void properties(const zs_summary *s, uint8_t net)
{
    int n = zreview_page_count(s, net);
    char seen[ZS_MAX_OUTPUTS][ZS_UA_MAX * 2];
    memset(seen, 0, sizeof seen);
    ZReviewPage p;
    for (int i = 0; i < n; i++) {
        CHECK(zreview_render(s, net, i, &p), "page %d not rendered", i);
        for (int r = 0; r < ZREVIEW_ROWS; r++) {
            CHECK(strlen(p.row[r]) <= ZREVIEW_COLS, "page %d row %d too wide", i, r);
            for (const char *c = p.row[r]; *c; c++) CHECK(*c >= 0x20 && *c < 0x7f, "control char");
        }
        bool sign = strstr(p.row[7], "SIGN") != NULL;
        CHECK(sign == (i == n - 1), "SIGN on page %d of %d", i + 1, n);
        /* "Send k of m" / "Send k addr" pages carry address rows. */
        int k = 0, first_row = 0;
        bool numbered = sscanf(p.row[0], "Send %d", &k) == 1 || sscanf(p.row[0], "To self %d", &k) == 1;
        if (numbered && strstr(p.row[0], " of ")) first_row = 3;
        else if (numbered && strstr(p.row[0], " addr ")) first_row = 1;
        if (first_row) {
            for (int r = first_row; r <= 6; r++) strcat(seen[k - 1], p.row[r]);
        }
    }
    CHECK(!zreview_render(s, net, n, &p), "rendered past the end");
    int k = 0;
    for (unsigned i = 0; i < s->n_outputs; i++) {
        if (s->outputs[i].kind == ZS_KIND_CHANGE) continue;
        CHECK(strcmp(seen[k], s->outputs[i].address) == 0, "recipient %d address shown as \"%s\"",
              k + 1, seen[k]);
        k++;
    }
}

int main(void)
{
    printf("== test_zcash_review\n");
    char a[32];

    ctx = "amount";
    zreview_amount(0, ZS_NET_TEST, a, sizeof a);            CHECK(!strcmp(a, "0 TAZ"), "%s", a);
    zreview_amount(10000, ZS_NET_TEST, a, sizeof a);        CHECK(!strcmp(a, "0.0001 TAZ"), "%s", a);
    zreview_amount(100000000, ZS_NET_MAIN, a, sizeof a);    CHECK(!strcmp(a, "1 ZEC"), "%s", a);
    zreview_amount(123456789, ZS_NET_MAIN, a, sizeof a);    CHECK(!strcmp(a, "1.23456789 ZEC"), "%s", a);
    zreview_amount(2100000000000000ull, ZS_NET_MAIN, a, sizeof a);
    CHECK(!strcmp(a, "21000000 ZEC"), "%s", a);

    printf("== the common send: one recipient with a memo, change, fee\n");
    ctx = "send";
    static zs_summary s;
    memset(&s, 0, sizeof s);
    s.branch_id = 0x37a5165b;
    s.tx_version = 6;
    s.n_spends = 1;
    s.fee = 10000;
    s.total_in = 1000000;
    add(&s, ZS_POOL_IRONWOOD, ZS_KIND_CHANGE, 590000, "");
    set_memo(add(&s, ZS_POOL_IRONWOOD, ZS_KIND_EXTERNAL, 400000, UA), "LeekWallet Zcash test", 21);
    dump(&s, ZS_NET_TEST);
    CHECK(zreview_page_count(&s, ZS_NET_TEST) == 6, "%d pages", zreview_page_count(&s, ZS_NET_TEST));

    static const char *const P[6][ZREVIEW_ROWS] = {
        { "Send 1 of 1       1/6", "0.004 TAZ", "Ironwood (shielded)",
          "utest1lpcp9q0703pkz3g", "8cqn7gnedx35pfcps0uwm", "4a84cwz8sxpsm8yrgez2g",
          "0ncw9tf5a96fpazsftx8d", "REJECT   ^v      NEXT" },
        { "Send 1 addr 2/2   2/6", "qaqqaq0qdcwlmzsvds9uc", "4r9z3", "", "", "", "",
          "REJECT   ^v      NEXT" },
        { "Send 1 memo 1/1   3/6", "LeekWallet Zcash test", "", "", "", "", "",
          "REJECT   ^v      NEXT" },
        { "Change            4/6", "0.0059 TAZ", "Ironwood (shielded)", "Back to this wallet",
          "", "", "", "REJECT   ^v      NEXT" },
        { "Fee & network     5/6", "Fee 0.0001 TAZ", "Network: Testnet", "Upgrade: NU6.3",
          "Spends: 1", "", "", "REJECT   ^v      NEXT" },
        { "Sign?             6/6", "", "Send 0.004 TAZ", "+fee 0.0001 TAZ", "to 1 recipient",
          "", "", "REJECT           SIGN" },
    };
    for (int i = 0; i < 6; i++) {
        ZReviewPage p;
        zreview_render(&s, ZS_NET_TEST, i, &p);
        CHECK(page_is(&p, P[i]), "page %d differs", i + 1);
    }
    properties(&s, ZS_NET_TEST);

    printf("== transparent recipient: public, twice\n");
    ctx = "transparent";
    memset(&s, 0, sizeof s);
    s.branch_id = 0x77190ad9;
    s.fee = 15000;
    s.warnings = ZS_WARN_TRANSPARENT_OUT;
    add(&s, ZS_POOL_TRANSPARENT, ZS_KIND_EXTERNAL, 300000, "tmFkhJaNXuoMeKKBHp8EE9oiFW4uXKAPWnH");
    add(&s, ZS_POOL_IRONWOOD, ZS_KIND_CHANGE, 685000, "");
    ZReviewPage p;
    zreview_render(&s, ZS_NET_TEST, 0, &p);
    CHECK(strstr(p.row[2], "TRANSPARENT") != NULL, "recipient page does not warn");
    zreview_render(&s, ZS_NET_TEST, zreview_page_count(&s, ZS_NET_TEST) - 2, &p);
    CHECK(strstr(p.row[6], "public") != NULL, "fee page does not warn");
    CHECK(strstr(p.row[3], "NU7") != NULL, "upgrade name");
    properties(&s, ZS_NET_TEST);

    printf("== many recipients, long hostile memo, mainnet\n");
    ctx = "many";
    memset(&s, 0, sizeof s);
    s.branch_id = 0x37a5165b;
    s.fee = 30000;
    char memo[512];
    for (size_t i = 0; i < sizeof memo; i++) memo[i] = (char)('a' + i % 26);
    memo[10] = '\n';
    memo[11] = 0x1b;    /* ESC */
    memo[12] = (char)0xff;
    char longaddr[ZS_UA_MAX];
    memset(longaddr, 'x', sizeof longaddr - 1);
    longaddr[sizeof longaddr - 1] = '\0';
    for (int i = 0; i < 6; i++) {
        zs_output *o = add(&s, ZS_POOL_ORCHARD, i == 5 ? ZS_KIND_SELF : ZS_KIND_EXTERNAL,
                           1000u * (unsigned)(i + 1), i == 2 ? longaddr : UA);
        if (i == 1) set_memo(o, memo, sizeof memo);
        if (i == 3) { o->memo_kind = ZS_MEMO_BINARY; o->memo_len = 512; }
        if (i == 4) o->memo_kind = ZS_MEMO_UNRECOVERED;
    }
    add(&s, ZS_POOL_ORCHARD, ZS_KIND_CHANGE, 5, "");
    properties(&s, ZS_NET_MAIN);
    zreview_render(&s, ZS_NET_MAIN, zreview_page_count(&s, ZS_NET_MAIN) - 1, &p);
    CHECK(strcmp(p.row[2], "Send 0.00015 ZEC") == 0, "final total \"%s\" (self excluded)", p.row[2]);
    CHECK(strcmp(p.row[4], "to 5 recipients") == 0, "\"%s\"", p.row[4]);
    /* The memo shows in full: 512 chars over ceil(512/126) = 5 pages. */
    int memo_pages = 0;
    for (int i = 0; i < zreview_page_count(&s, ZS_NET_MAIN); i++) {
        zreview_render(&s, ZS_NET_MAIN, i, &p);
        if (strncmp(p.row[0], "Send 2 memo", 11) == 0) memo_pages++;
    }
    CHECK(memo_pages == 5, "%d memo pages", memo_pages);

    if (failures) {
        printf("\nFAILED (%d failures)\n", failures);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
