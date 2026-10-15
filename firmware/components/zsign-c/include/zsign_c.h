/* LeekWallet Zcash: C signer backend.
 *
 * A second, independent implementation of zsign_rust.h's signer: the PCZT is
 * read by our own postcard reader (pczt_reader.c), and every check is redone
 * with libzcash-ironwood-c (vendored, MIT) plus this component's own code. No
 * Rust code is called or linked. Both backends fill the same zs_* structs and
 * return the same ZS_* codes (zsign-common/include/zsign_types.h), so the
 * firmware can require them to agree before it signs.
 *
 * Memory: no heap. zc_pczt_new uses one static context (about 14 KB on the
 * ESP32-S3); a second zc_pczt_new before zc_pczt_free fails with
 * ZS_E_INTERNAL. zc_pczt_new_in places the context in caller memory instead.
 * The context points into `bytes`, which must stay valid until
 * zc_pczt_free; their hash is taken at zc_pczt_new and re-checked before
 * signing, so a buffer that changed after the check is refused.
 *
 * Not single-thread-safe: the vendored library keeps static scratch state.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../../zsign-common/include/zsign_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as zs_account_derive. */
int32_t zc_account_derive(const uint8_t seed[64], uint32_t account, uint8_t net,
                          const uint8_t *t_account, const uint8_t *t_receiver,
                          zs_account *out);

typedef struct zc_ctx zc_ctx;

zc_ctx *zc_pczt_new(const uint8_t *bytes, size_t len, int32_t *err);
/* As zc_pczt_new, in caller memory of at least zc_ctx_size() bytes,
 * aligned for a pointer. */
size_t  zc_ctx_size(void);
zc_ctx *zc_pczt_new_in(void *mem, size_t mem_len, const uint8_t *bytes, size_t len,
                       int32_t *err);
void    zc_pczt_free(zc_ctx *ctx);
/* All checks; fills the summary. Required before zc_pczt_sign. */
int32_t zc_pczt_check(zc_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                      zs_summary *out);
/* Sign every spend of ours, after the user accepted the summary. One passing
 * check authorises one signing, with the same seed and account. */
int32_t zc_pczt_sign(zc_ctx *ctx, const uint8_t seed[64], uint32_t account, uint8_t net,
                     zs_sig *sigs, uint32_t max, uint32_t *n_out);
/* Verify one signature against the action's rk and the sighash (as a host does). */
int32_t zc_pczt_verify_sig(zc_ctx *ctx, const zs_sig *sig);

/* Known-answer self test of the primitives the library lacks (Poseidon) and
 * of the embedded Sinsemilla table. 0 on success. */
int32_t zc_selftest(void);

#ifdef __cplusplus
}
#endif
