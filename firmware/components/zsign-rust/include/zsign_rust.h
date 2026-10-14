/* LeekWallet Zcash: Rust signer backend, C ABI. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
