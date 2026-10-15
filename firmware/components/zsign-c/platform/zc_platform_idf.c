/* LeekWallet Zcash, C signer: randomness for ESP-IDF builds.
 *
 * esp_fill_random draws on the hardware RNG (true random while RF or the
 * bootloader entropy source is enabled; see docs/AUDIT-ENTROPY.md).
 * zc_platform_random feeds the RedPallas nonce; random_buffer()/random32()
 * are the vendored library's hooks, renamed zcv_* by zc_vendor_prefix.h, so
 * they never collide with trezor-crypto's. */
#include <stdint.h>
#include <stddef.h>
#include "esp_random.h"

#include "zc_crypto.h"

void zc_platform_random(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

void random_buffer(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

uint32_t random32(void)
{
    return esp_random();
}
