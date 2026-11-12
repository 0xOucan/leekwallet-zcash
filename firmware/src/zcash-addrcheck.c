/**
 * Check address. See zcash-addrcheck.h.
 */
#include "zcash-addrcheck.h"

#include <stdio.h>
#include <string.h>

#include "base58.h"
#include "hasher.h"
#include "memzero.h"
#include "segwit_addr.h"
#include "zcash-worker.h"
#include "zsign_c.h"

/* Base58Check version prefixes (zcash_protocol constants). */
static const struct {
    uint8_t v0, v1;
    ZacKind kind;
    uint8_t net;
} T_PREFIXES[] = {
    { 0x1C, 0xB8, ZAC_KIND_P2PKH, ZCASH_NET_MAIN },   /* t1 */
    { 0x1C, 0xBD, ZAC_KIND_P2SH,  ZCASH_NET_MAIN },   /* t3 */
    { 0x1D, 0x25, ZAC_KIND_P2PKH, ZCASH_NET_TEST },   /* tm */
    { 0x1C, 0xBA, ZAC_KIND_P2SH,  ZCASH_NET_TEST },   /* t2 */
};

const char *zac_net_name(uint8_t net)
{
    return net == ZCASH_NET_MAIN ? "Mainnet" : "Testnet";
}

static bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static char lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static bool starts_ci(const char *s, size_t n, const char *prefix)
{
    const size_t k = strlen(prefix);
    if (n < k) return false;
    for (size_t i = 0; i < k; i++) {
        if (lower(s[i]) != prefix[i]) return false;
    }
    return true;
}

/*
 * The address in a QR's text: the text itself, or a ZIP 321 URI's - the
 * path (zcash:<addr>?...), else the query's one unnumbered "address=". A URI
 * of several payments (address.N) is refused, as the SDK's
 * address_check_payload() refuses it: which one would the user be checking?
 * Copied into out (cap bytes) only if it is all letters and digits, which
 * every Zcash encoding is.
 */
static bool extract(const char *qr, size_t len, char *out, size_t cap)
{
    while (len > 0 && is_space(qr[0])) { qr++; len--; }
    while (len > 0 && is_space(qr[len - 1])) len--;
    const char *a = qr;
    size_t n = len;
    if (starts_ci(qr, len, "zcash:")) {
        const char *rest = qr + 6;
        const size_t rn = len - 6;
        size_t path = 0;
        while (path < rn && rest[path] != '?') path++;
        if (path > 0) {
            a = rest;
            n = path;
        } else {
            /* Query parameters, '&'-separated: exactly one "address". */
            const char *best = NULL;
            size_t best_n = 0;
            size_t i = path + 1;
            while (i < rn) {
                size_t j = i;
                while (j < rn && rest[j] != '&') j++;
                const char *kv = rest + i;
                const size_t kvn = j - i;
                size_t eq = 0;
                while (eq < kvn && kv[eq] != '=') eq++;
                const bool plain = eq == 7 && memcmp(kv, "address", 7) == 0;
                const bool numbered = eq > 8 && memcmp(kv, "address.", 8) == 0;
                if (numbered || (plain && best != NULL)) return false;
                if (plain) {
                    best = eq < kvn ? kv + eq + 1 : kv + kvn;
                    best_n = eq < kvn ? kvn - eq - 1 : 0;
                }
                i = j + 1;
            }
            if (best == NULL) return false;
            a = best;
            n = best_n;
        }
    }
    if (n == 0 || n >= cap) return false;
    for (size_t i = 0; i < n; i++) {
        const char ch = a[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    memcpy(out, a, n);
    out[n] = '\0';
    return true;
}

/* Bech32(m) is case-insensitive but never mixed: an all-upper string is
 * folded to lower case (what zc_ua_parse and the panel expect), a mixed one
 * is left for the decoder to refuse. */
static void fold_bech32(char *s)
{
    bool upper = false, low = false;
    for (char *p = s; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') upper = true;
        if (*p >= 'a' && *p <= 'z') low = true;
    }
    if (upper && !low) {
        for (char *p = s; *p; p++) *p = lower(*p);
    }
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* 5-bit groups to bytes, no padding allowed beyond the last whole byte. */
static bool bits5_to_8(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_n)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 5) | in[i];
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (k >= cap) return false;
            out[k++] = (uint8_t)(acc >> bits);
        }
    }
    if (bits >= 5 || (acc & ((1u << bits) - 1u)) != 0) return false;
    *out_n = k;
    return true;
}

