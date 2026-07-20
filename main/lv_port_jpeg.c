/**
 * Hiển thị JPEG trên LVGL (decode RGB888 → RGB565, giữ tỉ lệ trong khung max_w×max_h).
 */
#include "lv_port.h"
#include "lvgl.h"
#include "board_pins.h"
#include "lcd_color.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "stb_image.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "lv_port_jpeg";

static void img_delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    void *buf = lv_obj_get_user_data(obj);
    if (buf) {
        heap_caps_free(buf);
    }
}

bool lv_port_show_jpeg_file(void *parent, const char *path, int x, int y, int max_w, int max_h)
{
    lv_obj_t *par = parent ? (lv_obj_t *)parent : lv_scr_act();

    if (!path || !path[0] || max_w < 8 || max_h < 8) {
        return false;
    }

    int iw = 0, ih = 0, ch = 0;
    unsigned char *raw = stbi_load(path, &iw, &ih, &ch, 3);
    if (!raw || iw < 1 || ih < 1) {
        ESP_LOGW(TAG, "stbi_load failed: %s", path);
        if (raw) {
            stbi_image_free(raw);
        }
        return false;
    }

    int ow = iw;
    int oh = ih;
    if (ow > max_w || oh > max_h) {
        /* Thu nhỏ: nearest-neighbor — ảnh thẻ nhỏ, giữ nét hơn bilinear. */
        const int64_t num_w = (int64_t)iw * (int64_t)max_h;
        const int64_t num_h = (int64_t)ih * (int64_t)max_w;
        if (num_w >= num_h) {
            oh = max_h;
            ow = (int)((int64_t)iw * (int64_t)max_h / (int64_t)ih);
            if (ow < 1) {
                ow = 1;
            }
            if (ow > max_w) {
                ow = max_w;
            }
        } else {
            ow = max_w;
            oh = (int)((int64_t)ih * (int64_t)max_w / (int64_t)iw);
            if (oh < 1) {
                oh = 1;
            }
            if (oh > max_h) {
                oh = max_h;
            }
        }
    }

    const size_t px_bytes = (size_t)ow * (size_t)oh * sizeof(uint16_t);
    typedef struct {
        lv_img_dsc_t dsc;
        uint16_t pixels[];
    } img_mem_t;

    img_mem_t *m = heap_caps_malloc(sizeof(lv_img_dsc_t) + px_bytes, MALLOC_CAP_SPIRAM);
    if (!m) {
        m = heap_caps_malloc(sizeof(lv_img_dsc_t) + px_bytes, MALLOC_CAP_DEFAULT);
    }
    if (!m) {
        stbi_image_free(raw);
        ESP_LOGE(TAG, "OOM jpeg");
        return false;
    }

    uint16_t *dst = m->pixels;
    for (int yy = 0; yy < oh; yy++) {
        extern void lv_port_feed_wdt(void);
        lv_port_feed_wdt();

        const int sy = (oh > 1) ? (yy * (ih - 1)) / (oh - 1) : 0;
        for (int xx = 0; xx < ow; xx++) {
            const int sx = (ow > 1) ? (xx * (iw - 1)) / (ow - 1) : 0;
            const unsigned char *p = &raw[(sy * iw + sx) * 3];
#if BOARD_SD_IMAGE_SWAP_RB_CHANNELS
            dst[yy * ow + xx] = lcd_color_from888(p[2], p[1], p[0]);
#else
            dst[yy * ow + xx] = lcd_color_from888(p[0], p[1], p[2]);
#endif
        }
    }
    stbi_image_free(raw);

    m->dsc.header.always_zero = 0;
    m->dsc.header.w = (uint32_t)ow;
    m->dsc.header.h = (uint32_t)oh;
    m->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    m->dsc.data_size = px_bytes;
    m->dsc.data = (uint8_t *)m->pixels;

    lv_obj_t *img = lv_img_create(par);
    lv_img_set_src(img, &m->dsc);
    lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
    lv_obj_set_size(img, ow, oh);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_user_data(img, m);
    lv_obj_add_event_cb(img, img_delete_cb, LV_EVENT_DELETE, NULL);

    ESP_LOGI(TAG, "JPEG OK: %s src=%dx%d dst=%dx%d", path, iw, ih, ow, oh);
    return true;
}
