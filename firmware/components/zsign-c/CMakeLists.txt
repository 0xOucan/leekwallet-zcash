# LeekWallet Zcash: C signer backend as an ESP-IDF component (Phase 4).
#
# Plain C11, so unlike zsign-rust it builds for every target, the RISC-V C3
# included. Every translation unit is compiled with
# vendor/zc_vendor_prefix.h force-included, which renames each global the
# vendored libzcash-ironwood-c defines to zcv_*: the firmware also links
# trezor-crypto, which defines bn_*, blake2b_*, memzero and friends under the
# same names. zc_xcheck.c (the cross-check against the vendored arithmetic)
# is a host test only and is not built here.
set(ZCV vendor/libzcash-ironwood-c)

idf_component_register(
    SRCS "pczt_reader.c" "zsign_c.c" "zc_crypto.c" "zc_pallas.c" "zc_encoding.c"
         "zc_sinsemilla.S" "platform/zc_platform_idf.c"
         "${ZCV}/src/orchard.c" "${ZCV}/src/pallas.c" "${ZCV}/src/redpallas.c"
         "${ZCV}/src/zip244.c" "${ZCV}/src/bignum.c" "${ZCV}/src/blake2b.c"
         "${ZCV}/src/memzero.c" "${ZCV}/src/segwit_addr.c"
         "${ZCV}/src/chacha20poly1305.c" "${ZCV}/src/sha2.c" "${ZCV}/src/aes/aes_ct.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${ZCV}/include" "${ZCV}/src"
    PRIV_REQUIRES esp_hw_support)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -include ${COMPONENT_DIR}/vendor/zc_vendor_prefix.h
    -std=gnu11
    # The vendored sources are compiled as they are, warts included.
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
# zc_sinsemilla.S embeds the vendored S table with .incbin.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-Wa,-I${COMPONENT_DIR}/${ZCV}/src>)
