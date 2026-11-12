/**
 * The Zcash account the device shows and exports (ZIP 32, ZIP 316).
 *
 * Orchard + P2PKH, no Sapling: the device does not spend Sapling, so it does
 * not advertise a Sapling receiver it could not spend from. Two independent
 * implementations meet in every account: zsign (Rust, orchard) derives the
 * Orchard half from the seed, trezor-crypto derives the BIP44 transparent half,
 * and zsign assembles the UFVK and UA from both.
 *
 * Checked against librustzcash in sim/test_zcash_account.c.
 */
#ifndef LEEK_ZCASH_ACCOUNT_H
#define LEEK_ZCASH_ACCOUNT_H

#include <stdbool.h>
#include <stdint.h>

#include "leek-wallet.h"
#include "zsign_rust.h"

#define ZCASH_NET_MAIN ZS_NET_MAIN
#define ZCASH_NET_TEST ZS_NET_TEST

#define ZCASH_TADDR_MAX 40

typedef struct {
    uint32_t   account;
    uint8_t    network;                 /* ZCASH_NET_* */
    zs_account keys;                    /* fingerprint, Orchard FVK/receiver, UFVK, UA */
    uint8_t    t_account[65];           /* chain code || pubkey of m/44'/coin'/account' */
    uint8_t    t_receiver[20];          /* HASH160 at .../0/0 */
    char       t_address[ZCASH_TADDR_MAX]; /* "t1..." / "tm..." */
    /* The same Orchard receiver alone: what Receive shows and puts in its QR
     * (zcash-receive.h). keys.ua, with the P2PKH receiver, is what the
     * companion imports and checks against. */
    char       ua_orchard[ZS_UA_MAX];
} ZcashAccount;

/** ZIP 32 / BIP44 coin type: 133 on mainnet, 1 on testnet. */
uint32_t zcash_coin_type(uint8_t network);

/**
 * Derive `account` on `network` from the unlocked wallet's seed.
 * Nothing secret is written to `out`: it holds viewing keys and addresses only.
 */
WalletError zcash_account_derive(uint32_t account, uint8_t network, ZcashAccount *out);

/** What zcash_ua_check found. */
typedef enum {
    ZCASH_UA_OURS = 0,       /* every receiver is this account's */
    ZCASH_UA_MALFORMED,      /* not a UA on this network, or no Orchard receiver */
    ZCASH_UA_NOT_OURS,       /* a receiver belongs to someone else */
    ZCASH_UA_UNSUPPORTED,    /* a receiver the device cannot check (Sapling, P2SH, ...) */
} ZcashUaCheck;

/**
 * Is `ua` one of `a`'s own external addresses, at any diversifier index? The
 * Orchard receiver must be the account's (pk_d = [ivk] g_d); its index comes
 * from decrypting the diversifier, and a P2PKH receiver must be the account's
 * transparent key at that same index (ZIP 316). On ZCASH_UA_OURS, index_le
 * holds the 11-byte index and t_address_out the transparent address the UA
 * carries ("" if none). Needs `a` from zcash_account_derive.
 */
ZcashUaCheck zcash_ua_check(const ZcashAccount *a, const char *ua, uint8_t index_le[11],
                            char t_address_out[ZCASH_TADDR_MAX]);

/**
 * Search the account's external transparent keys m/44'/coin'/account'/0/i,
 * i in [from, from + count) and below 2^31, for the P2PKH key hash `hash160`.
 * Public derivation from the account key `a` holds (t_account), so no seed
 * and no private key: on the Zcash worker for its stack. True with the index
 * in *index_out when found. Check address (zcash-addrcheck.h) uses it.
 */
bool zcash_t_find(const ZcashAccount *a, const uint8_t hash160[20], uint32_t from,
                  uint32_t count, uint32_t *index_out);

#endif /* LEEK_ZCASH_ACCOUNT_H */
