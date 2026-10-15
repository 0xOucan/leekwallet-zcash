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
} ZcashAccount;

/** ZIP 32 / BIP44 coin type: 133 on mainnet, 1 on testnet. */
uint32_t zcash_coin_type(uint8_t network);

/**
 * Derive `account` on `network` from the unlocked wallet's seed.
 * Nothing secret is written to `out`: it holds viewing keys and addresses only.
 */
WalletError zcash_account_derive(uint32_t account, uint8_t network, ZcashAccount *out);

#endif /* LEEK_ZCASH_ACCOUNT_H */
