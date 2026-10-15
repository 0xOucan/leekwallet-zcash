/* Fake Zcash account for the protocol suites.
 *
 * test_protocol exercises the wire: who may ask, what comes back, how it is
 * encoded. Real derivation through the real vault is test_zcash_account's
 * job, so here the answer is a librustzcash vector, looked up by account and
 * network. The lock and session gates are protocol.c's, checked before this.
 */
#include <string.h>

#include "zcash-account.h"
#include "../../vectors/keys_vectors.h"

int fake_zcash_derive_calls;

uint32_t zcash_coin_type(uint8_t network) { return network == ZCASH_NET_MAIN ? 133 : 1; }

WalletError zcash_account_derive(uint32_t account, uint8_t network, ZcashAccount *out)
{
    fake_zcash_derive_calls++;
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const zv_key_vector *v = &ZV_KEYS[i];
        if (v->account == account && v->network == network && !v->passphrase[0]) {
            out->account = account;
            out->network = network;
            memcpy(out->keys.seed_fingerprint, v->seed_fingerprint, 32);
            strcpy(out->keys.ufvk, v->ufvk);
            strcpy(out->keys.ua, v->ua);
            strcpy(out->t_address, v->transparent_address);
            return WALLET_OK;
        }
    }
    return WALLET_ERROR_DERIVATION_FAILED;
}
