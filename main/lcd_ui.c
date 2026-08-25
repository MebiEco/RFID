/**
 * Lớp LCD tối thiểu: init panel, backlight, vẽ bitmap đồng bộ (LVGL flush), vài dòng text khi boot/lỗi.
 * Toàn bộ màn hình ứng dụng đã chuyển sang LVGL (`lv_port_*`).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#if __has_include("esp_cache.h")
#include "esp_cache.h"
#endif
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "board_pins.h"
#include "lcd_panel_config.h"
#include "lcd_color.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_ili9341_init_cmds_1.h"
#include "lcd_ui.h"
#include "ui_log_model.h"
#include "ui_card_cache.h"

#include "font8x8_basic.inc"

static const char *TAG = "lcd_ui";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_trans_done_sem;
static SemaphoreHandle_t s_lcd_draw_mux;

static bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd_trans_done_sem, &need_yield);
    return (need_yield == pdTRUE);
}

esp_err_t lcd_ui_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, const void *color_data)
{
    if (!panel || !s_lcd_trans_done_sem || !s_lcd_draw_mux) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lcd_draw_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Xóa token thừa sau lần timeout trước — tránh Take ngay mà không chờ DMA. */
    while (xSemaphoreTake(s_lcd_trans_done_sem, 0) == pdTRUE) {
    }

#if defined(ESP_CACHE_MSYNC_FLAG_DIR_C2M)
    if (color_data && esp_ptr_external_ram((void *)color_data)) {
        const size_t nbytes = (size_t)(x2 - x1) * (size_t)(y2 - y1) * sizeof(uint16_t);
#if defined(ESP_CACHE_MSYNC_FLAG_UNALIGNED)
        (void)esp_cache_msync((void *)color_data, nbytes,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#else
        (void)esp_cache_msync((void *)color_data, nbytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
    }
#endif

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, (void *)color_data);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_lcd_trans_done_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGW(TAG, "lcd draw timeout — SPI DMA callback missed");
            err = ESP_ERR_TIMEOUT;
        }
    } else {
        ESP_LOGW(TAG, "panel_draw_bitmap: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_lcd_draw_mux);
    return err;
}

static void lcd_ui_draw_chunk_wait(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, void *color_data)
{
    (void)lcd_ui_draw_bitmap_sync(panel, x1, y1, x2, y2, color_data);
}

/**
 * Chỉ bộ nhớ DMA — esp_lcd SPI queue_trans từ SPIRAM/internal không DMA →
 * "spi transmit (queue) color failed" / ramwr (đặc biệt khi WiFi/Azure chiếm heap).
 */
static uint16_t *lcd_ui_alloc_fb(size_t total_px)
{
    const size_t sz = total_px * sizeof(uint16_t);
    return heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

static esp_err_t lcd_bl_gpio_init(void)
{
    const gpio_num_t bl = BOARD_LCD_BL_GPIO;
    if (bl == GPIO_NUM_NC) {
        return ESP_OK;
    }
    gpio_reset_pin(bl);
    esp_err_t e = gpio_set_direction(bl, GPIO_MODE_OUTPUT);
    if (e != ESP_OK) {
        return e;
    }
    gpio_set_level(bl, BOARD_LCD_BL_ACTIVE_HIGH ? 0 : 1);
    return ESP_OK;
}

void lcd_ui_set_backlight(bool on)
{
    const gpio_num_t bl = BOARD_LCD_BL_GPIO;
    if (bl == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(bl, on ? (BOARD_LCD_BL_ACTIVE_HIGH ? 1 : 0) : (BOARD_LCD_BL_ACTIVE_HIGH ? 0 : 1));
}

#define RGB565(r, g, b) BOARD_RGB565_FROM888((r), (g), (b))

/** Trùng BOARD_UI_BG_* trong board_pins.h */
#define UI_BG       RGB565(BOARD_UI_BG_R, BOARD_UI_BG_G, BOARD_UI_BG_B)
#define UI_BLUE_H   RGB565(26, 92, 168)
#define UI_BLUE_TXT RGB565(18, 32, 52)
#define UI_MUTED    RGB565(95, 105, 118)
#define UI_RED      RGB565(200, 48, 58)
#define UI_WHITE    RGB565(255, 255, 255)
#define UI_SKY      UI_BLUE_H
#define UI_LEAF     UI_BLUE_H
#define UI_ORANGE   UI_BLUE_H
#define UI_ORG_LITE RGB565(245, 247, 250)
#define UI_SURFACE  RGB565(248, 249, 252)
#define UI_KEY_BG   RGB565(238, 241, 246)
#define UI_ROW_A    RGB565(248, 249, 252)
#define UI_ROW_B    RGB565(240, 243, 248)
#define UI_EDGE_HI  RGB565(255, 255, 255)
#define UI_EDGE_LO  RGB565(198, 204, 214)

/*
 * Hai buffer glyph luân phiên: tránh ghi đè pixel nếu lệnh SPI/DMA trước chưa đọc xong
 * (một buffer 8x8 dùng lại trong vòng lặc nhiều ký tự dễ vỡ chữ / nhiễu).
 */
static DRAM_ATTR uint16_t s_glyph[2][8 * 8] __attribute__((aligned(16)));
static uint8_t s_glyph_buf;

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    /* font8x8_basic chi co 128 glyph (0..127); byte UTF-8 >= 0x80 se vuot mang. */
    uint8_t idx = (uint8_t)c;
    if (idx >= 128u) {
        idx = (uint8_t)'?';
    }
    const uint16_t fgx = lcd_color_to_bus(fg);
    const uint16_t bgx = lcd_color_to_bus(bg);

    s_glyph_buf ^= 1u;
    uint16_t *g = s_glyph[s_glyph_buf];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            g[row * 8 + col] = (bits & (1 << col)) ? fgx : bgx;
        }
    }
    (void)lcd_ui_draw_bitmap_sync(s_panel, x, y, x + 8, y + 8, g);
}

