/* LeekWallet Zcash, C signer: ZIP 316 and transparent address encodings. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Revision-0 UFVK (Orchard FVK, optional P2PKH account key) and UA (Orchard
 * receiver, optional P2PKH receiver). net: 0 mainnet, 1 testnet. Return the
 * string length, or 0 if it does not fit `cap` (including the NUL). */
size_t zc_encode_ufvk(char *out, size_t cap, uint8_t net, const uint8_t fvk[96],
                      const uint8_t *t_account);
size_t zc_encode_ua(char *out, size_t cap, uint8_t net, const uint8_t orchard[43],
                    const uint8_t *p2pkh);

/* The receivers of a UA. `other` is set for any item this device cannot
 * check: P2SH, Sapling, an unknown typecode, or expiry metadata. */
typedef struct {
    int     has_orchard, has_p2pkh, other;
    uint8_t orchard[43];
    uint8_t p2pkh[20];
} zc_ua_items;

/* 1 if `ua` (len bytes, not NUL-terminated) is a well-formed UA on `net`
 * (ZIP 316: checksum, F4Jumble padding, ordering, item sizes); its
 * receivers in *out. */
int zc_ua_parse(const char *ua, size_t len, uint8_t net, zc_ua_items *out);

/* 1 if `ua` (len bytes, not NUL-terminated) is a well-formed revision-0 UA
 * on `net` that contains `raw43` as its Orchard receiver. */
int zc_ua_contains_orchard(const char *ua, size_t len, uint8_t net, const uint8_t raw43[43]);

/* Base58Check t-address for a P2PKH or P2SH script; 0 for any other script. */
size_t zc_t_address(char *out, size_t cap, uint8_t net, const uint8_t *script, size_t n);

/* 1 if the script's pushes all fit (zcash_script's parse). */
int zc_script_parses(const uint8_t *s, size_t n);

/* The same for the three script kinds a PCZT carries: PubKey (any valid
 * opcode; outputs), FromChain (any byte as an opcode; a spent coin's script),
 * Sig (pushes only; a script_sig). */
enum { ZC_SCRIPT_PUBKEY = 0, ZC_SCRIPT_FROM_CHAIN = 1, ZC_SCRIPT_SIG = 2 };
int zc_script_parses_as(const uint8_t *s, size_t n, int kind);

#ifdef __cplusplus
}
#endif
