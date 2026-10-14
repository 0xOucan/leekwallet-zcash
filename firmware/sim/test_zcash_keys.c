/* Zcash key derivation against librustzcash vectors (vectors/keys_vectors.h).
 *
 * Two independent implementations are checked against one reference:
 *   - trezor-crypto (C, already on the device): BIP39 seed, BIP44 coin 133
 *     account key and t1 address.
 *   - zsign-rust (orchard 0.16, zcash_address): ZIP 32 Orchard keys, seed
 *     fingerprint, and the ZIP 316 UFVK and UA built from both halves.
 * The vectors come from zcash_keys, so a pass means the device derives the
 * same account a Zcash wallet would import.
 */
#include <stdio.h>
#include <string.h>

#include "bip32.h"
#include "bip39.h"
#include "curves.h"
#include "ecdsa.h"
#include "memzero.h"
#include "zsign_rust.h"
#include "../../vectors/keys_vectors.h"

#define ZCASH_T1_PREFIX 0x1CB8 /* mainnet P2PKH, "t1" */
#define ZCASH_TM_PREFIX 0x1D25 /* testnet P2PKH, "tm" */
#define H(i) (0x80000000u | (i))

static int fails;

static void expect(int ok, const char *vec, const char *what)
{
    if (!ok) {
        printf("  FAIL %s: %s\n", vec, what);
        fails++;
    }
}

static void check_vector(const zv_key_vector *v)
{
    uint8_t seed[64];
    mnemonic_to_seed(v->mnemonic, v->passphrase, seed, NULL);
    expect(memcmp(seed, v->seed, 64) == 0, v->name, "BIP39 seed (trezor-crypto)");

    /* BIP44 m/44'/133'/account' : account key = chain code || compressed pubkey. */
    HDNode node;
    hdnode_from_seed(seed, 64, SECP256K1_NAME, &node);
    hdnode_private_ckd(&node, H(44));
    hdnode_private_ckd(&node, H(v->network ? 1 : 133));
    hdnode_private_ckd(&node, H(v->account));
    hdnode_fill_public_key(&node);
    uint8_t acct[65];
    memcpy(acct, node.chain_code, 32);
    memcpy(acct + 32, node.public_key, 33);
    expect(memcmp(acct, v->transparent_account_pubkey, 65) == 0, v->name,
           "transparent account key (trezor-crypto)");

    /* .../0/0 : first external address. */
    hdnode_private_ckd(&node, 0);
    hdnode_private_ckd(&node, 0);
    hdnode_fill_public_key(&node);
    char taddr[64];
    hdnode_get_address(&node, v->network ? ZCASH_TM_PREFIX : ZCASH_T1_PREFIX, taddr, sizeof taddr);
    expect(strcmp(taddr, v->transparent_address) == 0, v->name, "t address (trezor-crypto)");
    uint8_t t_receiver[20];
    ecdsa_get_pubkeyhash(node.public_key, HASHER_SHA2_RIPEMD, t_receiver);
    expect(memcmp(t_receiver, v->transparent_receiver, 20) == 0, v->name,
           "P2PKH receiver (trezor-crypto)");
    memzero(&node, sizeof node);

    /* The UFVK and UA join the two implementations: trezor-crypto's
       transparent half and zsign-rust's Orchard half. */
    zs_account a;
    expect(zs_account_derive(seed, v->account, v->network, acct, t_receiver, &a) == ZS_OK,
           v->name, "zs_account_derive rc");
    expect(memcmp(a.seed_fingerprint, v->seed_fingerprint, 32) == 0, v->name,
           "ZIP 32 seed fingerprint (zsign-rust)");
    expect(memcmp(a.orchard_fvk, v->orchard_fvk, 96) == 0, v->name, "Orchard FVK (zsign-rust)");
    expect(memcmp(a.orchard_address, v->orchard_address_raw, 43) == 0, v->name,
           "Orchard raw address (zsign-rust)");
    expect(strcmp(a.ufvk, v->ufvk) == 0, v->name, "UFVK Orchard+P2PKH");
    expect(strcmp(a.ua, v->ua) == 0, v->name, "UA Orchard+P2PKH");

    zs_account o;
    expect(zs_account_derive(seed, v->account, v->network, NULL, NULL, &o) == ZS_OK,
           v->name, "zs_account_derive (Orchard only) rc");
    expect(strcmp(o.ua, v->ua_orchard_only) == 0, v->name, "UA Orchard only");
    memzero(&a, sizeof a);
    memzero(&o, sizeof o);
    memzero(seed, sizeof seed);
}

int main(void)
{
    printf("== test_zcash_keys\n== %zu librustzcash vectors (main+test): seed, BIP44, ZIP 32, UFVK, UA\n",
           ZV_KEYS_COUNT);
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) check_vector(&ZV_KEYS[i]);
    if (fails) {
        printf("\nFAILED (%d failures)\n", fails);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
