/* Phase 0.4 spike: time the Orchard operations the signer needs.
 * Phase 4: the same PCZT through the C backend (zsign-c) next to Rust, each
 * in a fresh task so its stack high-water mark is its own, and the two
 * summaries compared byte for byte (what BOTH mode will require). */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "zsign_rust.h"
#include "zsign_c.h"
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

#define BACKEND_STACK (64 * 1024)   /* as the firmware's Zcash worker */

static zs_summary s_sum[2];
static zs_sig s_sigs[2][ZS_MAX_SIGS];
static SemaphoreHandle_t s_done;

/* One backend through new / check / sign / verify on PCZT_BLOB. */
static void backend_task(void *arg)
{
    int c = (int)(intptr_t)arg;
    const char *name = c ? "c" : "rust";
    zs_summary *sum = &s_sum[c];
    zs_sig *sigs = s_sigs[c];
    size_t hb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int64_t t0 = esp_timer_get_time(), t_init = 0;
    if (c) {
        /* First use: S-table fingerprint and cached generators, then the
         * Poseidon known answer and three S entries recomputed. */
        int32_t st = zc_selftest();
        t_init = esp_timer_get_time() - t0;
        printf("ZSPIKE c selftest rc=%d us=%" PRId64 "\n", (int)st, t_init);
        t0 = esp_timer_get_time();
    }
    int32_t err = 0;
    zs_ctx *rctx = NULL;
    zc_ctx *cctx = NULL;
    if (c) cctx = zc_pczt_new(PCZT_BLOB, sizeof PCZT_BLOB, &err);
    else rctx = zs_pczt_new(PCZT_BLOB, sizeof PCZT_BLOB, &err);
    int64_t t1 = esp_timer_get_time();
    int32_t rc = c ? (cctx ? zc_pczt_check(cctx, PCZT_SEED, 0, ZS_NET_TEST, sum) : err)
                   : (rctx ? zs_pczt_check(rctx, PCZT_SEED, 0, ZS_NET_TEST, sum) : err);
    int64_t t2 = esp_timer_get_time();
    uint32_t n = 0;
    int32_t rs = rc;
    if (rc == 0)
        rs = c ? zc_pczt_sign(cctx, PCZT_SEED, 0, ZS_NET_TEST, sigs, ZS_MAX_SIGS, &n)
               : zs_pczt_sign(rctx, PCZT_SEED, 0, ZS_NET_TEST, sigs, ZS_MAX_SIGS, &n);
    int64_t t3 = esp_timer_get_time();
    int vok = 0;
    for (uint32_t i = 0; i < n; i++)
        vok += (c ? zc_pczt_verify_sig(cctx, &sigs[i]) : zs_pczt_verify_sig(rctx, &sigs[i])) == 0;
    int64_t t4 = esp_timer_get_time();
    char sh[65];
    for (int i = 0; i < 32; i++) sprintf(sh + 2 * i, "%02x", sum->sighash[i]);
    printf("ZSPIKE %s pczt bytes=%u parse_us=%" PRId64 " check_us=%" PRId64 " sign_us=%" PRId64
           " verify_us=%" PRId64 "\n", name, (unsigned)sizeof PCZT_BLOB, t1 - t0, t2 - t1, t3 - t2,
           t4 - t3);
    printf("ZSPIKE %s pczt check=%d sign=%d sigs=%u verified=%d fee=%llu outputs=%u\n", name,
           (int)rc, (int)rs, (unsigned)n, vok, (unsigned long long)sum->fee, sum->n_outputs);
    printf("ZSPIKE %s pczt sighash %s\n", name, strcmp(sh, PCZT_SIGHASH) == 0 ? "MATCH" : "MISMATCH");
    printf("ZSPIKE %s pczt fee %s\n", name, sum->fee == PCZT_FEE ? "MATCH" : "MISMATCH");
    if (c) zc_pczt_free(cctx);
    else zs_pczt_free(rctx);
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    printf("ZSPIKE %s pczt heap_before=%u heap_min_ever=%u stack_used=%u of %u\n", name,
           (unsigned)hb, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)(BACKEND_STACK - hwm), (unsigned)BACKEND_STACK);
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void run_backend(int c)
{
    if (!s_done) s_done = xSemaphoreCreateBinary();
    xTaskCreate(backend_task, c ? "zs_c" : "zs_rust", BACKEND_STACK, (void *)(intptr_t)c, 5, NULL);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

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

    /* Full signer path on a real NU6.3 PCZT, Rust then C, each in its own
     * task with a fresh 64 KiB stack. */
    run_backend(0);
    run_backend(1);
    printf("ZSPIKE both summary %s\n", memcmp(&s_sum[0], &s_sum[1], sizeof s_sum[0]) == 0 ? "MATCH" : "MISMATCH");
    printf("ZSPIKE done\n");
}
