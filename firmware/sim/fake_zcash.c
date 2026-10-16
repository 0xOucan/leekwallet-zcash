/* Fake Zcash account for the protocol suites.
 *
 * test_protocol exercises the wire: who may ask, what comes back, how it is
 * encoded. Real derivation through the real vault is test_zcash_account's
 * job, so here the answer is a librustzcash vector, looked up by account and
 * network. The lock and session gates are protocol.c's, checked before this.
 */
#include <stdbool.h>
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

/* The real check is test_zcash_account's (real keys, librustzcash's
 * diversified addresses). Here: the account's two vector addresses are its
 * own, any other vector's address is someone else's, anything else is not
 * an address. */
ZcashUaCheck zcash_ua_check(const ZcashAccount *a, const char *ua, uint8_t index_le[11],
                            char t_address_out[ZCASH_TADDR_MAX])
{
    memset(index_le, 0, 11);
    t_address_out[0] = 0;
    for (size_t i = 0; i < ZV_KEYS_COUNT; i++) {
        const zv_key_vector *v = &ZV_KEYS[i];
        bool mine = v->account == a->account && v->network == a->network && !v->passphrase[0];
        if (strcmp(ua, v->ua) == 0) {
            if (!mine) return ZCASH_UA_NOT_OURS;
            strcpy(t_address_out, v->transparent_address);
            return ZCASH_UA_OURS;
        }
        if (strcmp(ua, v->ua_orchard_only) == 0) return mine ? ZCASH_UA_OURS : ZCASH_UA_NOT_OURS;
    }
    return ZCASH_UA_MALFORMED;
}