/* A Bech32 (Sapling) or Bech32m (TEX) string with HRP `hrp` and a payload of
 * exactly `want` bytes, into out. */
static bool bech32_payload(const char *s, const char *hrp, bech32_encoding enc, uint8_t *out,
                           size_t want)
{
    char got_hrp[BECH32_MAX_HRP_LEN + 1];
    uint8_t data[128];
    size_t data_n = 0;
    if (strlen(s) > sizeof data + 8) return false;
    if (bech32_decode(got_hrp, data, &data_n, s) != enc) return false;
    if (strcmp(got_hrp, hrp) != 0) return false;
    uint8_t bytes[64];
    size_t n = 0;
    if (!bits5_to_8(data, data_n, bytes, sizeof bytes, &n) || n != want) return false;
    memcpy(out, bytes, want);
    return true;
}

/* zc_ua_receivers on the Zcash worker: F4Jumble's buffers and BLAKE2b
 * state are more than the 8 KB UI task should carry. */
typedef struct {
    const char *ua;
    uint8_t     net;
    int         orchard, p2pkh, sapling, other;
    int32_t     rc;
} UaShapeJob;

static void ua_shape_job(void *p)
{
    UaShapeJob *j = p;
    j->rc = zc_ua_receivers(j->ua, j->net, &j->orchard, &j->p2pkh, &j->sapling, &j->other);
}

static ZacVerdict finish(ZacCheck *c, ZacVerdict v, ZacWhy why)
{
    c->verdict = v;
    c->why = why;
    return v;
}

ZacVerdict zac_parse(const char *qr, size_t len, uint8_t board_net, ZacCheck *c)
{
    memset(c, 0, sizeof *c);
    if (qr == NULL || !extract(qr, len, c->addr, sizeof c->addr)) {
        c->addr[0] = '\0';
        return finish(c, ZAC_MALFORMED, ZAC_WHY_NONE);
    }
    char *s = c->addr;
    const size_t n = strlen(s);
    bool ok = false;

    if (s[0] == 't' && n >= 30 && n <= 40 && !starts(s, "tex")) {
        /* Base58Check, two version bytes and a 20-byte hash. */
        uint8_t raw[22];
        if (base58_decode_check(s, HASHER_SHA2D, raw, sizeof raw) == (int)sizeof raw) {
            for (size_t i = 0; i < sizeof T_PREFIXES / sizeof T_PREFIXES[0]; i++) {
                if (raw[0] == T_PREFIXES[i].v0 && raw[1] == T_PREFIXES[i].v1) {
                    c->kind = T_PREFIXES[i].kind;
                    c->net = T_PREFIXES[i].net;
                    memcpy(c->p2pkh, raw + 2, 20);
                    c->receivers = ZAC_R_TRANSPARENT;
                    ok = true;
                }
            }
        }
        memzero(raw, sizeof raw);
    } else {
        fold_bech32(s);
        uint8_t raw[43];
        if (starts(s, "u1") || starts(s, "utest1")) {
            c->kind = ZAC_KIND_UA;
            c->net = s[1] == '1' ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
            UaShapeJob j = { s, c->net, 0, 0, 0, 0, ZS_E_PARSE };
            if (zcash_worker_run(ua_shape_job, &j) && j.rc == ZS_OK) {
                ok = true;
                c->receivers = (uint8_t)((j.orchard ? ZAC_R_ORCHARD : 0) |
                                         (j.p2pkh ? ZAC_R_TRANSPARENT : 0) |
                                         (j.sapling ? ZAC_R_SAPLING : 0) |
                                         (j.other ? ZAC_R_OTHER : 0));
            }
        } else if (starts(s, "ztestsapling1") || starts(s, "zs1")) {
            c->kind = ZAC_KIND_SAPLING;
            c->net = s[1] == 's' ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
            ok = bech32_payload(s, c->net == ZCASH_NET_MAIN ? "zs" : "ztestsapling",
                                BECH32_ENCODING_BECH32, raw, 43);
            c->receivers = ZAC_R_SAPLING;
        } else if (starts(s, "textest1") || starts(s, "tex1")) {
            c->kind = ZAC_KIND_TEX;
            c->net = s[3] == '1' ? ZCASH_NET_MAIN : ZCASH_NET_TEST;
            ok = bech32_payload(s, c->net == ZCASH_NET_MAIN ? "tex" : "textest",
                                BECH32_ENCODING_BECH32M, c->p2pkh, 20);
            c->receivers = ZAC_R_TRANSPARENT;
        }
        memzero(raw, sizeof raw);
    }

    if (!ok) {
        memzero(c, sizeof *c);
        return finish(c, ZAC_MALFORMED, ZAC_WHY_NONE);
    }
    if (c->net != board_net) return finish(c, ZAC_WRONG_NET, ZAC_WHY_NONE);
    switch (c->kind) {
    case ZAC_KIND_SAPLING: return finish(c, ZAC_CANNOT_CHECK, ZAC_WHY_SAPLING);
    case ZAC_KIND_P2SH:    return finish(c, ZAC_NOT_OURS, ZAC_WHY_P2SH);
    case ZAC_KIND_UA:
        /* Decided before any key is touched: a receiver this wallet never
         * makes, or no Orchard receiver to recover an index from (Sapling,
         * or Sapling + transparent: the index is in the Sapling half). */
        if (c->receivers & ZAC_R_OTHER) return finish(c, ZAC_NOT_OURS, ZAC_WHY_RECEIVER);
        if (!(c->receivers & ZAC_R_ORCHARD)) return finish(c, ZAC_CANNOT_CHECK, ZAC_WHY_NO_ORCHARD);
        break;
    default:
        break;
    }
    return finish(c, ZAC_PENDING, ZAC_WHY_NONE);
}

