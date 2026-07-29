/**
 * LVGL heap → PSRAM (fallback Internal nếu PSRAM hết).
 * Dùng qua CONFIG_LV_MEM_CUSTOM_INCLUDE — include sau khi lv_conf set malloc mặc định.
 */
#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static inline void *lv_psram_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p && size != 0) {
        p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    return p;
}

static inline void lv_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

#undef LV_MEM_CUSTOM_ALLOC
#undef LV_MEM_CUSTOM_FREE
#undef LV_MEM_CUSTOM_REALLOC
#define LV_MEM_CUSTOM_ALLOC   lv_psram_malloc
#define LV_MEM_CUSTOM_FREE    lv_psram_free
#define LV_MEM_CUSTOM_REALLOC lv_psram_realloc