/**
 * Sao chep vao out: ASCII giu nguyen; moi ky tu UTF-8 (nhieu byte) -> mot '?'.
 * Tranh vua vuot font vua ton nhieu cot cho tung byte 0x80..0xFF.
 */
static size_t copy_for_lcd_font(const char *s, char *out, size_t out_sz)
{
    size_t j = 0;
    if (!s || out_sz == 0) {
        if (out && out_sz) {
            out[0] = '\0';
        }
        return 0;
    }
    const size_t max_chars = out_sz - 1;
    for (size_t i = 0; s[i] && j < max_chars;) {
        unsigned char b = (unsigned char)s[i];
        if (b < 0x80u) {
            out[j++] = (char)b;
            i++;
            continue;
        }
        out[j++] = '?';
        i++;
        while (s[i] && ((unsigned char)s[i] & 0xc0u) == 0x80u) {
            i++;
        }
    }
    out[j] = '\0';
    return j;
}

/** Xóa một dải ngang (full chiều rộng) bằng màu nền — giảm “chấm”/ghost chữ cũ. */
static DRAM_ATTR uint16_t s_lcd_line_buf[BOARD_LCD_H_RES * 4] __attribute__((aligned(16)));

static void clear_hband_height(int y, int h, uint16_t bg_rgb565)
{
    if (!s_panel || y < 0 || y + h > BOARD_LCD_V_RES) {
        return;
    }
    const uint16_t bx = lcd_color_to_bus(bg_rgb565);
    for (int row = 0; row < h; ) {
        int limit = h - row;
        if (limit > 4) {
            limit = 4;
        }
        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * limit; i++) {
            s_lcd_line_buf[i] = bx;
        }
        (void)lcd_ui_draw_bitmap_sync(s_panel, 0, y + row, BOARD_LCD_H_RES, y + row + limit, s_lcd_line_buf);
        row += limit;
    }
}