bool zac_is_transparent(const ZacCheck *c)
{
    return c->kind == ZAC_KIND_P2PKH || c->kind == ZAC_KIND_TEX;
}

/* zc_ua_owned_but_sapling on the Zcash worker (Pallas arithmetic). */
typedef struct {
    const char    *ua;
    uint8_t        net;
    const uint8_t *fvk;
    uint8_t        index[11];
    uint8_t        p2pkh[20];
    int            has_p2pkh, has_sapling;
    int32_t        rc;
} UaOwnedJob;

static void ua_owned_job(void *p)
{
    UaOwnedJob *j = p;
    j->rc = zc_ua_owned_but_sapling(j->ua, j->net, j->fvk, j->index, j->p2pkh, &j->has_p2pkh,
                                    &j->has_sapling);
}

ZacVerdict zac_check_ua(ZacCheck *c, const ZcashAccount *a)
{
    if (c->verdict != ZAC_PENDING || c->kind != ZAC_KIND_UA || a->network != c->net) {
        return c->verdict;
    }
    static UaOwnedJob j;
    memset(&j, 0, sizeof j);
    j.ua = c->addr;
    j.net = c->net;
    j.fvk = a->keys.orchard_fvk;
    j.rc = ZS_E_INTERNAL;
    ZacVerdict v;
    if (!zcash_worker_run(ua_owned_job, &j)) {
        v = finish(c, ZAC_MALFORMED, ZAC_WHY_NONE);
    } else if (j.rc == ZS_E_USER_ADDRESS) {
        v = ZAC_NOT_OURS;   /* this account's verdict; still pending overall */
    } else if (j.rc == ZS_E_UNSUPPORTED_BUNDLE) {
        v = finish(c, ZAC_NOT_OURS, ZAC_WHY_RECEIVER);
    } else if (j.rc != ZS_OK) {
        v = finish(c, ZAC_MALFORMED, ZAC_WHY_NONE);
    } else {
        /* Our Orchard receiver. ZIP 316: a P2PKH receiver with it is the
         * external transparent key at the same index, so below 2^31 only. */
        bool ok = true;
        if (j.has_p2pkh) {
            for (int i = 4; i < 11; i++) ok = ok && j.index[i] == 0;
            const uint32_t idx = (uint32_t)j.index[0] | (uint32_t)j.index[1] << 8 |
                                 (uint32_t)j.index[2] << 16 | (uint32_t)j.index[3] << 24;
            uint32_t found = 0;
            ok = ok && idx < 0x80000000u && zcash_t_find(a, j.p2pkh, idx, 1, &found) &&
                 found == idx;
        }
        if (ok) {
            c->account = a->account;
            memcpy(c->index_le, j.index, sizeof j.index);
            v = finish(c, ZAC_OURS, ZAC_WHY_NONE);
        } else {
            v = ZAC_NOT_OURS;   /* a transparent receiver that is not ours here */
        }
    }
    memzero(&j, sizeof j);
    return v;
}

