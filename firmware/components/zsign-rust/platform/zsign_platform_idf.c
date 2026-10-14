/* Platform glue the Rust crate links against (ESP-IDF build). */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"

void *zs_platform_alloc(size_t size, size_t align)
{
    if (align < 4) align = 4;
    return heap_caps_aligned_alloc(align, size, MALLOC_CAP_DEFAULT);
}

void zs_platform_free(void *ptr)
{
    heap_caps_free(ptr);
}

void zs_platform_random(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

void zs_platform_abort(const uint8_t *msg, size_t len)
{
    ESP_LOGE("zsign", "%.*s", (int)len, (const char *)msg);
    abort();
}
