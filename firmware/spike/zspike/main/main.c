/* Phase 0.4 spike: time the Orchard operations the signer needs. */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zsign_rust.h"

static void hex(const char *label, const uint8_t *b, size_t n)
{
    printf("%s=", label);
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

#define TIMED(label, expr) do {                                         \
    int64_t t0 = esp_timer_get_time();                                  \
    int32_t rc = (expr);                                                \
    int64_t dt = esp_timer_get_time() - t0;                             \
    printf("ZSPIKE %-8s rc=%" PRId32 " us=%" PRId64 "\n", label, rc, dt); \
} while (0)

void app_main(void)
{
    uint8_t seed[64], fvk[96], addr[43], sig[64], cmx[32], alpha[32] = {0}, sighash[32];
    for (int i = 0; i < 64; i++) seed[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) sighash[i] = (uint8_t)(0xA0 + i);
    alpha[0] = 5;

    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    printf("ZSPIKE start heap_free=%u min_ever=%u\n", (unsigned)heap0,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));

    TIMED("fvk", zs_spike_fvk(seed, 0, fvk));
    TIMED("address", zs_spike_address(seed, 0, addr));
    TIMED("sign", zs_spike_sign(seed, 0, alpha, sighash, sig));
    TIMED("cmx", zs_spike_cmx(seed, 100000, cmx));

    hex("ZSPIKE fvk", fvk, 96);
    hex("ZSPIKE addr", addr, 43);
    hex("ZSPIKE cmx", cmx, 32);
    hex("ZSPIKE sig", sig, 64);
    TIMED("verify", zs_spike_verify(seed, 0, alpha, sighash, sig));
    printf("ZSPIKE heap_min_ever=%u stack_hwm_words=%u\n",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    printf("ZSPIKE done\n");
}