static void draw_char_scaled(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    uint8_t idx = (uint8_t)c;
    if (idx >= 128u) idx = (uint8_t)'?';
    const uint16_t fgx = lcd_color_to_bus(fg);
    const uint16_t bgx = lcd_color_to_bus(bg);

    if (scale == 1) {
        draw_char(x, y, c, fg, bg);
        return;
    }

    /* Tai dung s_lcd_line_buf (DRAM/DMA) — tranh giu them ~2.3KB Internal cho OTA text. */
    const size_t need_px = (size_t)(8 * scale) * (size_t)(8 * scale);
    if (need_px > (sizeof(s_lcd_line_buf) / sizeof(s_lcd_line_buf[0]))) {
        return;
    }
    uint16_t *g = s_lcd_line_buf;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            uint16_t color = (bits & (1 << col)) ? fgx : bgx;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    g[(row * scale + dy) * (8 * scale) + (col * scale + dx)] = color;
                }
            }
        }
    }
    (void)lcd_ui_draw_bitmap_sync(s_panel, x, y, x + 8 * scale, y + 8 * scale, g);
}

static void draw_str_centered_scaled(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (!s) return;
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    clear_hband_height(y, 8 * scale, bg);
    char tmp[LCD_UI_MAX_CHARS_PER_LINE + 1];
    size_t n = copy_for_lcd_font(s, tmp, sizeof(tmp));
    if (n > (size_t)LCD_UI_MAX_CHARS_PER_LINE) {
        n = (size_t)LCD_UI_MAX_CHARS_PER_LINE;
        tmp[n] = '\0';
    }
    if (y < 0 || y + 8 * scale > BOARD_LCD_V_RES) return;

    const int text_px = (int)(n * 8u * scale);
    int x = ((int)BOARD_LCD_H_RES - text_px) / 2;
    if (x < 0) x = 0;
    size_t max_n = (size_t)((BOARD_LCD_H_RES - x) / (8 * scale));
    if (max_n == 0) return;
    if (n > max_n) n = max_n;

    for (size_t i = 0; i < n; i++) {
        int cx = x + (int)i * (8 * scale);
        if (cx + 8 * scale > BOARD_LCD_H_RES) break;
        draw_char_scaled(cx, y, tmp[i], fg, bg, scale);
    }
}

static void draw_str_centered(int y, const char *s, uint16_t fg, uint16_t bg)
{
    draw_str_centered_scaled(y, s, fg, bg, 1);
}

esp_err_t lcd_ui_init(void)
{
    if (!s_lcd_trans_done_sem) {
        s_lcd_trans_done_sem = xSemaphoreCreateBinary();
    }
    if (!s_lcd_draw_mux) {
        s_lcd_draw_mux = xSemaphoreCreateMutex();
    }

    esp_err_t e = lcd_bl_gpio_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "lcd_bl_gpio_init: %s", esp_err_to_name(e));
        return e;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    bool lcd_spi_inited = false;

    /* Chi can chunk LVGL/JPEG — max = chunk bytes (khong ep 8KB). */
#ifndef BOARD_LCD_SPI_MAX_TRANSFER
#define BOARD_LCD_SPI_MAX_TRANSFER \
    ((size_t)BOARD_LCD_H_RES * (size_t)BOARD_LCD_SPI_CHUNK_LINES * 2u)
#endif
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
#if BOARD_LCD_SHARE_SPI2_WITH_SD
        .miso_io_num = BOARD_LCD_MISO_GPIO,
#elif BOARD_LCD_SPI_USE_MISO
        .miso_io_num = BOARD_LCD_MISO_GPIO,
#else
        .miso_io_num = -1,
#endif
        .sclk_io_num = BOARD_LCD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)BOARD_LCD_SPI_MAX_TRANSFER,
    };
    e = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    lcd_spi_inited = (e == ESP_OK);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return e;
    }

    const lcd_panel_profile_t *lcd_prof = lcd_panel_get_active();

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .pclk_hz = lcd_prof->pclk_hz,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = BOARD_LCD_SPI_MODE,
        .trans_queue_depth = BOARD_LCD_SPI_TRANS_QUEUE_DEPTH,
        .on_color_trans_done = lcd_trans_done_cb,
    };
    e = esp_lcd_new_panel_io_spi(BOARD_LCD_SPI_HOST, &io_cfg, &io);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(e));
        if (lcd_spi_inited) {
            spi_bus_free(BOARD_LCD_SPI_HOST);
        }
        return e;
    }

    static const ili9341_vendor_config_t s_ili9341_vendor = {
        .init_cmds = ili9341_lcd_init_vendor,
        .init_cmds_size = sizeof(ili9341_lcd_init_vendor) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
#if BOARD_LCD_COLOR_PIPELINE_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
        .vendor_config = lcd_prof->use_vendor_init ? (void *)&s_ili9341_vendor : NULL,
    };
    e = esp_lcd_new_panel_ili9341(io, &panel_cfg, &s_panel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel: %s", esp_err_to_name(e));
        esp_lcd_panel_io_del(io);
        if (lcd_spi_inited) {
            spi_bus_free(BOARD_LCD_SPI_HOST);
        }
        return e;
    }

    e = esp_lcd_panel_reset(s_panel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset: %s", esp_err_to_name(e));
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(io);
        if (lcd_spi_inited) {
            spi_bus_free(BOARD_LCD_SPI_HOST);
        }
        return e;
    }
    e = esp_lcd_panel_init(s_panel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init: %s", esp_err_to_name(e));
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(io);
        if (lcd_spi_inited) {
            spi_bus_free(BOARD_LCD_SPI_HOST);
        }
        return e;
    }


