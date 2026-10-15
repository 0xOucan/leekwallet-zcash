/* LeekWallet Zcash, C signer: randomness for host builds (sim tests).
 *
 * zc_platform_random feeds the RedPallas nonce (80 bytes of T per
 * signature). random_buffer()/random32() are the vendored library's hooks,
 * renamed zcv_* by zc_vendor_prefix.h; nothing on the signing path calls
 * them, but objects the linker keeps reference them. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "zc_crypto.h"

void zc_platform_random(uint8_t *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) abort();
    fclose(f);
}

void random_buffer(uint8_t *buf, size_t len)
{
    zc_platform_random(buf, len);
}

uint32_t random32(void)
{
    uint32_t v;
    zc_platform_random((uint8_t *)&v, sizeof v);
    return v;
}
