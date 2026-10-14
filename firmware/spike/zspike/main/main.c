/* Phase 0.4 spike: time the Orchard operations the signer needs. */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zsign_rust.h"
#include "pczt_blob.h"

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

    /* Full signer path on a real NU6.3 PCZT. */
    static zs_summary sum;
    static zs_sig sigs[ZS_MAX_SIGS];
    size_t hb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int64_t t0 = esp_timer_get_time();
    int32_t err = 0;
    zs_ctx *ctx = zs_pczt_new(PCZT_BLOB, sizeof PCZT_BLOB, &err);
    int64_t t1 = esp_timer_get_time();
    int32_t rc = ctx ? zs_pczt_check(ctx, PCZT_SEED, 0, ZS_NET_TEST, &sum) : err;
    int64_t t2 = esp_timer_get_time();
    uint32_t n = 0;
    int32_t rs = rc == 0 ? zs_pczt_sign(ctx, PCZT_SEED, 0, ZS_NET_TEST, sigs, ZS_MAX_SIGS, &n) : rc;
    int64_t t3 = esp_timer_get_time();
    int vok = 0;
    for (uint32_t i = 0; i < n; i++) vok += zs_pczt_verify_sig(ctx, &sigs[i]) == 0;
    char sh[65];
    for (int i = 0; i < 32; i++) sprintf(sh + 2 * i, "%02x", sum.sighash[i]);
    printf("ZSPIKE pczt bytes=%u parse_us=%" PRId64 " check_us=%" PRId64 " sign_us=%" PRId64 "\n",
           (unsigned)sizeof PCZT_BLOB, t1 - t0, t2 - t1, t3 - t2);
    printf("ZSPIKE pczt check=%d sign=%d sigs=%u verified=%d fee=%llu outputs=%u\n",
           (int)rc, (int)rs, (unsigned)n, vok, (unsigned long long)sum.fee, sum.n_outputs);
    printf("ZSPIKE pczt sighash %s\n", strcmp(sh, PCZT_SIGHASH) == 0 ? "MATCH" : "MISMATCH");
    printf("ZSPIKE pczt fee %s\n", sum.fee == PCZT_FEE ? "MATCH" : "MISMATCH");
    zs_pczt_free(ctx);
    printf("ZSPIKE pczt heap_before=%u heap_min_ever=%u stack_hwm=%u\n", (unsigned)hb,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    printf("ZSPIKE done\n");
}
