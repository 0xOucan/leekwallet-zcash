/* LeekWallet Zcash: secp256k1 for the C signer (see zc_secp.h).
 *
 * The only translation unit of zsign-c that uses trezor-crypto. It must be
 * compiled WITHOUT vendor/zc_vendor_prefix.h: the prefix would rename these
 * calls to the vendored library's zcv_* copies, whose bignum layout is not
 * trezor-crypto's. The headers are named by path, so that the vendored
 * include directory (which has its own bignum.h, sha2.h and memzero.h)
 * cannot shadow them; trezor-crypto's own includes resolve next to it.
 */
#include "zc_secp.h"

#include <string.h>

#include "../trezor-crypto/bignum.h"
#include "../trezor-crypto/bip32.h"
#include "../trezor-crypto/curves.h"
#include "../trezor-crypto/ecdsa.h"
#include "../trezor-crypto/memzero.h"
#include "../trezor-crypto/secp256k1.h"

#define H(i) (0x80000000u | (i))

void zc_t_wipe(void *p, size_t n) { memzero(p, n); }

int zc_t_account_derive(const uint8_t seed[64], uint32_t coin, uint32_t account,
                        zc_t_account *out)
{
    HDNode node;
    int rc = -1;
    if (coin >= 0x80000000u || account >= 0x80000000u) return -1;
    if (hdnode_from_seed(seed, 64, SECP256K1_NAME, &node) == 1 &&
        hdnode_private_ckd(&node, H(44)) == 1 &&
        hdnode_private_ckd(&node, H(coin)) == 1 &&
        hdnode_private_ckd(&node, H(account)) == 1 &&
        hdnode_fill_public_key(&node) == 0) {
        memcpy(out->chain_code, node.chain_code, 32);
        memcpy(out->priv, node.private_key, 32);
        memcpy(out->pub, node.public_key, 33);
        out->child_num = node.child_num;
        rc = 0;
    }
    memzero(&node, sizeof node);
    return rc;
}

int zc_t_child(const zc_t_account *a, uint32_t scope, uint32_t index, uint8_t sk[32],
               uint8_t pk[33])
{
    HDNode node;
    int rc = -1;
    if (scope >= 0x80000000u || index >= 0x80000000u) return -1;
    if (hdnode_from_xprv(3, a->child_num, a->chain_code, a->priv, SECP256K1_NAME, &node) == 1 &&
        hdnode_private_ckd(&node, scope) == 1 &&
        hdnode_private_ckd(&node, index) == 1 &&
        hdnode_fill_public_key(&node) == 0) {
        memcpy(sk, node.private_key, 32);
        memcpy(pk, node.public_key, 33);
        rc = 0;
    }
    memzero(&node, sizeof node);
    return rc;
}

void zc_t_hash160(const uint8_t pk[33], uint8_t out[20])
{
    /* ecdsa_get_pubkeyhash reads 65 bytes after a 0x04 prefix; only the
     * compressed form is ever hashed here. */
    if (pk[0] != 0x02 && pk[0] != 0x03) {
        memset(out, 0, 20);     /* no P2PKH script pays to all zeros knowingly */
        return;
    }
    ecdsa_get_pubkeyhash(pk, HASHER_SHA2_RIPEMD, out);
}

/* s <= n/2 (BIP 62 low S, as libsecp256k1 produces and requires). */
static int low_s(const uint8_t sig[64])
{
    bignum256 s;
    bn_read_be(sig + 32, &s);
    int ok = !bn_is_zero(&s) && !bn_is_less(&secp256k1.order_half, &s);
    memzero(&s, sizeof s);
    return ok;
}

int zc_t_sign(const uint8_t sk[32], const uint8_t digest[32], uint8_t sig[64])
{
    /* RFC 6979 with SHA-256, no extra entropy: the nonce libsecp256k1
     * derives too. trezor-crypto returns the low-S form. */
    if (ecdsa_sign_digest(&secp256k1, sk, digest, sig, NULL, NULL) != 0) return -1;
    return low_s(sig) ? 0 : -1;
}

int zc_t_verify(const uint8_t pk[33], const uint8_t digest[32], const uint8_t sig[64])
{
    if (pk[0] != 0x02 && pk[0] != 0x03) return -1;
    if (!low_s(sig)) return -1;
    return ecdsa_verify_digest(&secp256k1, pk, sig, digest) == 0 ? 0 : -1;
}
