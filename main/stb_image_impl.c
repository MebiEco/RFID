#include "esp_heap_caps.h"

/* -----------------------------------------------------------------------
 * Redirect stb_image allocs vào PSRAM (SPIRAM) để tránh dùng internal RAM
 * khi decode JPEG. Cần board có PSRAM và CONFIG_SPIRAM=y trong sdkconfig.
 * Nếu alloc SPIRAM thất bại (board ko PSRAM), tự fallback về internal RAM.
 * --------------------------------------------------------------------- */
static void *stbi_psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = heap_caps_malloc(sz, MALLOC_CAP_DEFAULT);
    }
    return p;
}

static void *stbi_psram_realloc(void *ptr, size_t newsz)
{
    /* Thử SPIRAM trước; nếu không được, fallback DEFAULT */
    void *p = heap_caps_realloc(ptr, newsz, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = heap_caps_realloc(ptr, newsz, MALLOC_CAP_DEFAULT);
    }
    return p;
}

static void stbi_psram_free(void *ptr)
{
    heap_caps_free(ptr);
}

#define STBI_MALLOC(sz)         stbi_psram_malloc(sz)
#define STBI_REALLOC(p, newsz)  stbi_psram_realloc(p, newsz)
#define STBI_FREE(p)            stbi_psram_free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
