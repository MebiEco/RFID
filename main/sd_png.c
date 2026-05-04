#include "sd_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

#include "esp_attr.h"

#include "board_pins.h"
#include "lcd_ui.h"
#include "sd_card.h"
#include "stb_image.h"

/** JPEG toi da doc vao RAM — anh 320x480 nen thuong < ~200 KB. */
#define SD_PNG_MAX_FILE_BYTES (192 * 1024)

static const char *TAG = "sd_png";

/**
 * Gioi han pixel sau giai ma (w*h). 320x480x3 ~ 450KB RGB — dung PSRAM.
 * Hien thi: zoom vung giua, giu dung ty le 64:80.
 */
#define MAX_DECODE_PIXELS (320 * 480)

static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8U) << 8) | ((uint16_t)(g & 0xFCU) << 3) | ((uint16_t)b >> 3));
}

/** Cùng endianness bus SPI như lcd_ui (ILI9488 RGB565) */
static inline uint16_t rgb565_to_panel_bus(uint16_t c)
{
#if BOARD_LCD_RGB565_SWAP_BYTES
    return __builtin_bswap16(c);
#else
    return c;
#endif
}

/**
 * Full frame ~41KB; canh 32 byte — DMA SPI ESP32-S3 on dinh hon, giam vân nhòe.
 */
/* Framebuffer được cấp phát từ PSRAM mỗi lần vẽ — không chiếm DRAM tĩnh (~41 KB). */

