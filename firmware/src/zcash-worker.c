/**
 * Zcash worker task. See zcash-worker.h.
 */
#include "zcash-worker.h"

#ifdef LEEK_HOST_TEST

bool zcash_worker_start(void) { return true; }

bool zcash_worker_run(ZcashJobFn fn, void *ctx)
{
    if (!fn) return false;
    fn(ctx);
    return true;
}

#else

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "zworker";

/* Longest a single job may take: a multi-action check on hardware is seconds,
 * not minutes. A worker that does not answer by then is treated as failed. */
#define ZCASH_JOB_TIMEOUT_MS 120000

static TaskHandle_t      task;
static SemaphoreHandle_t gate;     /* one caller at a time */
static SemaphoreHandle_t start;    /* caller -> worker */
static SemaphoreHandle_t done;     /* worker -> caller */
static ZcashJobFn        job_fn;
static void             *job_ctx;

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(start, portMAX_DELAY) != pdTRUE) continue;
        if (job_fn) job_fn(job_ctx);
        job_fn = NULL;
        job_ctx = NULL;
        ESP_LOGD(TAG, "job done, stack left %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        xSemaphoreGive(done);
    }
}

bool zcash_worker_start(void)
{
    if (task) return true;
    gate = xSemaphoreCreateMutex();
    start = xSemaphoreCreateBinary();
    done = xSemaphoreCreateBinary();
    if (!gate || !start || !done) return false;
    /* Priority 4, the protocol tasks' level: the UI (5) and the button poller
     * (6) stay responsive while a check runs. Internal RAM: the stack holds
     * key material during derivation and signing. */
    if (xTaskCreate(worker, "zcash", ZCASH_WORKER_STACK, NULL, 4, &task) != pdPASS) {
        task = NULL;
        ESP_LOGE(TAG, "could not create the Zcash worker (%u B stack)", ZCASH_WORKER_STACK);
        return false;
    }
    return true;
}

bool zcash_worker_run(ZcashJobFn fn, void *ctx)
{
    if (!fn || !task) return false;
    if (xSemaphoreTake(gate, pdMS_TO_TICKS(ZCASH_JOB_TIMEOUT_MS)) != pdTRUE) return false;
    job_fn = fn;
    job_ctx = ctx;
    xSemaphoreGive(start);
    bool ok = xSemaphoreTake(done, pdMS_TO_TICKS(ZCASH_JOB_TIMEOUT_MS)) == pdTRUE;
    if (!ok) ESP_LOGE(TAG, "job timed out");
    xSemaphoreGive(gate);
    return ok;
}

#endif
