/* Build unit for zc_secp.c, the one zsign-c source that calls trezor-crypto
 * (BIP 32 and ECDSA for transparent inputs). Unlike every other unit here it
 * does NOT include vendor/zc_vendor_prefix.h: the prefix would bind these
 * calls to the vendored library's zcv_* copies, whose bignum is not
 * trezor-crypto's. scripts/check-zsign-c-symbols.sh allows trezor-crypto
 * references from this object only. */
#include "../zc_secp.c"
