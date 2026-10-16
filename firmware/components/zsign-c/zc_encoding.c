/* LeekWallet Zcash, C signer: ZIP 316 unified containers, transparent
 * addresses and bech32m, written for the signer rather than taken from the
 * vendored library (whose UA decoder has a fixed 512-symbol scratch buffer and
 * a 256-character bech32 limit that a UFVK exceeds).
 *
 * The decoder follows zcash_address 0.14's rules for revision 0 ("u",
 * "utest"): bech32m checksum, lower case only, F4Jumble length bounds,
 * the 16-byte HRP padding, canonical CompactSize typecodes and lengths,
 * strictly ascending typecodes, known receivers at their exact lengths, no
 * P2PKH together with P2SH, MUST-understand metadata refused, and at least one
 * shielded item.
 */
#include "zc_encoding.h"

#include <string.h>

#include "memzero.h"
#include "orchard.h"   /* f4jumble / f4jumble_inv */
#include "sha2.h"

static const char CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
#define BECH32M_CONST 0x2bc830a3u

static uint32_t polymod_step(uint32_t pre)
{
    uint8_t b = (uint8_t)(pre >> 25);
    return ((pre & 0x1FFFFFF) << 5) ^ (-((b >> 0) & 1) & 0x3b6a57b2UL) ^
           (-((b >> 1) & 1) & 0x26508e6dUL) ^ (-((b >> 2) & 1) & 0x1ea119faUL) ^
           (-((b >> 3) & 1) & 0x3d4233ddUL) ^ (-((b >> 4) & 1) & 0x2a1462b3UL);
}

static int charset_rev(char c)
{
    const char *p = c ? strchr(CHARSET, c) : NULL;
    return p ? (int)(p - CHARSET) : -1;
}

/* bech32m(hrp, 8-bit data) with no length limit. Returns length or 0. */
static size_t bech32m_encode(char *out, size_t cap, const char *hrp,
                             const uint8_t *data, size_t len)
{
    size_t hl = strlen(hrp);
    size_t n5 = (len * 8 + 4) / 5;
    if (hl + 1 + n5 + 6 + 1 > cap) return 0;
    uint32_t chk = 1;
    for (size_t i = 0; i < hl; i++) chk = polymod_step(chk) ^ ((uint8_t)hrp[i] >> 5);
    chk = polymod_step(chk);
    for (size_t i = 0; i < hl; i++) chk = polymod_step(chk) ^ ((uint8_t)hrp[i] & 0x1f);
    memcpy(out, hrp, hl);
    size_t o = hl;
    out[o++] = '1';
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            uint8_t v = (acc >> (bits - 5)) & 31;
            bits -= 5;
            chk = polymod_step(chk) ^ v;
            out[o++] = CHARSET[v];
        }
    }
    if (bits) {
        uint8_t v = (acc << (5 - bits)) & 31;
        chk = polymod_step(chk) ^ v;
        out[o++] = CHARSET[v];
    }
    for (int i = 0; i < 6; i++) chk = polymod_step(chk);
    chk ^= BECH32M_CONST;
    for (int i = 0; i < 6; i++) out[o++] = CHARSET[(chk >> ((5 - i) * 5)) & 31];
    out[o] = 0;
    return o;
}

/* Lower-case bech32m decode into 8-bit data (trailing partial bits are
 * dropped, as bech32's byte iterator does). Returns 0 and fills hrp/data. */
