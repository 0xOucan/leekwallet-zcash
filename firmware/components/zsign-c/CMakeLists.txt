# LeekWallet Zcash: C signer backend as an ESP-IDF component (Phase 4).
#
# Plain C11, so unlike zsign-rust it builds for every target, the RISC-V C3
# included. Every global the vendored libzcash-ironwood-c defines is renamed
# zcv_* by vendor/zc_vendor_prefix.h: the firmware also links trezor-crypto,
# which defines bn_*, blake2b_*, memzero and friends under the same names.
#
# The prefix reaches each translation unit through a build unit in idf/ that
# includes it and then the real source, unchanged. It used to be a
# `-include` compile option, which the sim Makefile still uses; PlatformIO's
# ESP-IDF builder silently drops that option, and the C signer then linked
# against trezor-crypto's same-named functions with no error at all.
# zc_xcheck.c (the cross-check against the vendored arithmetic) is a host
# test only and is not built here.
set(ZCV vendor/libzcash-ironwood-c)

file(GLOB zc_units "${CMAKE_CURRENT_LIST_DIR}/idf/*.c")

idf_component_register(
    SRCS ${zc_units} "zc_sinsemilla.S"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${ZCV}/include" "${ZCV}/src"
    # trezor-crypto only for idf/zc_unit_zc_secp.c (transparent inputs).
    PRIV_REQUIRES esp_hw_support trezor-crypto)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=gnu11
    # The vendored sources are compiled as they are, warts included.
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
# zc_sinsemilla.S embeds the vendored S table with .incbin.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:ASM>:-Wa,-I${COMPONENT_DIR}/${ZCV}/src>)
