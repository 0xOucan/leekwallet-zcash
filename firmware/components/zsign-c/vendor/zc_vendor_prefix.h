/* LeekWallet Zcash: symbol prefix for the vendored libzcash-ironwood-c.
 *
 * Force-included (-include) into every translation unit of zsign-c, so each
 * global the vendored library defines or calls is renamed zcv_*. The library
 * carries its own (modified) copies of trezor-crypto's bignum, blake2b,
 * bech32, chacha20poly1305 and memzero under the same names; the firmware
 * links the real trezor-crypto too, and the two must not meet at link time.
 * Renaming them all (not only today's clashes) also keeps the C signer from
 * ever silently binding to a trezor-crypto function, which would weaken the
 * independence the dual-signer design depends on.
 *
 * Generated from `nm -g --defined-only` over the vendored objects, plus the
 * RNG entry points the library expects the platform to provide
 * (random_buffer/random32, supplied by zc_platform_*.c). Regenerate when
 * re-vendoring; see vendor/libzcash-ironwood-c/README.md.
 */
#ifndef ZC_VENDOR_PREFIX_H
#define ZC_VENDOR_PREFIX_H

#define aes_ct_256_ecb_encrypt                       zcv_aes_ct_256_ecb_encrypt
#define aes_ct_256_encrypt_single                    zcv_aes_ct_256_encrypt_single
#define aes_ct_256_keysched                          zcv_aes_ct_256_keysched
#define aes_ct_256_self_test                         zcv_aes_ct_256_self_test
#define aes_ct_256_set_override                      zcv_aes_ct_256_set_override
#define bech32_decode                                zcv_bech32_decode
#define bech32_encode                                zcv_bech32_encode
#define blake2b                                      zcv_blake2b
#define blake2b_Final                                zcv_blake2b_Final
#define blake2b_Init                                 zcv_blake2b_Init
#define blake2b_InitKey                              zcv_blake2b_InitKey
#define blake2b_InitKeyPersonal                      zcv_blake2b_InitKeyPersonal
#define blake2b_init_param                           zcv_blake2b_init_param
#define blake2b_InitPersonal                         zcv_blake2b_InitPersonal
#define blake2b_Key                                  zcv_blake2b_Key
#define blake2b_Update                               zcv_blake2b_Update
#define bn_add                                       zcv_bn_add
#define bn_addi                                      zcv_bn_addi
#define bn_addmod                                    zcv_bn_addmod
#define bn_bitcount                                  zcv_bn_bitcount
#define bn_clearbit                                  zcv_bn_clearbit
#define bn_cmov                                      zcv_bn_cmov
#define bn_cnegate                                   zcv_bn_cnegate
#define bn_digitcount                                zcv_bn_digitcount
#define bn_divide_base                               zcv_bn_divide_base
#define bn_divmod10                                  zcv_bn_divmod10
#define bn_divmod1000                                zcv_bn_divmod1000
#define bn_divmod58                                  zcv_bn_divmod58
#define bn_fast_mod                                  zcv_bn_fast_mod
#define bn_format                                    zcv_bn_format
#define bn_inverse                                   zcv_bn_inverse
#define bn_is_equal                                  zcv_bn_is_equal
#define bn_is_less                                   zcv_bn_is_less
#define bn_is_one                                    zcv_bn_is_one
#define bn_is_zero                                   zcv_bn_is_zero
#define bn_long_division                             zcv_bn_long_division
#define bn_lshift                                    zcv_bn_lshift
#define bn_mod                                       zcv_bn_mod
#define bn_mult_half                                 zcv_bn_mult_half
#define bn_multiply                                  zcv_bn_multiply
#define bn_multiply_long                             zcv_bn_multiply_long
#define bn_multiply_reduce                           zcv_bn_multiply_reduce
#define bn_multiply_reduce_step                      zcv_bn_multiply_reduce_step
#define bn_mult_k                                    zcv_bn_mult_k
#define bn_normalize                                 zcv_bn_normalize
#define bn_one                                       zcv_bn_one
#define bn_power_mod                                 zcv_bn_power_mod
#define bn_read_be                                   zcv_bn_read_be
#define bn_read_le                                   zcv_bn_read_le
#define bn_read_uint32                               zcv_bn_read_uint32
#define bn_read_uint64                               zcv_bn_read_uint64
#define bn_rshift                                    zcv_bn_rshift
#define bn_setbit                                    zcv_bn_setbit
#define bn_sqrt                                      zcv_bn_sqrt
#define bn_subi                                      zcv_bn_subi
#define bn_subtract                                  zcv_bn_subtract
#define bn_subtractmod                               zcv_bn_subtractmod
#define bn_testbit                                   zcv_bn_testbit
#define bn_write_be                                  zcv_bn_write_be
#define bn_write_le                                  zcv_bn_write_le
#define bn_xor                                       zcv_bn_xor
#define bn_zero                                      zcv_bn_zero
#define chacha20poly1305_decrypt                     zcv_chacha20poly1305_decrypt
#define chacha20poly1305_encrypt                     zcv_chacha20poly1305_encrypt
#define chacha20poly1305_self_test                   zcv_chacha20poly1305_self_test
#define ct_memequal                                  zcv_ct_memequal
#define f4jumble                                     zcv_f4jumble
#define f4jumble_inv                                 zcv_f4jumble_inv
#define ff1_aes256_encrypt                           zcv_ff1_aes256_encrypt
#define fp_add                                       zcv_fp_add
#define fp_inv                                       zcv_fp_inv
#define fp_is_square                                 zcv_fp_is_square
#define fp_mul                                       zcv_fp_mul
#define fp_neg                                       zcv_fp_neg
#define fp_sqr                                       zcv_fp_sqr
#define fp_sqrt                                      zcv_fp_sqrt
#define fp_sub                                       zcv_fp_sub
#define fq_reduce                                    zcv_fq_reduce
#define inverse_mod_power_two                        zcv_inverse_mod_power_two
#define memzero                                      zcv_memzero
#define orchard_child_key                            zcv_orchard_child_key
#define orchard_compute_cmx                          zcv_orchard_compute_cmx
#define orchard_compute_enc_ciphertext               zcv_orchard_compute_enc_ciphertext
#define orchard_compute_enc_ciphertext_from_rseed    zcv_orchard_compute_enc_ciphertext_from_rseed
#define orchard_decode_ua_orchard_receiver           zcv_orchard_decode_ua_orchard_receiver
#define orchard_derive_account_sk                    zcv_orchard_derive_account_sk
#define orchard_derive_keys                          zcv_orchard_derive_keys
#define orchard_derive_unified_address               zcv_orchard_derive_unified_address
#define orchard_encode_ua_raw                        zcv_orchard_encode_ua_raw
#define orchard_master_key                           zcv_orchard_master_key
#define orchard_sign_with_personal                   zcv_orchard_sign_with_personal
#define pallas_from_jac                              zcv_pallas_from_jac
#define pallas_group_hash                            zcv_pallas_group_hash
#define pallas_hash_to_curve                         zcv_pallas_hash_to_curve
#define pallas_init                                  zcv_pallas_init
#define pallas_jac_add                               zcv_pallas_jac_add
#define pallas_jac_add_mixed                         zcv_pallas_jac_add_mixed
#define pallas_jac_double                            zcv_pallas_jac_double
#define pallas_p                                     zcv_pallas_p
#define pallas_point_mul                             zcv_pallas_point_mul
#define pallas_point_set_infinity                    zcv_pallas_point_set_infinity
#define pallas_q                                     zcv_pallas_q
#define pallas_report                                zcv_pallas_report
#define pallas_set_progress_cb                       zcv_pallas_set_progress_cb
#define pallas_set_sinsemilla_lookup                 zcv_pallas_set_sinsemilla_lookup
#define pallas_set_yield_cb                          zcv_pallas_set_yield_cb
#define pallas_to_jac                                zcv_pallas_to_jac
#define pallas_verify_sinsemilla_table               zcv_pallas_verify_sinsemilla_table
#define random32                                     zcv_random32
#define random_buffer                                zcv_random_buffer
#define redpallas_derive_ak                          zcv_redpallas_derive_ak
#define redpallas_sign                               zcv_redpallas_sign
#define segwit_addr_decode                           zcv_segwit_addr_decode
#define segwit_addr_encode                           zcv_segwit_addr_encode
#define sha1_Data                                    zcv_sha1_Data
#define sha1_End                                     zcv_sha1_End
#define sha1_Final                                   zcv_sha1_Final
#define sha1_Init                                    zcv_sha1_Init
#define sha1_initial_hash_value                      zcv_sha1_initial_hash_value
#define sha1_Raw                                     zcv_sha1_Raw
#define sha1_Transform                               zcv_sha1_Transform
#define sha1_Update                                  zcv_sha1_Update
#define sha256_Data                                  zcv_sha256_Data
#define sha256_End                                   zcv_sha256_End
#define sha256_Final                                 zcv_sha256_Final
#define sha256_Init                                  zcv_sha256_Init
#define sha256_Init_ex                               zcv_sha256_Init_ex
#define sha256_initial_hash_value                    zcv_sha256_initial_hash_value
#define sha256_Raw                                   zcv_sha256_Raw
#define sha256_Transform                             zcv_sha256_Transform
#define sha256_Update                                zcv_sha256_Update
#define sha512_Data                                  zcv_sha512_Data
#define sha512_End                                   zcv_sha512_End
#define sha512_Final                                 zcv_sha512_Final
#define sha512_Init                                  zcv_sha512_Init
#define sha512_initial_hash_value                    zcv_sha512_initial_hash_value
#define sha512_Raw                                   zcv_sha512_Raw
#define sha512_Transform                             zcv_sha512_Transform
#define sha512_Update                                zcv_sha512_Update
#define sinsemilla_hash_to_point                     zcv_sinsemilla_hash_to_point
#define sinsemilla_short_commit                      zcv_sinsemilla_short_commit
#define zip244_actions_init_pool                     zcv_zip244_actions_init_pool
#define zip244_bundle_digest                         zcv_zip244_bundle_digest
#define zip244_empty_digest                          zcv_zip244_empty_digest
#define zip244_encode_tx_meta                        zcv_zip244_encode_tx_meta
#define zip244_hash_action                           zcv_zip244_hash_action
#define zip244_hash_transparent_input                zcv_zip244_hash_transparent_input
#define zip244_hash_transparent_output               zcv_zip244_hash_transparent_output
#define zip244_header_digest                         zcv_zip244_header_digest
#define zip244_parse_tx_meta                         zcv_zip244_parse_tx_meta
#define zip244_pool_empty_digest                     zcv_zip244_pool_empty_digest
#define zip244_sapling_empty_digest                  zcv_zip244_sapling_empty_digest
#define zip244_shielded_sighash_v6                   zcv_zip244_shielded_sighash_v6
#define zip244_transparent_digest                    zcv_zip244_transparent_digest
#define zip244_transparent_init                      zcv_zip244_transparent_init
#define zip244_transparent_per_input_sighash         zcv_zip244_transparent_per_input_sighash

#endif /* ZC_VENDOR_PREFIX_H */