static int bech32m_decode(const char *s, size_t n, char hrp[17], uint8_t *data,
                          size_t cap, size_t *dlen)
{
    size_t sep = n;
    for (size_t i = n; i > 0; i--) if (s[i - 1] == '1') { sep = i - 1; break; }
    if (sep == n || sep == 0 || sep > 16 || n - sep - 1 < 6) return -1;
    uint32_t chk = 1;
    for (size_t i = 0; i < sep; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 33 || c > 126 || (c >= 'A' && c <= 'Z')) return -1;
        chk = polymod_step(chk) ^ (c >> 5);
        hrp[i] = (char)c;
    }
    hrp[sep] = 0;
    chk = polymod_step(chk);
    for (size_t i = 0; i < sep; i++) chk = polymod_step(chk) ^ ((uint8_t)s[i] & 0x1f);
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    size_t ndata = n - sep - 1 - 6;
    for (size_t i = sep + 1; i < n; i++) {
        int v = charset_rev(s[i]);
        if (v < 0) return -1;
        chk = polymod_step(chk) ^ (uint32_t)v;
        if (i - sep - 1 < ndata) {
            acc = (acc << 5) | (uint32_t)v;
            bits += 5;
            if (bits >= 8) {
                if (o >= cap) return -1;
                data[o++] = (uint8_t)(acc >> (bits - 8));
                bits -= 8;
            }
        }
    }
    if (chk != BECH32M_CONST) return -1;
    *dlen = o;
    return 0;
}

static const char *ua_hrp(uint8_t net) { return net == 0 ? "u" : "utest"; }
static const char *ufvk_hrp(uint8_t net) { return net == 0 ? "uview" : "uviewtest"; }

/* Assemble items || padding, F4Jumble, bech32m. Items must be pre-sorted. */
static size_t encode_container(char *out, size_t cap, const char *hrp,
                               const uint8_t *raw, size_t raw_len)
{
    static uint8_t buf[320];
    size_t hl = strlen(hrp);
    if (raw_len + 16 > sizeof buf || hl > 16) return 0;
    memcpy(buf, raw, raw_len);
    memset(buf + raw_len, 0, 16);
    memcpy(buf + raw_len, hrp, hl);
    f4jumble(buf, raw_len + 16);
    size_t r = bech32m_encode(out, cap, hrp, buf, raw_len + 16);
    memzero(buf, sizeof buf);
    return r;
}

size_t zc_encode_ufvk(char *out, size_t cap, uint8_t net, const uint8_t fvk[96],
                      const uint8_t *t_account)
{
    uint8_t raw[2 + 65 + 2 + 96];
    size_t n = 0;
    if (t_account) {             /* P2PKH (0x00) sorts before Orchard (0x03) */
        raw[n++] = 0x00;
        raw[n++] = 65;
        memcpy(raw + n, t_account, 65);
        n += 65;
    }
    raw[n++] = 0x03;
    raw[n++] = 96;
    memcpy(raw + n, fvk, 96);
    n += 96;
    return encode_container(out, cap, ufvk_hrp(net), raw, n);
}

size_t zc_encode_ua(char *out, size_t cap, uint8_t net, const uint8_t orchard[43],
                    const uint8_t *p2pkh)
{
    uint8_t raw[2 + 20 + 2 + 43];
    size_t n = 0;
    if (p2pkh) {
        raw[n++] = 0x00;
        raw[n++] = 20;
        memcpy(raw + n, p2pkh, 20);
        n += 20;
    }
    raw[n++] = 0x03;
    raw[n++] = 43;
    memcpy(raw + n, orchard, 43);
    n += 43;
    return encode_container(out, cap, ua_hrp(net), raw, n);
}

/* zcash_encoding::CompactSize::read: canonical, <= 0x02000000. */
static int compact_size(const uint8_t *p, size_t n, size_t *i, uint64_t *v)
{
    if (*i >= n) return -1;
    uint8_t f = p[(*i)++];
    uint64_t x;
    size_t k;
    if (f < 0xFD) { *v = f; return 0; }
    k = f == 0xFD ? 2 : f == 0xFE ? 4 : 8;
    if (n - *i < k) return -1;
    x = 0;
    for (size_t j = 0; j < k; j++) x |= (uint64_t)p[*i + j] << (8 * j);
    *i += k;
    if ((k == 2 && x < 0xFD) || (k == 4 && x < 0x10000) || (k == 8 && x < 0x100000000ull))
        return -1;
    if (x > 0x02000000u) return -1;
    *v = x;
    return 0;
}