#if BOARD_LCD_SWAP_XY_AFTER_INIT
    e = esp_lcd_panel_swap_xy(s_panel, true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_swap_xy: %s", esp_err_to_name(e));
    }
#endif
    e = esp_lcd_panel_mirror(s_panel, lcd_prof->mirror_x, lcd_prof->mirror_y);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_mirror: %s", esp_err_to_name(e));
    }
    (void)esp_lcd_panel_set_gap(s_panel, 0, 0);

    (void)esp_lcd_panel_invert_color(s_panel, lcd_prof->invert_color);
    (void)esp_lcd_panel_disp_on_off(s_panel, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_ui_set_backlight(true);
    vTaskDelay(pdMS_TO_TICKS(BOARD_LCD_POST_DISPON_DELAY_MS));

#if BOARD_LCD_BL_GPIO == GPIO_NUM_NC
    ESP_LOGI(TAG,
             "LCD BL: không dùng GPIO (NC) — đèn nền thường nối sẵn trên module; nếu màn TỐI thì nối BL/3V3.");
#endif

    /* Nền sau init — khớp UI_BG (xám sáng) */
    uint16_t bg = lcd_color_to_bus(RGB565(BOARD_LCD_STARTUP_CLEAR_R, BOARD_LCD_STARTUP_CLEAR_G, BOARD_LCD_STARTUP_CLEAR_B));
    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) {
        ESP_LOGE(TAG, "lcd_ui_alloc_fb (chunk)");
        esp_lcd_panel_del(s_panel);
        esp_lcd_panel_io_del(io);
        if (lcd_spi_inited) {
            spi_bus_free(BOARD_LCD_SPI_HOST);
        }
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < chunk_px; i++) {
        fb[i] = bg;
    }
    for (int y = 0; y < BOARD_LCD_V_RES; y += lines_per_chunk) {
        int y_end = y + lines_per_chunk;
        if (y_end > BOARD_LCD_V_RES) y_end = BOARD_LCD_V_RES;
        esp_err_t dr = lcd_ui_draw_bitmap_sync(s_panel, 0, y, BOARD_LCD_H_RES, y_end, fb);
        if (dr != ESP_OK) {
            ESP_LOGE(TAG, "startup clear row %d-%d: %s", y, y_end, esp_err_to_name(dr));
            heap_caps_free(fb);
            esp_lcd_panel_del(s_panel);
            esp_lcd_panel_io_del(io);
            if (lcd_spi_inited) {
                spi_bus_free(BOARD_LCD_SPI_HOST);
            }
            return dr;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    heap_caps_free(fb);

    ESP_LOGI(TAG,
             "LCD profile=%s %dx%d pclk=%u invert=%d mirror=%d,%d vendor_init=%d bl=%s",
             lcd_prof->name, BOARD_LCD_H_RES, BOARD_LCD_V_RES, (unsigned)lcd_prof->pclk_hz,
             lcd_prof->invert_color, lcd_prof->mirror_x, lcd_prof->mirror_y, lcd_prof->use_vendor_init,
             (BOARD_LCD_BL_GPIO == GPIO_NUM_NC) ? "NC" : "on");
    return ESP_OK;
}

esp_lcd_panel_handle_t lcd_ui_get_panel(void)
{
    return s_panel;
}

void lcd_ui_show_centered(const char *text)
{
    if (!s_panel || !text) {
        return;
    }
    uint16_t bg = UI_BG;
    uint16_t fg = UI_SKY;
    int y = (BOARD_LCD_V_RES - 8) / 2; 
    draw_str_centered(y, text, fg, bg);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void lcd_ui_show_lines(const char *line1, const char *line2)
{
    if (!s_panel) {
        return;
    }
    uint16_t bg = UI_BG;
    uint16_t fg = UI_BLUE_TXT;
    const int line_h = 8;
    const int gap = 4;
    const int block_h = line_h + gap + line_h;
    int y1 = (BOARD_LCD_V_RES - block_h) / 2;
    int y2 = y1 + line_h + gap;
    if (y1 < 0) {
        y1 = 0;
    }
    if (y2 + line_h > BOARD_LCD_V_RES) {
        y2 = BOARD_LCD_V_RES - line_h;
        y1 = y2 - gap - line_h;
        if (y1 < 0) {
            y1 = 0;
        }
    }
    draw_str_centered(y1, line1 ? line1 : "", fg, bg);
    draw_str_centered(y2, line2 ? line2 : "", fg, bg);
}

void lcd_ui_clear_screen(void)
{
    if (!s_panel) {
        return;
    }
    uint16_t bg = lcd_color_to_bus(UI_BG);
    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) {
        return;
    }
    for (size_t i = 0; i < chunk_px; i++) {
        fb[i] = bg;
    }
    for (int y = 0; y < BOARD_LCD_V_RES; y += lines_per_chunk) {
        int y_end = y + lines_per_chunk;
        if (y_end > BOARD_LCD_V_RES) y_end = BOARD_LCD_V_RES;
        lcd_ui_draw_chunk_wait(s_panel, 0, y, BOARD_LCD_H_RES, y_end, fb);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    heap_caps_free(fb);
}

void lcd_ui_invalidate_log_cache(void) { ui_log_model_invalidate(); }

void lcd_ui_invalidate_card_cache(void) { ui_card_cache_invalidate(); }

int lcd_ui_card_list_total(void)
{
    extern int g_ui_state;
    return ui_card_cache_total(g_ui_state == 10);
}

static void draw_fill_rect(int x, int y, int w, int h, uint16_t color_rgb565)
{
    if (!s_panel || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BOARD_LCD_H_RES) w = BOARD_LCD_H_RES - x;
    if (y + h > BOARD_LCD_V_RES) h = BOARD_LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    const uint16_t col = lcd_color_to_bus(color_rgb565);
    int chunk_h = 4;
    for (int cur_y = y; cur_y < y + h; cur_y += chunk_h) {
        int lines = (y + h - cur_y > chunk_h) ? chunk_h : (y + h - cur_y);
        size_t total_px = (size_t)w * lines;
        for (size_t i = 0; i < total_px; i++) {
            s_lcd_line_buf[i] = col;
        }
        (void)lcd_ui_draw_bitmap_sync(s_panel, x, cur_y, x + w, cur_y + lines, s_lcd_line_buf);
    }
}

static bool s_ota_ui_initialized = false;

void lcd_ui_show_ota_progress(int pct, int read_bytes, int total_bytes)
{
    if (!s_panel) return;

    const uint16_t bg = RGB565(15, 23, 42);          /* Slate 900 */
    const uint16_t fg_title = RGB565(56, 189, 248);   /* Sky 400 */
    const uint16_t fg_sub = RGB565(148, 163, 184);    /* Slate 400 */
    const uint16_t bar_border = RGB565(51, 65, 85);   /* Slate 700 */
    const uint16_t bar_empty = RGB565(30, 41, 59);    /* Slate 800 */
    const uint16_t bar_fill = RGB565(34, 197, 94);     /* Green 500 */
    const uint16_t fg_warn = RGB565(250, 204, 21);     /* Yellow 400 */

    int bar_x = 24;
    int bar_w = BOARD_LCD_H_RES - 48;
    int bar_y = 105;
    int bar_h = 18;

    if (!s_ota_ui_initialized) {
        /* Xóa toàn màn — chunk 4 dòng (~2.5KB DMA), khớp LVGL/headroom Internal. */
        const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
        const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
        uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
        if (fb) {
            uint16_t bg_bus = lcd_color_to_bus(bg);
            for (size_t i = 0; i < chunk_px; i++) fb[i] = bg_bus;
            for (int y = 0; y < BOARD_LCD_V_RES; y += lines_per_chunk) {
                int y_end = y + lines_per_chunk;
                if (y_end > BOARD_LCD_V_RES) y_end = BOARD_LCD_V_RES;
                lcd_ui_draw_chunk_wait(s_panel, 0, y, BOARD_LCD_H_RES, y_end, fb);
            }
            heap_caps_free(fb);
        }

        /* Tiêu đề & Subtitle */
        draw_str_centered_scaled(28, "CAP NHAT FIRMWARE", fg_title, bg, 2);
        draw_str_centered_scaled(60, "Dang tai firmware...", fg_sub, bg, 1);

        /* Khung viền thanh tiến trình */
        draw_fill_rect(bar_x - 2, bar_y - 2, bar_w + 4, bar_h + 4, bar_border);
        draw_fill_rect(bar_x, bar_y, bar_w, bar_h, bar_empty);

        /* Dòng cảnh báo */
        draw_str_centered_scaled(BOARD_LCD_V_RES - 32, "KHONG TAT NGUON THIET BI", fg_warn, bg, 1);

        s_ota_ui_initialized = true;
    }

    /* Vẽ phần thanh tiến trình đã chạy */
    if (pct >= 0) {
        int fill_w = (pct * bar_w) / 100;
        if (fill_w > bar_w) fill_w = bar_w;
        if (fill_w > 0) {
            draw_fill_rect(bar_x, bar_y, fill_w, bar_h, bar_fill);
        }
        if (bar_w - fill_w > 0) {
            draw_fill_rect(bar_x + fill_w, bar_y, bar_w - fill_w, bar_h, bar_empty);
        }

        /* Số % hiển thị */
        char pct_str[32];
        snprintf(pct_str, sizeof(pct_str), "%d %%", pct);
        draw_str_centered_scaled(138, pct_str, RGB565(255, 255, 255), bg, 2);
    } else {
        /* Chế độ indeterminate (chưa rõ tổng dung lượng) */
        draw_str_centered_scaled(138, "DANG TAI...", RGB565(255, 255, 255), bg, 2);
    }

    /* Dung lượng đã tải */
    char size_str[48];
    if (total_bytes > 0) {
        snprintf(size_str, sizeof(size_str), "%.1f MB / %.1f MB", 
                 (float)read_bytes / (1024.0f * 1024.0f), 
                 (float)total_bytes / (1024.0f * 1024.0f));
    } else if (read_bytes > 0) {
        snprintf(size_str, sizeof(size_str), "%d KB", read_bytes / 1024);
    } else {
        size_str[0] = '\0';
    }
    if (size_str[0]) {
        draw_str_centered_scaled(168, size_str, fg_sub, bg, 1);
    }
}

void lcd_ui_show_ota_result(bool success, const char *msg)
{
    if (!s_panel) return;
    const uint16_t bg = RGB565(15, 23, 42);
    s_ota_ui_initialized = false;

    if (success) {
        int bar_x = 24;
        int bar_w = BOARD_LCD_H_RES - 48;
        int bar_y = 105;
        int bar_h = 18;
        draw_fill_rect(bar_x, bar_y, bar_w, bar_h, RGB565(34, 197, 94));
        draw_str_centered_scaled(28, "CAP NHAT THANH CONG", RGB565(34, 197, 94), bg, 2);
        draw_str_centered_scaled(138, "100 %", RGB565(255, 255, 255), bg, 2);
        draw_str_centered_scaled(168, msg ? msg : "Dang khoi dong lai...", RGB565(56, 189, 248), bg, 1);
    } else {
        draw_str_centered_scaled(28, "CAP NHAT THAT BAI", RGB565(239, 68, 68), bg, 2);
        draw_str_centered_scaled(138, "ERROR", RGB565(239, 68, 68), bg, 2);
        draw_str_centered_scaled(168, msg ? msg : "Loi ket noi / Firmware", RGB565(248, 113, 113), bg, 1);
    }
}