bool zac_t_step(ZacCheck *c, const ZcashAccount *a)
{
    if (c->verdict != ZAC_PENDING || !zac_is_transparent(c) || a->network != c->net) {
        return true;
    }
    c->account = a->account;
    if (c->t_next >= ZAC_T_SEARCH) {
        finish(c, ZAC_NOT_OURS, ZAC_WHY_T_RANGE);
        return true;
    }
    uint32_t count = ZAC_T_SEARCH - c->t_next;
    if (count > ZAC_T_CHUNK) count = ZAC_T_CHUNK;
    uint32_t found = 0;
    if (zcash_t_find(a, c->p2pkh, c->t_next, count, &found)) {
        memset(c->index_le, 0, sizeof c->index_le);
        for (int k = 0; k < 4; k++) c->index_le[k] = (uint8_t)(found >> (8 * k));
        finish(c, ZAC_OURS, ZAC_WHY_NONE);
        return true;
    }
    c->t_next += count;
    if (c->t_next >= ZAC_T_SEARCH) {
        finish(c, ZAC_NOT_OURS, ZAC_WHY_T_RANGE);
        return true;
    }
    return false;
}

/* ---- Pages ------------------------------------------------------------- */

/* Address rows: 1-6, in groups of four, four groups a row ("abcd efgh ..."). */
#define ADDR_ROWS        6
#define GROUP            4
#define GROUPS_PER_ROW   4
#define CHARS_PER_ROW    (GROUP * GROUPS_PER_ROW)
#define CHARS_PER_PAGE   (CHARS_PER_ROW * ADDR_ROWS)

static int addr_pages(const ZacCheck *c)
{
    const int n = (int)strlen(c->addr);
    return n == 0 ? 0 : (n + CHARS_PER_PAGE - 1) / CHARS_PER_PAGE;
}

int zac_page_count(const ZacCheck *c)
{
    return 1 + addr_pages(c);
}

static void put(ZacPage *p, int r, ZacRowRole role, const char *s)
{
    snprintf(p->row[r], sizeof p->row[r], "%s", s);
    p->role[r] = role;
}

static const char *kind_name(const ZacCheck *c)
{
    switch (c->kind) {
    case ZAC_KIND_UA:
        if ((c->receivers & (ZAC_R_ORCHARD | ZAC_R_TRANSPARENT)) ==
            (ZAC_R_ORCHARD | ZAC_R_TRANSPARENT)) {
            return "Orchard + transparent";
        }
        if ((c->receivers & ZAC_R_SAPLING) && !(c->receivers & ZAC_R_ORCHARD)) {
            return "Unified, Sapling";
        }
        return (c->receivers & ZAC_R_ORCHARD) ? "Orchard" : "Unified";
    case ZAC_KIND_SAPLING: return "Sapling";
    case ZAC_KIND_P2PKH:   return "Transparent";
    case ZAC_KIND_P2SH:    return "Transparent P2SH";
    case ZAC_KIND_TEX:     return "TEX (transparent)";
    }
    return "";
}

/* The 88-bit index, in decimal when it fits 64 bits (wallets hand out small
 * ones), else in hex. */
static void index_text(const uint8_t le[11], char *out, size_t cap)
{
    if (le[8] == 0 && le[9] == 0 && le[10] == 0) {
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | le[k];
        snprintf(out, cap, "%llu", (unsigned long long)v);
        return;
    }
    char hex[23];
    int at = 0;
    bool lead = true;
    for (int k = 10; k >= 0; k--) {
        if (lead && le[k] == 0) continue;
        at += snprintf(hex + at, sizeof hex - (size_t)at, lead ? "%x" : "%02x", le[k]);
        lead = false;
    }
    snprintf(out, cap, "0x%s", hex);
}

