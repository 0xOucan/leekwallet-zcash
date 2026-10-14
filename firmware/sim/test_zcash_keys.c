/* Zcash key derivation against librustzcash vectors (vectors/keys_vectors.h).
 *
 * Two independent implementations are checked against one reference:
 *   - trezor-crypto (C, already on the device): BIP39 seed, BIP44 coin 133
 *     account key and t1 address.
 *   - zsign-rust (orchard 0.16): ZIP 32 Orchard FVK and raw address.
 * The vectors come from zcash_keys, so a pass means the device derives the
 * same account a Zcash wallet would import.
 */
#include <stdio.h>
#include <string.h>

#include "bip32.h"
#include "bip39.h"
#include "curves.h"
#include "memzero.h"
#include "zsign_rust.h"
#include "../../vectors/keys_vectors.h"

#define ZCASH_T1_PREFIX 0x1CB8 /* mainnet P2PKH, "t1" */
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
    hdnode_private_ckd(&node, H(133));
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
    hdnode_get_address(&node, ZCASH_T1_PREFIX, taddr, sizeof taddr);
    expect(strcmp(taddr, v->transparent_address) == 0, v->name, "t1 address (trezor-crypto)");
    memzero(&node, sizeof node);

    uint8_t fvk[96], addr[43];
    expect(zs_spike_fvk(seed, v->account, fvk) == 0, v->name, "zs_spike_fvk rc");
    expect(memcmp(fvk, v->orchard_fvk, 96) == 0, v->name, "Orchard FVK (zsign-rust)");
    expect(zs_spike_address(seed, v->account, addr) == 0, v->name, "zs_spike_address rc");
    expect(memcmp(addr, v->orchard_address_raw, 43) == 0, v->name,
           "Orchard raw address (zsign-rust)");
    memzero(seed, sizeof seed);
}

int main(void)
{
    printf("== test_zcash_keys\n== %zu librustzcash vectors: seed, BIP44/133, Orchard ZIP 32\n",
           ZV_KEYS_COUNT);
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) check_vector(&ZV_KEYS[i]);
    if (fails) {
        printf("\nFAILED (%d failures)\n", fails);
        return 1;
    }
    printf("\nPASSED (0 failures)\n");
    return 0;
}
