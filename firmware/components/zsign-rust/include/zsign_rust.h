/* LeekWallet Zcash: Rust signer backend, C ABI. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes. */
#define ZS_OK          0
#define ZS_ERR_ARG    (-1)
#define ZS_ERR_DERIVE (-2)
#define ZS_ERR_ENCODE (-3)

#define ZS_NET_MAIN 0
#define ZS_NET_TEST 1

#define ZS_UFVK_MAX 512
#define ZS_UA_MAX   256

/* One account as the device shows and exports it: Orchard + P2PKH (ZIP 316). */
typedef struct {
    uint8_t seed_fingerprint[32];   /* ZIP 32 seed fingerprint */
    uint8_t orchard_fvk[96];        /* ak || nk || rivk */
    uint8_t orchard_address[43];    /* raw receiver, diversifier index 0 */
    char    ufvk[ZS_UFVK_MAX];      /* "uview1..." / "uviewtest1...", NUL-terminated */
    char    ua[ZS_UA_MAX];          /* "u1..." / "utest1...", NUL-terminated */
} zs_account;

/* Derive account `account` from a 64-byte BIP39 seed. `net` is ZS_NET_*.
 * t_account: 65 bytes (chain code || compressed pubkey of m/44'/coin'/account'),
 * t_receiver: 20-byte HASH160 of the key at .../0/0. Either may be NULL to
 * leave the transparent item out of the UFVK or UA. */
int32_t zs_account_derive(const uint8_t seed[64], uint32_t account, uint8_t net,
                          const uint8_t *t_account, const uint8_t *t_receiver,
                          zs_account *out);

/* Phase 0.4 spike entry points. All return 0 on success. */
int32_t zs_spike_fvk(const uint8_t seed[64], uint32_t account, uint8_t out_fvk[96]);
int32_t zs_spike_address(const uint8_t seed[64], uint32_t account, uint8_t out_addr[43]);
int32_t zs_spike_sign(const uint8_t seed[64], uint32_t account, const uint8_t alpha[32],
                      const uint8_t sighash[32], uint8_t out_sig[64]);
int32_t zs_spike_cmx(const uint8_t seed[64], uint64_t value, uint8_t out_cmx[32]);
int32_t zs_spike_verify(const uint8_t seed[64], uint32_t account, const uint8_t alpha[32],
                        const uint8_t sighash[32], const uint8_t sig[64]);

#ifdef __cplusplus
}
#endif
