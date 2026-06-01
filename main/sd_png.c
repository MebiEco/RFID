#include "sd_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

#include "esp_attr.h"

#include "board_pins.h"
#include "lcd_color.h"
#include "lcd_ui.h"
#include "sd_card.h"
#include "stb_image.h"

/** JPEG toi da doc vao RAM — anh full man thuong < ~200 KB. */
#define SD_PNG_MAX_FILE_BYTES (192 * 1024)

static const char *TAG = "sd_png";

/**
 * Gioi han pixel sau giai ma (w*h). 320x240x3 — dung PSRAM.
 * Hien thi: scale vua khung (contain), khong cat anh; vien den neu ty le khac.
 */
#define MAX_DECODE_PIXELS ((size_t)BOARD_LCD_H_RES * (size_t)BOARD_LCD_V_RES)

/**
 * Chi pixel tu JPEG: co the doi R<->B neu file/coord decode lech.
 * Nen letterbox / mau co dinh: dung BOARD_RGB565_FROM888 truc tiep (giong lcd_ui UI_BG),
 * khong qua ham nay — neu khong vien chua anh se lech mau (vd. xanh la) khi SWAP_RB bat.
 */
static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
#if BOARD_SD_IMAGE_SWAP_RB_CHANNELS
    uint8_t t = r;
    r = b;
    b = t;
#endif
    return BOARD_RGB565_FROM888(r, g, b);
}

/** Lay RGB tai (sx,sy) bang noi suy song tuyen — giam rang cua khi thu nho. */
static void sample_rgb_bilinear(const uint8_t *rgba, int w, int h, int ch, float sx, float sy, uint8_t *ro,
                                uint8_t *go, uint8_t *bo)
{
    const float mx = (float)(w > 1 ? w - 1 : 0);
    const float my = (float)(h > 1 ? h - 1 : 0);
    if (sx < 0.f) {
        sx = 0.f;
    }
    if (sy < 0.f) {
        sy = 0.f;
    }
    if (sx > mx) {
        sx = mx;
    }
    if (sy > my) {
        sy = my;
    }
    int x0 = (int)floorf(sx);
    int y0 = (int)floorf(sy);
    int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
    int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;
    float w00 = (1.f - fx) * (1.f - fy);
    float w10 = fx * (1.f - fy);
    float w01 = (1.f - fx) * fy;
    float w11 = fx * fy;

#define CH_AT(xx, yy, c) ((float)rgba[((yy) * w + (xx)) * ch + (c)])

    float r = CH_AT(x0, y0, 0) * w00 + CH_AT(x1, y0, 0) * w10 + CH_AT(x0, y1, 0) * w01 + CH_AT(x1, y1, 0) * w11;
    float g = CH_AT(x0, y0, 1) * w00 + CH_AT(x1, y0, 1) * w10 + CH_AT(x0, y1, 1) * w01 + CH_AT(x1, y1, 1) * w11;
    float b = CH_AT(x0, y0, 2) * w00 + CH_AT(x1, y0, 2) * w10 + CH_AT(x0, y1, 2) * w01 + CH_AT(x1, y1, 2) * w11;

#undef CH_AT

    if (ch >= 4) {
        float a00 = (float)rgba[(y0 * w + x0) * ch + 3];
        float a10 = (float)rgba[(y0 * w + x1) * ch + 3];
        float a01 = (float)rgba[(y1 * w + x0) * ch + 3];
        float a11 = (float)rgba[(y1 * w + x1) * ch + 3];
        float a = a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
        if (a < 255.f) {
            r = r * a / 255.f;
            g = g * a / 255.f;
            b = b * a / 255.f;
        }
    }

    *ro = (uint8_t)(r + 0.5f);
    *go = (uint8_t)(g + 0.5f);
    *bo = (uint8_t)(b + 0.5f);
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

    /* Thu nho vua khung (contain): khong cat anh; vien = lcd_color_letterbox_bus() (mac dinh = UI_BG). */
    int sw = 0;
    int sh = 0;
    const int64_t w_th = (int64_t)w * (int64_t)tgt_h;
    const int64_t h_tw = (int64_t)h * (int64_t)tgt_w;
    if (w_th > h_tw) {
        sw = tgt_w;
        sh = (int)((h_tw + (int64_t)w / 2) / (int64_t)w);
    } else {
        sh = tgt_h;
        sw = (int)((w_th + (int64_t)h / 2) / (int64_t)h);
    }
    if (sw < 1) {
        sw = 1;
    }
    if (sh < 1) {
        sh = 1;
    }
    if (sw > tgt_w) {
        sw = tgt_w;
    }
    if (sh > tgt_h) {
        sh = tgt_h;
    }
    const int off_x = (tgt_w - sw) / 2;
    const int off_y = (tgt_h - sh) / 2;

    /* Viền = nền UI (lcd_color.h), không qua rgb888_to_rgb565/SWAP_RB — pixel ảnh JPEG vẫn qua rgb888_to_rgb565. */
    const uint16_t letter_pix = lcd_color_letterbox_bus();

    /* Buffer RGB565: ưu tiên SPIRAM để tiết kiệm RAM nội bộ cho Azure TLS. */
    const size_t fb_bytes = total_px * sizeof(uint16_t);
    uint16_t *fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM);
    if (!fb) {
        fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!fb) {
        ESP_LOGE(TAG, "draw_rgba_scaled_at: het RAM cho framebuffer (%u bytes)", (unsigned)fb_bytes);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < total_px; i++) {
        fb[i] = letter_pix;
    }

    const int ch = src_channels;

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
            if (sx_map < off_x || sx_map >= off_x + sw || sy_map < off_y || sy_map >= off_y + sh) {
                continue;
            }
            /* Tọa độ tâm pixel đích → không gian ảnh gốc (bilinear). */
            float sx = ((float)(sx_map - off_x) + 0.5f) * (float)w / (float)sw - 0.5f;
            float sy = ((float)(sy_map - off_y) + 0.5f) * (float)h / (float)sh - 0.5f;

            uint8_t r = 0, g = 0, b = 0;
            sample_rgb_bilinear(rgba, w, h, ch, sx, sy, &r, &g, &b);
            uint16_t color = rgb888_to_rgb565(r, g, b);

            fb[(size_t)y * tgt_w + (size_t)dx] = lcd_color_to_bus(color);
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
    int iw = 0, ih = 0, ic = 0;
    if (stbi_info_from_memory(raw_buf, (int)got, &iw, &ih, &ic)) {
        ESP_LOGI(TAG, "%s: anh goc %dx%d, kenh=%d", path, iw, ih, ic);
        if (ic == 1) {
            ESP_LOGW(TAG, "JPEG 1 kenh (den trang) — luu lai anh RGB neu can mau");
        }
    }

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
