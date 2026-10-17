/* LeekWallet Zcash: secp256k1 for the C signer's transparent inputs.
 *
 * BIP 32/44 key derivation, HASH160 and ECDSA (RFC 6979, low S), done by
 * trezor-crypto in zc_secp.c, the one translation unit of zsign-c that calls
 * it. Everything else in zsign-c stays on the vendored, zcv_*-prefixed
 * library: this header names no trezor-crypto type, so zsign_c.c can include
 * it next to the vendored headers. The Rust backend does the same with
 * libsecp256k1 and the bip32 crate, so the two still share no code.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The account key m/44'/coin'/account'. */
typedef struct {
    uint8_t chain_code[32];
    uint8_t priv[32];
    uint8_t pub[33];            /* compressed */
    uint32_t child_num;
} zc_t_account;

/* Derive m/44'/coin'/account' from a 64-byte seed. 0 on success. */
int zc_t_account_derive(const uint8_t seed[64], uint32_t coin, uint32_t account,
                        zc_t_account *out);

/* The key at .../scope/index (both non-hardened). 0 on success. */
int zc_t_child(const zc_t_account *a, uint32_t scope, uint32_t index, uint8_t sk[32],
               uint8_t pk[33]);

/* RIPEMD160(SHA256(pk)) of a 33-byte compressed key. */
void zc_t_hash160(const uint8_t pk[33], uint8_t out[20]);

/* ECDSA over a 32-byte digest: r || s with s <= n/2, RFC 6979 nonce. */
int zc_t_sign(const uint8_t sk[32], const uint8_t digest[32], uint8_t sig[64]);

/* 0 if `sig` (r || s, s <= n/2) verifies under `pk` for `digest`. */
int zc_t_verify(const uint8_t pk[33], const uint8_t digest[32], const uint8_t sig[64]);

/* Wipe (not optimised away). */
void zc_t_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