static void verdict_page(const ZacCheck *c, ZacPage *p)
{
    char line[48];
    put(p, 0, ZAC_ROW_TEXT, "Check address");
    switch (c->verdict) {
    case ZAC_OURS: {
        /* Row 1: what was NOT checked, if anything, above all else. */
        if (c->receivers & ZAC_R_SAPLING) {
            put(p, 1, ZAC_ROW_WARN, "Sapling NOT checked");
        } else {
            put(p, 1, ZAC_ROW_DIM, zac_net_name(c->net));
        }
        put(p, 2, ZAC_ROW_GOOD, "YOURS");
        snprintf(line, sizeof line, "Account %u", (unsigned)c->account);
        put(p, 3, ZAC_ROW_TEXT, line);
        char idx[32];
        index_text(c->index_le, idx, sizeof idx);
        snprintf(line, sizeof line, "Address #%s", idx);
        int r = 4;
        if (strlen(line) <= ZAC_COLS) {
            put(p, r++, ZAC_ROW_TEXT, line);
        } else {
            put(p, r++, ZAC_ROW_TEXT, "Address index");
            put(p, r++, ZAC_ROW_TEXT, idx);
        }
        /* What was checked, receiver by receiver (a big index has no
           transparent receiver, so this never reaches the footer). */
        if (c->kind == ZAC_KIND_UA) {
            put(p, r++, ZAC_ROW_DIM, "Checked: Orchard");
            if ((c->receivers & ZAC_R_TRANSPARENT) && r < ZAC_ROWS - 1) {
                put(p, r++, ZAC_ROW_DIM, "Checked: transparent");
            }
        } else {
            put(p, r++, ZAC_ROW_DIM, c->kind == ZAC_KIND_TEX ? "Checked: TEX" : "Checked: transparent");
        }
        break;
    }
    case ZAC_CANNOT_CHECK:
        put(p, 2, ZAC_ROW_WARN, "Cannot check");
        if (c->why == ZAC_WHY_NO_ORCHARD) {
            put(p, 3, ZAC_ROW_TEXT, "No Orchard receiver,");
            put(p, 4, ZAC_ROW_TEXT, "and the board has no");
            put(p, 5, ZAC_ROW_TEXT, "Sapling keys");
        } else {
            put(p, 3, ZAC_ROW_TEXT, "Sapling address: the");
            put(p, 4, ZAC_ROW_TEXT, "board has no Sapling");
            put(p, 5, ZAC_ROW_TEXT, "keys. Not verified.");
        }
        break;
    case ZAC_WRONG_NET:
        put(p, 2, ZAC_ROW_WARN, "Wrong network");
        snprintf(line, sizeof line, "%s address,", zac_net_name(c->net));
        put(p, 3, ZAC_ROW_TEXT, line);
        snprintf(line, sizeof line, "board is on %s", c->net == ZCASH_NET_MAIN ? "testnet" : "mainnet");
        put(p, 4, ZAC_ROW_TEXT, line);
        put(p, 5, ZAC_ROW_DIM, "Settings > Network");
        break;
    case ZAC_NOT_OURS:
        put(p, 2, ZAC_ROW_DANGER, "NOT from this wallet");
        switch (c->why) {
        case ZAC_WHY_P2SH:
            put(p, 3, ZAC_ROW_TEXT, "P2SH address; this");
            put(p, 4, ZAC_ROW_TEXT, "wallet makes none");
            break;
        case ZAC_WHY_RECEIVER:
            put(p, 3, ZAC_ROW_TEXT, "Has a receiver this");
            put(p, 4, ZAC_ROW_TEXT, "wallet never makes");
            put(p, 5, ZAC_ROW_DIM, "(P2SH, unknown)");
            break;
        case ZAC_WHY_T_RANGE:
            snprintf(line, sizeof line, "Not account %u's", (unsigned)c->account);
            put(p, 3, ZAC_ROW_TEXT, line);
            snprintf(line, sizeof line, "t-address #0-%u", (unsigned)(ZAC_T_SEARCH - 1));
            put(p, 4, ZAC_ROW_TEXT, line);
            break;
        default:
            put(p, 3, ZAC_ROW_TEXT, "No account on this");
            put(p, 4, ZAC_ROW_TEXT, "seed makes it");
            break;
        }
        break;
    default:
        put(p, 3, ZAC_ROW_TEXT, "Not a Zcash address");
        break;
    }
    if (c->verdict != ZAC_OURS && c->verdict != ZAC_MALFORMED) {
        put(p, 6, ZAC_ROW_DIM, kind_name(c));
    }
}

bool zac_render(const ZacCheck *c, int page, const char *hint, ZacPage *out)
{
    memset(out, 0, sizeof *out);
    if (page < 0 || page >= zac_page_count(c)) return false;
    if (page == 0) {
        verdict_page(c, out);
    } else {
        char line[48];
        const int pages = addr_pages(c);
        snprintf(line, sizeof line, "Address %d/%d", page, pages);
        put(out, 0, ZAC_ROW_DIM, line);
        const char *s = c->addr;
        const int n = (int)strlen(s);
        int at = (page - 1) * CHARS_PER_PAGE;
        for (int r = 0; r < ADDR_ROWS && at < n; r++) {
            char row[ZAC_COLS + 1];
            int k = 0;
            for (int g = 0; g < GROUPS_PER_ROW && at < n; g++) {
                if (g > 0) row[k++] = ' ';
                for (int i = 0; i < GROUP && at < n; i++) row[k++] = s[at++];
            }
            row[k] = '\0';
            put(out, 1 + r, ZAC_ROW_TEXT, row);
        }
    }
    put(out, 7, ZAC_ROW_DIM, hint ? hint : "");
    return true;
}
