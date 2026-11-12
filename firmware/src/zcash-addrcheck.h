/**
 * Check address: is the address a wallet's Receive screen shows one of this
 * device's own? (Main menu -> Check address, on a board with a camera.)
 *
 * Over USB or Bluetooth the companion asks (zShowAddress); in QR-only mode
 * nobody asks, so the user scans the phone's receive QR and the board answers
 * from its own keys. What it reads is what every wallet's Receive screen
 * shows: a unified address (u1... / utest1...), a Sapling address (zs1... /
 * ztestsapling1...), a transparent address (t1, t3 / tm, t2), a TEX address
 * (tex1... / textest1..., ZIP 320), or a ZIP 321 "zcash:" URI with one
 * payment (the path, or an unnumbered "address="; a URI with numbered
 * payments is refused), the forms the SDK's address_check_qr() /
 * address_check_payload() (host/leek-zcash/src/ua.rs) produces and accepts.
 * Bech32 and Bech32m are read in either case, never mixed (BIP 173).
 *
 * What "ours" means here is what this device's accounts actually are: an
 * Orchard key and a BIP 44 transparent key (zcash-account.h), no Sapling.
 *
 *   - A UA: its Orchard receiver must be the account's, its diversifier
 *     index recovered by decrypting the diversifier (FF1 with the account's
 *     dk, zc_ua_owned_but_sapling), and a P2PKH receiver in it must be the
 *     account's external key at that same index. A wallet's Receive UA
 *     usually carries a Sapling receiver too; the device has no Sapling keys
 *     (no Jubjub, no Sapling ZIP 32 anywhere in its signers), so that one is
 *     NOT checked, and the result says so in as many words. A P2SH or
 *     unknown receiver, or expiry metadata, makes it not this wallet's.
 *     Every account the device's own screens can select (0..HD_ACCOUNT_COUNT-1)
 *     is tried, the selected one first.
 *   - A transparent P2PKH or TEX address: the selected account's external
 *     keys m/44'/coin'/account'/0/i for i in 0..ZAC_T_SEARCH-1, searched from
 *     the account's public key alone (zcash_t_find).
 *   - Sapling: decoded (a typo is "not an address"), then "cannot check".
 *     So is a UA with no Orchard receiver.
 *   - P2SH: decoded, then not this wallet's (it makes none).
 *
 * The network comes from the address's own prefix and is compared with the
 * board's (Settings > Network) before any key is touched.
 *
 * The parsing and the result pages are pure and host-tested
 * (sim/test_zcash_addrcheck.c, with the real derivation and librustzcash's
 * diversified addresses). Nothing here holds a secret: the account it is
 * handed has viewing keys and addresses only, and the caller wipes both.
 */
#ifndef LEEK_ZCASH_ADDRCHECK_H
#define LEEK_ZCASH_ADDRCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zcash-account.h"

/* External transparent indices searched: 0..199. The search reads the
 * account's xpub only, one public CKD and one HASH160 per index, and the
 * CKD is a full secp256k1 scalar multiplication (trezor-crypto is built
 * without its precomputed tables): 0.7 ms an index on a desktop host
 * (test_zcash_addrcheck prints it), so an estimated 20-40 ms on the S3 and
 * 4-8 s for an address that is not there; 1000 would be 20-40 s. The device
 * logs the real time ("check address: ... transparent indices in N ms"); a
 * build can move it with -DZAC_T_SEARCH=... */
#ifndef ZAC_T_SEARCH
#define ZAC_T_SEARCH  200u
#endif
/* Indices per zac_t_step(): the UI draws progress and reads Cancel between,
 * about every second on the S3. */
#define ZAC_T_CHUNK   20u

typedef enum {
    ZAC_KIND_UA = 0,
    ZAC_KIND_SAPLING,
    ZAC_KIND_P2PKH,      /* t1 / tm */
    ZAC_KIND_P2SH,       /* t3 / t2 */
    ZAC_KIND_TEX,        /* tex1 / textest1: a P2PKH hash, ZIP 320 */
} ZacKind;

typedef enum {
    ZAC_PENDING = 0,     /* parsed; the account checks are still to run */
    ZAC_OURS,
    ZAC_NOT_OURS,
    ZAC_WRONG_NET,
    ZAC_CANNOT_CHECK,    /* nothing in it the device has keys for (Sapling) */
    ZAC_MALFORMED,       /* not a Zcash address (or URI holding one) */
} ZacVerdict;