/* The longest user_address we decode. zcash_address accepts up to the
 * F4Jumble bound (4 MiB); a real UA is a few hundred characters, and one past
 * this limit is refused (ZS_E_USER_ADDRESS), never accepted. */
#define UA_MAX_CHARS 2048

/* Known HRPs for addresses: revision 0 ("u"), revision 2 shielded-only
 * ("zu") and transparent-including ("tu"). Regtest decodes in zcash_address
 * but never matches mainnet or testnet, so it is simply unknown here. */
static int ua_hrp_network(const char *hrp, uint8_t *net, int *r2, int *r2_ti)
{
    static const struct { const char *hrp; uint8_t net, r2, ti; } T[] = {
        { "u", 0, 0, 0 }, { "utest", 1, 0, 0 },
        { "zu", 0, 1, 0 }, { "zutest", 1, 1, 0 },
        { "tu", 0, 1, 1 }, { "tutest", 1, 1, 1 },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (strcmp(hrp, T[i].hrp) == 0) {
            *net = T[i].net;
            *r2 = T[i].r2;
            *r2_ti = T[i].ti;
            return 1;
        }
    return 0;
}

int zc_ua_parse(const char *ua, size_t len, uint8_t net, zc_ua_items *out)
{
    static uint8_t buf[(UA_MAX_CHARS * 5) / 8 + 1];
    char hrp[17];
    size_t n;
    uint8_t ua_net;
    int r2, r2_ti;
    memset(out, 0, sizeof *out);
    if (len > UA_MAX_CHARS || memchr(ua, 0, len)) return 0;
    if (bech32m_decode(ua, len, hrp, buf, sizeof buf, &n) != 0) return 0;
    if (!ua_hrp_network(hrp, &ua_net, &r2, &r2_ti) || ua_net != net) return 0;
    if (n < 48 || n > 4194368) return 0;           /* F4Jumble VALID_LENGTH */
    f4jumble_inv(buf, n);
    uint8_t pad[16] = {0};
    memcpy(pad, hrp, strlen(hrp));
    if (memcmp(buf + n - 16, pad, 16) != 0) return 0;

    size_t end = n - 16, i = 0;
    int64_t prev = -1;
    int has_data = 0, has_shielded = 0, has_transparent = 0;
    while (i < end) {
        uint64_t tc, ln;
        if (compact_size(buf, end, &i, &tc) != 0) return 0;
        if (compact_size(buf, end, &i, &ln) != 0) return 0;
        if (ln > end - i) return 0;
        if ((int64_t)tc <= prev) return 0;         /* order and duplicates */
        if (tc >= 0xC0 && tc <= 0xFC) {           /* metadata */
            if (tc >= 0xE0) {                      /* MUST-understand */
                if (!r2) return 0;                 /* none defined in revision 0 */
                if (tc == 0xE0 && ln != 4) return 0;   /* expiry height */
                if (tc == 0xE1 && ln != 8) return 0;   /* expiry time */
                if (tc > 0xE1) return 0;
                out->other = 1;                    /* expiring: not checked here */
            }
        } else {                                   /* data item */
            if ((tc == 0 || tc == 1) && ln != 20) return 0;
            if ((tc == 2 || tc == 3) && ln != 43) return 0;
            if (tc == 1 && prev == 0) return 0;    /* both P2PKH and P2SH */
            has_data = 1;
            if (tc <= 1) has_transparent = 1;
            else has_shielded = 1;
            if (tc == 3) {
                out->has_orchard = 1;
                memcpy(out->orchard, buf + i, 43);
            } else if (tc == 0) {
                out->has_p2pkh = 1;
                memcpy(out->p2pkh, buf + i, 20);
            } else {
                out->other = 1;                    /* P2SH, Sapling, unknown */
            }
        }
        prev = (int64_t)tc;
        i += (size_t)ln;
    }
    if (!has_data) return 0;
    if (!r2 && !has_shielded) return 0;           /* revision 0: one shielded item */
    if (r2 && !r2_ti && has_transparent) return 0; /* "zu": no transparent receiver */
    return 1;
}

int zc_ua_contains_orchard(const char *ua, size_t len, uint8_t net, const uint8_t raw43[43])
{
    zc_ua_items it;
    return zc_ua_parse(ua, len, net, &it) && it.has_orchard &&
           memcmp(it.orchard, raw43, 43) == 0;
}

/* ---- transparent addresses ---------------------------------------------- */

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static size_t base58check(char *out, size_t cap, const uint8_t *payload, size_t n)
{
    uint8_t buf[32], h[32];
    if (n + 4 > sizeof buf) return 0;
    memcpy(buf, payload, n);
    sha256_Raw(buf, n, h);
    sha256_Raw(h, 32, h);
    memcpy(buf + n, h, 4);
    n += 4;
    uint8_t digits[48];
    size_t nd = 0, zeros = 0;
    while (zeros < n && buf[zeros] == 0) zeros++;
    for (size_t i = zeros; i < n; i++) {
        uint32_t carry = buf[i];
        for (size_t j = 0; j < nd; j++) {
            carry += (uint32_t)digits[j] << 8;
            digits[j] = carry % 58;
            carry /= 58;
        }
        while (carry) {
            digits[nd++] = carry % 58;
            carry /= 58;
        }
    }
    if (zeros + nd + 1 > cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < zeros; i++) out[o++] = '1';
    for (size_t i = nd; i > 0; i--) out[o++] = B58[digits[i - 1]];
    out[o] = 0;
    return o;
}

size_t zc_t_address(char *out, size_t cap, uint8_t net, const uint8_t *script, size_t n)
{
    uint8_t payload[22];
    if (n == 25 && script[0] == 0x76 && script[1] == 0xa9 && script[2] == 0x14 &&
        script[23] == 0x88 && script[24] == 0xac) {
        payload[0] = net == 0 ? 0x1C : 0x1D;
        payload[1] = net == 0 ? 0xB8 : 0x25;
        memcpy(payload + 2, script + 3, 20);
    } else if (n == 23 && script[0] == 0xa9 && script[1] == 0x14 && script[22] == 0x87) {
        payload[0] = 0x1C;
        payload[1] = net == 0 ? 0xBD : 0xBA;
        memcpy(payload + 2, script + 2, 20);
    } else {
        return 0;
    }
    return base58check(out, cap, payload, sizeof payload);
}

/* zcash_script 0.6 script::PubKey::parse: every opcode decodes, pushes fit
 * in the script and are at most 520 bytes, and no opcode is disabled or bad
 * (reserved, OP_VER*, or unassigned). */
static int opcode_ok(uint8_t op)
{
    switch (op) {
    case 0x7e: case 0x7f: case 0x80: case 0x81: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x8d: case 0x8e: case 0x95: case 0x96: case 0x97: case 0x98:
    case 0x99: case 0xab:                          /* disabled */
    case 0x50: case 0x62: case 0x65: case 0x66: case 0x89: case 0x8a:  /* bad */
        return 0;
    default:
        return op < 0xba;                          /* 0xba.. are unassigned */
    }
}

int zc_script_parses(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t op = s[i++];
        size_t len = 0;
        if (op >= 0x01 && op <= 0x4b) {
            len = op;
        } else if (op >= 0x4c && op <= 0x4e) {
            size_t k = op == 0x4c ? 1 : op == 0x4d ? 2 : 4;
            if (n - i < k) return 0;
            for (size_t j = k; j > 0; j--) len = (len << 8) | s[i + j - 1];
            i += k;
        } else {
            if (!opcode_ok(op)) return 0;
            continue;
        }
        if (n - i < len || len > 520) return 0;
        i += len;
    }
    return 1;
}
