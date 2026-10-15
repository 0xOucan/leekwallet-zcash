/* LeekWallet Zcash: Rust signer backend, C ABI.
 *
 * Codes and structs (ZS_*, zs_account, zs_summary, zs_output, zs_sig) live in
 * zsign-common/include/zsign_types.h, shared with the C backend (zsign_c.h)
 * so the two can be compared field by field. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../zsign-common/include/zsign_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Derive account `account` from a 64-byte BIP39 seed. `net` is ZS_NET_*.
 * t_account: 65 bytes (chain code || compressed pubkey of m/44'/coin'/account'),
 * t_receiver: 20-byte HASH160 of the key at .../0/0. Either may be NULL to
 * leave the transparent item out of the UFVK or UA. */
int32_t zs_account_derive(const uint8_t seed[64], uint32_t account, uint8_t net,
                          const uint8_t *t_account, const uint8_t *t_receiver,
                          zs_account *out);

/* ---- PCZT signer (ZIP 374) ------------------------------------------- */

typedef struct zs_ctx zs_ctx;

zs_ctx *zs_pczt_new(const uint8_t *bytes, size_t len, int32_t *err);
void    zs_pczt_free(zs_ctx *ctx);
/* All checks; fills the summary. Required before zs_pczt_sign. */
int32_t zs_pczt_check(zs_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                      zs_summary *out);
/* Sign every spend of ours, after the user accepted the summary. */
int32_t zs_pczt_sign(zs_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                     zs_sig *sigs, uint32_t max, uint32_t *n_out);
/* Verify one signature against the action's rk and the sighash (as a host does). */
int32_t zs_pczt_verify_sig(const zs_ctx *ctx, const zs_sig *sig);

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