/* Why an address is not this wallet's: one line on the result page. */
typedef enum {
    ZAC_WHY_NONE = 0,
    ZAC_WHY_FOREIGN,     /* well formed, no account's key makes it */
    ZAC_WHY_SAPLING,     /* ZAC_CANNOT_CHECK: a Sapling address, no Sapling keys here */
    ZAC_WHY_NO_ORCHARD,  /* ZAC_CANNOT_CHECK: a UA without an Orchard receiver */
    ZAC_WHY_P2SH,        /* a P2SH address: this wallet makes none */
    ZAC_WHY_RECEIVER,    /* a UA with a P2SH or unknown receiver, or metadata */
    ZAC_WHY_T_RANGE,     /* transparent, not in the searched range */
} ZacWhy;

/* Receivers the address carries, for the result page. */
#define ZAC_R_ORCHARD      0x01
#define ZAC_R_TRANSPARENT  0x02
#define ZAC_R_OTHER        0x04   /* P2SH, unknown or metadata inside a UA */
#define ZAC_R_SAPLING      0x08   /* present, never checked: no Sapling keys */

typedef struct {
    char       addr[ZS_UA_MAX];   /* as checked: trimmed, lower-case bech32 */
    ZacKind    kind;
    uint8_t    net;               /* the address's own network, ZCASH_NET_* */
    uint8_t    receivers;         /* ZAC_R_* */
    uint8_t    p2pkh[20];         /* P2PKH / TEX: the key hash to find */
    ZacVerdict verdict;
    ZacWhy     why;
    uint32_t   account;           /* ZAC_OURS: which account */
    uint8_t    index_le[11];      /* ZAC_OURS: the diversifier / child index */
    uint32_t   t_next;            /* transparent search: next index to try */
} ZacCheck;

/**
 * Read a scanned QR's text into `c`. `board_net` is the board's network.
 * Returns c->verdict: ZAC_MALFORMED (nothing usable; c->addr is empty),
 * ZAC_WRONG_NET, ZAC_NOT_OURS for kinds no account can make (P2SH, a UA
 * with such a receiver), ZAC_CANNOT_CHECK (Sapling, a UA without Orchard),
 * or ZAC_PENDING when the accounts must be asked.
 */
ZacVerdict zac_parse(const char *qr, size_t len, uint8_t board_net, ZacCheck *c);

/** True for the kinds whose check is the transparent index search. */
bool zac_is_transparent(const ZacCheck *c);

/**
 * A pending UA against one account (from zcash_account_derive on c->net).
 * ZAC_OURS sets account and index; ZAC_NOT_OURS means not THIS account's
 * (the caller tries the next) and leaves the verdict pending; anything else
 * is final and written to c->verdict.
 */
ZacVerdict zac_check_ua(ZacCheck *c, const ZcashAccount *a);

/**
 * The next ZAC_T_CHUNK transparent indices of account `a`. True when the
 * search is over: c->verdict is then ZAC_OURS or ZAC_NOT_OURS.
 */
bool zac_t_step(ZacCheck *c, const ZcashAccount *a);

/* ---- Result pages, 8 rows of 21 characters ---------------------------- */

#define ZAC_ROWS 8
#define ZAC_COLS 21

typedef enum {
    ZAC_ROW_TEXT = 0,
    ZAC_ROW_GOOD,
    ZAC_ROW_DANGER,
    ZAC_ROW_WARN,
    ZAC_ROW_DIM,
} ZacRowRole;

typedef struct {
    char       row[ZAC_ROWS][ZAC_COLS + 1];
    ZacRowRole role[ZAC_ROWS];
} ZacPage;

/** The verdict, then the address in groups of four. At least 1. */
int zac_page_count(const ZacCheck *c);

/** Page `page` (0-based), footer `hint` on row 7. False past the last. */
bool zac_render(const ZacCheck *c, int page, const char *hint, ZacPage *out);

/** "Mainnet" / "Testnet". */
const char *zac_net_name(uint8_t net);

#endif /* LEEK_ZCASH_ADDRCHECK_H */