static esp_err_t draw_rgba_scaled_at(const uint8_t *rgba, int w, int h, int src_channels, int tgt_x, int tgt_y, int tgt_w, int tgt_h)
{
    esp_lcd_panel_handle_t panel = lcd_ui_get_panel();
    if (!panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (w < 1 || h < 1 || tgt_w < 1 || tgt_h < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total_px = (size_t)tgt_w * (size_t)tgt_h;

    /* Crop giua anh: giu ty le tgt_w:tgt_h, bo vien (giong "cover" nhung zoom giua). */
    int crop_x, crop_y, crop_w, crop_h;
    const int64_t wh_tgt = (int64_t)w * (int64_t)tgt_h;
    const int64_t hw_tgt = (int64_t)h * (int64_t)tgt_w;
    if (wh_tgt > hw_tgt) {
        crop_h = h;
        crop_w = (int)(hw_tgt / (int64_t)tgt_h);
        if (crop_w < 1) {
            crop_w = 1;
        }
        if (crop_w > w) {
            crop_w = w;
        }
        crop_x = (w - crop_w) / 2;
        crop_y = 0;
    } else {
        crop_w = w;
        crop_h = (int)(wh_tgt / (int64_t)tgt_w);
        if (crop_h < 1) {
            crop_h = 1;
        }
        if (crop_h > h) {
            crop_h = h;
        }
        crop_x = 0;
        crop_y = (h - crop_h) / 2;
    }

    /* DMA-capable: SPI LCD không chấp nhận buffer chỉ SPIRAM (không DMA) — lỗi tx_color queue */
    uint16_t *fb = heap_caps_malloc(total_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!fb) {
        fb = heap_caps_malloc(total_px * sizeof(uint16_t), MALLOC_CAP_DMA);
    }
    if (!fb) {
        ESP_LOGW(TAG, "DMA fb alloc failed — thu PSRAM only");
        fb = heap_caps_malloc(total_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ESP_LOGE(TAG, "draw_rgba_scaled_at: het RAM cho framebuffer (%u bytes)",
                 (unsigned)(total_px * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < tgt_h; y++) {
#if BOARD_SD_IMAGE_FLIP_Y
        int sy_map = tgt_h - 1 - y;
#else
        int sy_map = y;
#endif
        for (int dx = 0; dx < tgt_w; dx++) {
#if BOARD_SD_IMAGE_FLIP_X
            int sx_map = tgt_w - 1 - dx;
#else
            int sx_map = dx;
#endif
            int sx = crop_x + (sx_map * crop_w) / tgt_w;
            int sy = crop_y + (sy_map * crop_h) / tgt_h;
            if (sx < 0) {
                sx = 0;
            }
            if (sy < 0) {
                sy = 0;
            }
            if (sx >= w) {
                sx = w - 1;
            }
            if (sy >= h) {
                sy = h - 1;
            }
            const uint8_t *p = rgba + (size_t)(sy * w + sx) * (size_t)src_channels;
            uint8_t r = p[0];
            uint8_t g = p[1];
            uint8_t b = p[2];
            if (src_channels >= 4) {
                uint8_t a = p[3];
                if (a < 255) {
                    r = (uint8_t)((uint16_t)r * a / 255);
                    g = (uint8_t)((uint16_t)g * a / 255);
                    b = (uint8_t)((uint16_t)b * a / 255);
                }
            }
            fb[(size_t)y * tgt_w + (size_t)dx] =
                rgb565_to_panel_bus(rgb888_to_rgb565(r, g, b));
        }
    }

    int real_tgt_x = BOARD_LCD_H_RES - tgt_w - tgt_x;
    esp_err_t e = lcd_ui_draw_bitmap_sync(panel, real_tgt_x, tgt_y, real_tgt_x + tgt_w, tgt_y + tgt_h, fb);
    heap_caps_free(fb);
    return e;
}

esp_err_t sd_png_show_image_at(const char *path, int tgt_x, int tgt_y, int tgt_w, int tgt_h)
{
    if (!sd_card_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* -------------------------------------------------------
     * BƯỚC 1: Đọc raw bytes từ SD vào heap buffer (giữ lock)
     * Chỉ file I/O — không decode, không vẽ LCD.
     * ------------------------------------------------------- */
    sd_card_lock();

    FILE *f = fopen(path, "rb");
    if (!f) {
        sd_card_unlock();
        ESP_LOGW(TAG, "fopen %s failed", path);
        return ESP_FAIL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        sd_card_unlock();
        return ESP_FAIL;
    }
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0 || file_size > (long)SD_PNG_MAX_FILE_BYTES) {
        ESP_LOGW(TAG, "%s: kich thuoc %ld B vuot gioi han %d B", path, file_size, SD_PNG_MAX_FILE_BYTES);
        fclose(f);
        sd_card_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    /* Cấp buffer từ PSRAM; fallback internal RAM */
    uint8_t *raw_buf = heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM);
    if (!raw_buf) {
        raw_buf = heap_caps_malloc((size_t)file_size, MALLOC_CAP_DEFAULT);
    }
    if (!raw_buf) {
        ESP_LOGE(TAG, "Het RAM cho raw_buf (%ld B)", file_size);
        fclose(f);
        sd_card_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t got = fread(raw_buf, 1, (size_t)file_size, f);
    fclose(f);
    sd_card_unlock(); /* <<< GIẢI PHÓNG SD LOCK NGAY SAU KHI ĐỌC XONG */

    if ((long)got != file_size) {
        ESP_LOGW(TAG, "fread %s: doc %u/%ld B", path, (unsigned)got, file_size);
        heap_caps_free(raw_buf);
        return ESP_FAIL;
    }

    /* -------------------------------------------------------
     * BƯỚC 2: Decode JPEG từ buffer (NGOÀI lock — không cần SD)
     * stbi_load_from_memory tốn CPU ~0.5-2s nhưng không đụng SD.
     * ------------------------------------------------------- */
    int w = 0, h = 0, ch = 0;
    unsigned char *pixels = stbi_load_from_memory(raw_buf, (int)got, &w, &h, &ch, 3);
    heap_caps_free(raw_buf);

    if (!pixels) {
        ESP_LOGE(TAG, "stbi decode failed: %s", path);
        return ESP_FAIL;
    }

    if ((int64_t)w * (int64_t)h > (int64_t)MAX_DECODE_PIXELS) {
        stbi_image_free(pixels);
        return ESP_ERR_INVALID_SIZE;
    }

    /* BƯỚC 3: Vẽ lên LCD (NGOÀI lock — chỉ dùng SPI3/LCD, không SD) */
    esp_err_t ret = draw_rgba_scaled_at(pixels, w, h, 3, tgt_x, tgt_y, tgt_w, tgt_h);
    stbi_image_free(pixels);

    return ret;
}
