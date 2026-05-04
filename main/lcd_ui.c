#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "board_pins.h"
#include "app_rfid.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "ili9488_panel.h"
#include "lcd_ui.h"

#include "font8x8_basic.inc"

static const char *TAG = "lcd_ui";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lcd_trans_done_sem;

static bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(s_lcd_trans_done_sem, &need_yield);
    return (need_yield == pdTRUE);
}

esp_err_t lcd_ui_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, const void *color_data)
{
    if (!panel || !s_lcd_trans_done_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, (void *)color_data);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_lcd_trans_done_sem, portMAX_DELAY);
    return ESP_OK;
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
    uint16_t *fb = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!fb) {
        fb = heap_caps_malloc(sz, MALLOC_CAP_DMA);
    }
    return fb;
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

#define RGB565(r, g, b) \
    ((((uint16_t)(r) & 0xF8U) << 8) | (((uint16_t)(g) & 0xFCU) << 3) | (((uint16_t)(b)) >> 3))

static inline uint16_t lcd_rgb565_bus(uint16_t c)
{
#if BOARD_LCD_RGB565_SWAP_BYTES
    return __builtin_bswap16(c);
#else
    return c;
#endif
}

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
    const uint16_t fgx = lcd_rgb565_bus(fg);
    const uint16_t bgx = lcd_rgb565_bus(bg);

    s_glyph_buf ^= 1u;
    uint16_t *g = s_glyph[s_glyph_buf];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            // Khối bitmap bị lật ngược bởi phần cứng, nên cần đưa bit=7 vào col=0 (phía bên phải của màn hình vật lý)
            int bit = 7 - col;
            g[row * 8 + col] = (bits & (1 << bit)) ? fgx : bgx;
        }
    }
    int real_x = BOARD_LCD_H_RES - 8 - x;
    (void)lcd_ui_draw_bitmap_sync(s_panel, real_x, y, real_x + 8, y + 8, g);
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
static void clear_hband_height(int y, int h, uint16_t bg_rgb565)
{
    if (!s_panel || y < 0 || y + h > BOARD_LCD_V_RES) {
        return;
    }
    static DRAM_ATTR uint16_t band[BOARD_LCD_H_RES * 24] __attribute__((aligned(16))); // support up to scale=3 (24 px height)
    const uint16_t bx = lcd_rgb565_bus(bg_rgb565);
    int limit = (h > 24) ? 24 : h;
    for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * limit; i++) {
        band[i] = bx;
    }
    (void)lcd_ui_draw_bitmap_sync(s_panel, 0, y, BOARD_LCD_H_RES, y + limit, band);
}

static DRAM_ATTR uint16_t s_glyph_scaled[2][24 * 24] __attribute__((aligned(16)));
static uint8_t s_glyph_scaled_buf;

static void draw_char_scaled(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 3) scale = 3;
    uint8_t idx = (uint8_t)c;
    if (idx >= 128u) idx = (uint8_t)'?';
    const uint16_t fgx = lcd_rgb565_bus(fg);
    const uint16_t bgx = lcd_rgb565_bus(bg);

    if (scale == 1) {
        draw_char(x, y, c, fg, bg);
        return;
    }

    s_glyph_scaled_buf ^= 1u;
    uint16_t *g = s_glyph_scaled[s_glyph_scaled_buf];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            // Khối bitmap bị lật ngược bởi phần cứng, nên cần đưa bit=7 vào col=0 (phía bên phải của màn hình vật lý)
            int bit = 7 - col;
            uint16_t color = (bits & (1 << bit)) ? fgx : bgx;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    g[(row * scale + dy) * (8 * scale) + (col * scale + dx)] = color;
                }
            }
        }
    }
    int real_x = BOARD_LCD_H_RES - (8 * scale) - x;
    (void)lcd_ui_draw_bitmap_sync(s_panel, real_x, y, real_x + 8 * scale, y + 8 * scale, g);
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
        // Forced recompile comment
        draw_char_scaled(cx, y, tmp[i], fg, bg, scale);
    }
}

static void draw_str_centered(int y, const char *s, uint16_t fg, uint16_t bg)
{
    draw_str_centered_scaled(y, s, fg, bg, 1);
}

/** Căn trái (lề ngang nhỏ), cùng font với draw_str_centered. */
static void draw_str_left_scaled(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
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

    const int pad_x = 0;
    size_t max_n = (size_t)((BOARD_LCD_H_RES - pad_x) / (8 * scale));
    if (max_n == 0) return;
    if (n > max_n) n = max_n;

    int x0 = pad_x;
    for (size_t i = 0; i < n; i++) {
        int cx = x0 + (int)i * (8 * scale);
        if (cx + 8 * scale > BOARD_LCD_H_RES) break;
        draw_char_scaled(cx, y, tmp[i], fg, bg, scale);
    }
}

esp_err_t lcd_ui_init(void)
{
    if (!s_lcd_trans_done_sem) {
        s_lcd_trans_done_sem = xSemaphoreCreateBinary();
    }

    esp_err_t e = lcd_bl_gpio_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "lcd_bl_gpio_init: %s", esp_err_to_name(e));
        return e;
    }

    /*
     * Màn RPi 3.5": touch XPT2046 chung SPI bus (SCLK/MOSI/MISO) với LCD.
     * Nếu TP_CS float LOW → XPT2046 active cùng lúc LCD → bus contention → trắng.
     * Kéo TP_CS HIGH trước khi init SPI bus để deselect touch.
     */
    {
        const gpio_num_t tp_cs = BOARD_TOUCH_CS_GPIO;
        if (tp_cs != GPIO_NUM_NC) {
            gpio_reset_pin(tp_cs);
            gpio_set_direction(tp_cs, GPIO_MODE_OUTPUT);
            gpio_set_level(tp_cs, 1);
            ESP_LOGI(TAG, "TP_CS GPIO%d pulled HIGH (deselect touch)", (int)tp_cs);
        }
    }

    esp_lcd_panel_io_handle_t io = NULL;
    bool lcd_spi_inited = false;

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
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 3,
    };
    e = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    lcd_spi_inited = (e == ESP_OK);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return e;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
#if BOARD_LCD_PANEL_PROFILE == 1
        /* ILI9486 SPI shift register is 16-bit wide — cmd 16-bit, params padded manually */
        .lcd_cmd_bits = 16,
        .lcd_param_bits = 8,
#else
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
#endif
        .spi_mode = BOARD_LCD_SPI_MODE,
        .trans_queue_depth = 24,
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

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
#if BOARD_LCD_USE_BGR_ELEMENT_ORDER
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_ili9488(io, &panel_cfg, &s_panel);
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

// #if BOARD_LCD_TRY_READ_LCD_ID 
//     /* Sau reset + SLPOUT + init. Đọc 4B RDDID: byte đầu SPI thường dummy; vẫn FF = không có SDO TFT trên MISO. */
//     vTaskDelay(pdMS_TO_TICKS(10));
//     {
//         uint8_t id4[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
//         uint8_t id1 = 0xFF;
//         uint8_t d0 = 0xFF;
//         esp_err_t ir = esp_lcd_panel_io_rx_param(io, 0x04, id4, sizeof id4);
//         (void)esp_lcd_panel_io_rx_param(io, 0x0A, &id1, 1);
//         (void)esp_lcd_panel_io_rx_param(io, 0xD0, &d0, 1);
//         ESP_LOGI(TAG,
//                  "LCD đọc SPI (sau init): RDDID(04h) err=%s, 4 byte=%02x %02x %02x %02x | RDDPM(0Ah)=%02x | read(D0h)=%02x",
//                  esp_err_to_name(ir), id4[0], id4[1], id4[2], id4[3], id1, d0);
//         const bool rddid_all_ff =
//             id4[0] == 0xff && id4[1] == 0xff && id4[2] == 0xff && id4[3] == 0xff;
//         if (rddid_all_ff) {
//             ESP_LOGW(TAG,
//                      "RDDID toàn FF: thường KHÔNG phải lỗi phần mềm — trên shield Pi, pin MISO (Pi 21) hay chỉ nối "
//                      "touch (XPT2046), không nối SDO của ILI9488. Hiển thị chỉ cần MOSI/SCK/CS/DC/RST.");
//             ESP_LOGW(TAG,
//                      "Nếu đã hàn đúng SDO chip TFT → GPIO%d mà vẫn FF: đo/dây hoặc thử BOARD_LCD_ILI9488_B0=0x80.",
//                      (int)BOARD_LCD_MISO_GPIO);
//         }
//     }
// #endif

#if BOARD_LCD_SWAP_XY_AFTER_INIT
    e = esp_lcd_panel_swap_xy(s_panel, true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_swap_xy: %s", esp_err_to_name(e));
    }
#endif
    e = esp_lcd_panel_mirror(s_panel,
                             BOARD_LCD_MIRROR_X_AFTER_INIT != 0,
                             BOARD_LCD_MIRROR_Y_AFTER_INIT != 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_mirror: %s", esp_err_to_name(e));
    }
    (void)esp_lcd_panel_set_gap(s_panel, 0, 0);

#if BOARD_LCD_PANEL_INVERT
    (void)esp_lcd_panel_invert_color(s_panel, true);
#else
    (void)esp_lcd_panel_invert_color(s_panel, false);
#endif
    (void)esp_lcd_panel_disp_on_off(s_panel, true);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_ui_set_backlight(true);
    vTaskDelay(pdMS_TO_TICKS(BOARD_LCD_POST_DISPON_DELAY_MS));

#if BOARD_LCD_BL_GPIO == GPIO_NUM_NC
    ESP_LOGI(TAG,
             "LCD BL: không dùng GPIO (NC) — đèn nền thường nối sẵn trên module; nếu màn TỐI thì nối BL/3V3.");
#endif

    /* Nền sau init (mặc định xanh đậm — tránh cảm giác “trắng xóa” nếu invert/polarity lệch) */
    uint16_t bg = lcd_rgb565_bus(RGB565(BOARD_LCD_STARTUP_CLEAR_R, BOARD_LCD_STARTUP_CLEAR_G, BOARD_LCD_STARTUP_CLEAR_B));
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
             "LCD profile=%d %dx%d OK pclk=%u invert=%d madctl_base=0x%02x spi_rgb666=%d swap_caset_raset_mv=%d bgr=%d swap_xy=%d mirror=%d,%d bl=%s",
             BOARD_LCD_PANEL_PROFILE,
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, (unsigned)BOARD_LCD_PCLK_HZ, BOARD_LCD_PANEL_INVERT,
             BOARD_ILI9488_MADCTL_BASE,
#if BOARD_LCD_PANEL_PROFILE == 1
             1,
#elif BOARD_LCD_ILI9488_SPI_RGB666
             1,
#else
             0,
#endif
#if BOARD_LCD_ILI9488_SWAP_ADDR_WINDOW_WITH_MV
             1,
#else
             0,
#endif
             BOARD_LCD_USE_BGR_ELEMENT_ORDER,
             BOARD_LCD_SWAP_XY_AFTER_INIT,
             BOARD_LCD_MIRROR_X_AFTER_INIT,
             BOARD_LCD_MIRROR_Y_AFTER_INIT,
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
    uint16_t bg = RGB565(0, 0, 0);
    uint16_t fg = RGB565(0, 255, 0);
    int y = (BOARD_LCD_V_RES - 8) / 2; 
    draw_str_centered(y, text, fg, bg);
    vTaskDelay(pdMS_TO_TICKS(50)); //
}

void lcd_ui_show_lines(const char *line1, const char *line2)
{
    if (!s_panel) {
        return;
    }
    uint16_t bg = RGB565(0, 0, 0);
    uint16_t fg = RGB565(230, 240, 255);
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
    uint16_t bg = lcd_rgb565_bus(RGB565(0, 0, 0));
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

void lcd_ui_show_attendance(const char *line1, const char *line2, const char *line3, int azure_st, int check_type)
{
    if (!s_panel) {
        return;
    }
    uint16_t bg = RGB565(0, 0, 0);
    uint16_t fg = RGB565(255, 255, 255);
 
    int scale = 2;
    const int line_h = 8 * scale;
    int gap = 16;
    const int bottom_pad = 10;
    int y3 = BOARD_LCD_V_RES - line_h - bottom_pad;
    int y2, y1, y0;
    int show_greet = (check_type == 1 || check_type == 2);
    if (show_greet) {
        /* 4 hang: chao (tren) -> ma -> ten -> gio; giam gap neu tran man */
        do {
            y2 = y3 - line_h - gap;
            y1 = y2 - line_h - gap;
            y0 = y1 - line_h - gap;
            if (y0 >= 100) { // Keep it below the image (assuming image is at top)
                break;
            }
            gap--;
        } while (gap >= 4);
        if (y0 < 100) {
            show_greet = 0;
        }
    }
    if (!show_greet) {
        y2 = y3 - line_h - gap;
        y1 = y2 - line_h - gap;
        y0 = 100;
    }
    if (y1 < 100) {
        y1 = 100;
        y2 = y1 + line_h + gap;
        y3 = y2 + line_h + gap;
        if (y3 + line_h > BOARD_LCD_V_RES) {
            y3 = BOARD_LCD_V_RES - line_h;
        }
    }

    /* Dòng trên cùng: lời chào (IN / OUT) — chuỗi ASCII, font 8x8 không dấu */
    if (show_greet) {
        if (check_type == 1) {
            draw_str_centered_scaled(y0, "Xin Chao ! ^_^", RGB565(0, 255, 0), bg, scale);
        } else {
            draw_str_centered_scaled(y0, "Tam Biet ! <3", RGB565(255, 165, 0), bg, scale);
        }
    }

    // Mã số
    draw_str_left_scaled(y1, line1 ? line1 : "", RGB565(200, 200, 200), bg, scale);

    // Tên toàn bộ hàng 
    draw_str_left_scaled(y2, line2 ? line2 : "", fg, bg, scale);

    // Dòng 3: Thời gian
    draw_str_left_scaled(y3, line3 ? line3 : "", RGB565(150, 150, 150), bg, scale);
 
    int len1 = line1 ? strlen(line1) : 0;
    if (len1 > (BOARD_LCD_H_RES / (8 * scale)) - 2) {
        len1 = (BOARD_LCD_H_RES / (8 * scale)) - 2;
    }
    int ax = 2 + (len1 + 1) * 8 * scale;
 
    if (azure_st == 1) {
        draw_char_scaled(ax, y1, 'v', RGB565(0, 255, 0), bg, scale);
    } else {
        draw_char_scaled(ax, y1, 'x', RGB565(255, 0, 0), bg, scale);
    }
}

static int s_fb_y_offset = 0;
static int s_fb_chunk_h = BOARD_LCD_V_RES;

static void fb_draw_pixel(uint16_t *fb, int x, int y, uint16_t color) {
    x = BOARD_LCD_H_RES - 1 - x;
    
    if (x >= 0 && x < BOARD_LCD_H_RES && y >= s_fb_y_offset && y < s_fb_y_offset + s_fb_chunk_h) {
        fb[(y - s_fb_y_offset) * BOARD_LCD_H_RES + x] = lcd_rgb565_bus(color);
    }
}

static void fb_draw_circle(uint16_t *fb, int xc, int yc, int r, uint16_t color) {
    int x = -r, y = 0, err = 2 - 2 * r;
    do {
        fb_draw_pixel(fb, xc - x, yc + y, color);
        fb_draw_pixel(fb, xc - y, yc - x, color);
        fb_draw_pixel(fb, xc + x, yc - y, color);
        fb_draw_pixel(fb, xc + y, yc + x, color);
        r = err;
        if (r <= y) err += ++y * 2 + 1;
        if (r > x || err > y) err += ++x * 2 + 1;
    } while (x < 0);
}

static void fb_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        fb_draw_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void fb_draw_char_scaled(uint16_t *fb, int x, int y, char c, uint16_t fg, uint16_t bg, int scale) {
    uint8_t idx = (uint8_t)c;
    if (idx >= 128u) idx = (uint8_t)'?';
    if (scale < 1) scale = 1;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            // LSB của font nằm bên trái
            int bit = col;
            uint16_t color = (bits & (1 << bit)) ? fg : bg;
            if (color != bg || bg != 0) { 
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        fb_draw_pixel(fb, x + col * scale + dx, y + row * scale + dy, color);
                    }
                }
            }
        }
    }
}

static void fb_draw_rect_filled(uint16_t *fb, int x, int y, int w, int h, uint16_t color) {
    for (int i = y; i < y + h; i++) {
        for (int j = x; j < x + w; j++) {
            fb_draw_pixel(fb, j, i, color);
        }
    }
}

static void fb_draw_str(uint16_t *fb, int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    if (!s) return;
    int n = strlen(s);
    for (int i = 0; i < n; i++) {
        int cx = x + i * 8 * scale;
        fb_draw_char_scaled(fb, cx, y, s[i], fg, bg, scale);
    }
}

static void fb_draw_arc(uint16_t *fb, int xc, int yc, int r, int start_angle, int end_angle, uint16_t color) {
    for (int a = start_angle; a <= end_angle; a += 2) {
        float rad = a * 3.14159f / 180.0f;
        int x = xc + (int)(r * cosf(rad));
        int y = yc + (int)(r * sinf(rad));
        fb_draw_pixel(fb, x, y, color);
        // Vẽ dày thêm một chút
        fb_draw_pixel(fb, x, y+1, color);
        fb_draw_pixel(fb, x+1, y, color);
    }
}

static void fb_draw_analog_clock(uint16_t *fb, int xc, int yc, int r, int h, int m, int s, uint16_t bg) {
    // Vẽ viền ngoài sang trọng
    fb_draw_circle(fb, xc, yc, r, RGB565(80, 80, 120));
    fb_draw_circle(fb, xc, yc, r - 1, RGB565(150, 150, 200));
    fb_draw_circle(fb, xc, yc, r - 2, RGB565(80, 80, 120));
    
    // Draw numbers
    fb_draw_str(fb, xc - 8, yc - r + 8, "12", RGB565(200, 200, 200), bg, 2);
    fb_draw_str(fb, xc + r - 22, yc - 8, "3", RGB565(200, 200, 200), bg, 2);
    fb_draw_str(fb, xc - 8, yc + r - 24, "6", RGB565(200, 200, 200), bg, 2);
    fb_draw_str(fb, xc - r + 6, yc - 8, "9", RGB565(200, 200, 200), bg, 2);

    // Vẽ vạch giờ (12 vạch)
    for (int i = 0; i < 12; i++) {
        if (i % 3 == 0) continue; // Skip number positions
        float angle = i * 30.0f * 3.14159f / 180.0f;
        int x0 = xc + (int)((r - 8) * cosf(angle));
        int y0 = yc + (int)((r - 8) * sinf(angle));
        int x1 = xc + (int)((r - 3) * cosf(angle));
        int y1 = yc + (int)((r - 3) * sinf(angle));
        fb_draw_line(fb, x0, y0, x1, y1, RGB565(150, 150, 150));
    }

    // Kim Giờ (Ngắn, Dày)
    float angle_h = (h % 12 + m / 60.0f) * 30.0f - 90.0f;
    int xh = xc + (int)((r * 0.5f) * cosf(angle_h * 3.14159f / 180.0f));
    int yh = yc + (int)((r * 0.5f) * sinf(angle_h * 3.14159f / 180.0f));
    fb_draw_line(fb, xc, yc, xh, yh, RGB565(255, 255, 0));
    fb_draw_line(fb, xc+1, yc, xh+1, yh, RGB565(255, 255, 0)); // Double line for thickness
    
    // Kim Phút (Dài, Mảnh)
    float angle_m = (m + s / 60.0f) * 6.0f - 90.0f;
    int xm = xc + (int)((r * 0.8f) * cosf(angle_m * 3.14159f / 180.0f));
    int ym = yc + (int)((r * 0.8f) * sinf(angle_m * 3.14159f / 180.0f));
    fb_draw_line(fb, xc, yc, xm, ym, RGB565(0, 255, 255));
    
    // Kim Giây (Màu Đỏ)
    float angle_s = s * 6.0f - 90.0f;
    int xs = xc + (int)((r * 0.9f) * cosf(angle_s * 3.14159f / 180.0f));
    int ys = yc + (int)((r * 0.9f) * sinf(angle_s * 3.14159f / 180.0f));
    fb_draw_line(fb, xc, yc, xs, ys, RGB565(255, 50, 50));
    
    // Tâm đồng hồ
    fb_draw_circle(fb, xc, yc, 3, RGB565(255, 255, 255));
}

static void fb_draw_str_centered(uint16_t *fb, int y, const char *s, uint16_t fg, uint16_t bg, int scale) {
    if (!s) return;
    int n = strlen(s);
    if (n > 20) n = 20; // safe limit
    int text_w = n * 8 * scale;
    int x = (BOARD_LCD_H_RES - text_w) / 2;
    if (x < 0) x = 0;
    
    for (int i = 0; i < n; i++) {
        int cx = x + i * 8 * scale;
        fb_draw_char_scaled(fb, cx, y, s[i], fg, bg, scale);
    }
}

void lcd_ui_show_idle_screen(const char *title, const char *wifi_status, int rssi, int azure_st, int d, int mo, int y, int h, int m, int s)
{
    if (!s_panel) return;

    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    // Palette
    uint16_t bg_color = RGB565(10, 10, 20);      
    uint16_t header_bg = RGB565(20, 25, 45);    
    uint16_t accent_color = RGB565(0, 200, 255); 
    uint16_t fg_clock = RGB565(255, 255, 255);  
    uint16_t fg_date = RGB565(180, 190, 210);   
    uint16_t button_bg = RGB565(30, 144, 255);
    uint16_t button_fg = RGB565(255, 255, 255);

    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", h, m, s);
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "THU %d, %02d/%02d/%04d", (d % 7) + 2, d, mo, y); // Giả lập thứ

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES) {
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        }

        // Xóa nền
        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++) fb[i] = lcd_rgb565_bus(bg_color);

        // Header area (Y=0..60)
        if (s_fb_y_offset < 60) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 60, header_bg);
            fb_draw_line(fb, 0, 59, BOARD_LCD_H_RES, 59, accent_color);
            
            // Dòng 1
            fb_draw_str_centered(fb, 10, title ? title : "MAY CHAM CONG", accent_color, header_bg, 2);
            
            // Dòng 2
            fb_draw_str(fb, 20, 35, "MEBIECO", RGB565(255, 200, 0), header_bg, 2);
            
            // Vẽ icon Wifi bên cạnh (Dạng sóng cong chuẩn)
            int wifi_x = 160;
            int wifi_y = 52;
            int bars = (rssi > -60) ? 3 : (rssi > -75) ? 2 : (rssi > -85) ? 1 : 0;
            if (rssi == 0) bars = 0; // Disconnected
            
            // Chấm tròn dưới cùng
            uint16_t c_dot = (bars >= 0 && rssi != 0) ? RGB565(0, 255, 0) : RGB565(100, 100, 100);
            fb_draw_circle(fb, wifi_x, wifi_y, 2, c_dot);
            
            // Các vòng cung (arcs)
            for (int i = 1; i <= 3; i++) {
                uint16_t c_arc = (i <= bars) ? RGB565(0, 255, 0) : RGB565(100, 100, 100);
                fb_draw_arc(fb, wifi_x, wifi_y, 2 + i * 5, 225, 315, c_arc);
            }
            
            // Vẽ icon Azure kết nối (Lên xuống hoặc Xoay tròn)
            int az_x = 220;
            int az_y = 35;
            if (azure_st) {
                // Connected: Mũi tên lên xuống
                fb_draw_line(fb, az_x, az_y + 12, az_x, az_y, RGB565(0, 255, 0)); // Lên
                fb_draw_line(fb, az_x, az_y, az_x - 4, az_y + 4, RGB565(0, 255, 0));
                fb_draw_line(fb, az_x, az_y, az_x + 4, az_y + 4, RGB565(0, 255, 0));
                
                int ax2 = az_x + 12;
                fb_draw_line(fb, ax2, az_y, ax2, az_y + 12, RGB565(0, 200, 255)); // Xuống
                fb_draw_line(fb, ax2, az_y + 12, ax2 - 4, az_y + 8, RGB565(0, 200, 255));
                fb_draw_line(fb, ax2, az_y + 12, ax2 + 4, az_y + 8, RGB565(0, 200, 255));
            } else {
                // Disconnected: Chấm xoay tròn
                int phase = (s % 4);
                int r = 6;
                int cx = az_x + 6;
                int cy = az_y + 6;
                uint16_t c_on = RGB565(255, 50, 50);
                uint16_t c_off = RGB565(100, 50, 50);
                fb_draw_circle(fb, cx, cy - r, 2, phase == 0 ? c_on : c_off);
                fb_draw_circle(fb, cx + r, cy, 2, phase == 1 ? c_on : c_off);
                fb_draw_circle(fb, cx, cy + r, 2, phase == 2 ? c_on : c_off);
                fb_draw_circle(fb, cx - r, cy, 2, phase == 3 ? c_on : c_off);
            }
        }

        // Dòng 3 & 4: Đồng hồ số và Ngày tháng (Y=80..150)
        fb_draw_str_centered(fb, 80, time_str, fg_clock, bg_color, 4);
        fb_draw_str_centered(fb, 125, date_str, fg_date, bg_color, 2);

        // Dòng 5: Đồng hồ kim (Y=160..340)
        fb_draw_analog_clock(fb, BOARD_LCD_H_RES / 2, 250, 85, h, m, s, bg_color);

        // Dòng 6: Nút ĐĂNG NHẬP (Y=400..460)
        if (s_fb_y_offset + s_fb_chunk_h > 380) {
            int btn_y = 400;
            int btn_h = 50;
            int btn_w = 260;
            int btn_x = (BOARD_LCD_H_RES - btn_w) / 2;
            
            // Vẽ nút
            fb_draw_rect_filled(fb, btn_x, btn_y, btn_w, btn_h, button_bg);
            
            // Viền nút
            fb_draw_line(fb, btn_x, btn_y, btn_x + btn_w, btn_y, RGB565(100, 200, 255));
            fb_draw_line(fb, btn_x, btn_y + btn_h, btn_x + btn_w, btn_y + btn_h, RGB565(10, 50, 100));
            fb_draw_line(fb, btn_x, btn_y, btn_x, btn_y + btn_h, RGB565(100, 200, 255));
            fb_draw_line(fb, btn_x + btn_w, btn_y, btn_x + btn_w, btn_y + btn_h, RGB565(10, 50, 100));
            
            fb_draw_str_centered(fb, btn_y + 17, "DANG NHAP", button_fg, button_bg, 2);
        }

        // Đẩy chunk
        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }

    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;
}

void lcd_ui_show_login_screen(const char *entered_pin, bool is_error)
{
    if (!s_panel) return;

    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    uint16_t bg_color = RGB565(15, 15, 25);
    uint16_t header_bg = RGB565(30, 35, 55);
    uint16_t text_color = RGB565(255, 255, 255);
    uint16_t err_color = RGB565(255, 50, 50);
    uint16_t box_bg = RGB565(40, 45, 65);
    uint16_t btn_bg = RGB565(50, 60, 80);
    uint16_t btn_hi = RGB565(100, 150, 255);

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES) {
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        }

        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++) fb[i] = lcd_rgb565_bus(bg_color);

        // Header
        if (s_fb_y_offset < 60) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 60, header_bg);
            fb_draw_str_centered(fb, 20, "NHAP MAT KHAU", text_color, header_bg, 3);
        }

        // Input Box (Y = 80, H = 50)
        if (s_fb_y_offset + s_fb_chunk_h > 80 && s_fb_y_offset <= 130) {
            fb_draw_rect_filled(fb, 30, 80, 260, 50, box_bg);
            fb_draw_line(fb, 30, 80, 290, 80, btn_hi);
            fb_draw_line(fb, 30, 130, 290, 130, btn_hi);
            
            // Draw asterisks or text
            if (is_error) {
                fb_draw_str_centered(fb, 95, "SAI MAT KHAU!", err_color, box_bg, 2);
            } else {
                char display_pin[16] = {0};
                int len = strlen(entered_pin);
                for (int i=0; i<len && i<15; i++) display_pin[i] = '*'; // Mask pin
                if (len == 0) {
                    fb_draw_str_centered(fb, 95, "NHAP MA PIN", RGB565(150,150,150), box_bg, 2);
                } else {
                    fb_draw_str_centered(fb, 95, display_pin, text_color, box_bg, 3);
                }
            }
        }

        // Numpad Grid
        int start_y = 150;
        int key_w = 80;
        int key_h = 55;
        int sp_x = 15;
        int sp_y = 10;
        int start_x = 25; 
        
        const char* kmap[4][3] = {
            {"1","2","3"},
            {"4","5","6"},
            {"7","8","9"},
            {"Huy","0","<"}
        };

        for (int r = 0; r < 4; r++) {
            int by = start_y + r * (key_h + sp_y);
            if (s_fb_y_offset + s_fb_chunk_h > by && s_fb_y_offset <= by + key_h) {
                for (int c = 0; c < 3; c++) {
                    int bx = start_x + c * (key_w + sp_x);
                    uint16_t current_bg = btn_bg;
                    const char* key = kmap[r][c];
                    
                    if (strcmp(key, "Huy") == 0 || strcmp(key, "<") == 0) current_bg = RGB565(200, 50, 50); 

                    fb_draw_rect_filled(fb, bx, by, key_w, key_h, current_bg);
                    fb_draw_line(fb, bx, by, bx+key_w, by, RGB565(150,160,180));
                    fb_draw_line(fb, bx, by+key_h, bx+key_w, by+key_h, RGB565(20,30,50));
                    
                    int text_len = strlen(key);
                    int text_size = (strcmp(key, "Huy") == 0) ? 2 : 3;
                    int char_w = (text_size == 3) ? 18 : 12;
                    int tx = bx + (key_w / 2) - ((text_len * char_w) / 2);
                    int ty = by + (key_h / 2) - (text_size * 4);
                    fb_draw_str(fb, tx, ty, key, text_color, current_bg, text_size);
                }
            }
        }

        // BIG OK BUTTON
        int ok_y = start_y + 4 * (key_h + sp_y);
        int ok_h = 60;
        if (s_fb_y_offset + s_fb_chunk_h > ok_y && s_fb_y_offset <= ok_y + ok_h) {
            uint16_t ok_bg = RGB565(50, 200, 50);
            fb_draw_rect_filled(fb, 25, ok_y, 270, ok_h, ok_bg);
            fb_draw_line(fb, 25, ok_y, 295, ok_y, RGB565(150, 250, 150));
            fb_draw_line(fb, 25, ok_y+ok_h, 295, ok_y+ok_h, RGB565(20, 100, 20));
            fb_draw_str_centered(fb, ok_y + 18, "OK", text_color, ok_bg, 3);
        }

        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;
}

void lcd_ui_show_menu_screen(int active_item)
{
    if (!s_panel) return;

    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    uint16_t bg_color = RGB565(20, 22, 30);      
    uint16_t header_bg = RGB565(35, 40, 60);    
    uint16_t accent_color = RGB565(0, 200, 255); 
    uint16_t tile_bg = RGB565(40, 45, 65);
    uint16_t tile_hi = RGB565(0, 120, 215);

    // Grid config
    int t_w = 130;
    int t_h = 130;
    int sp_x = 20;
    int sp_y = 20;
    int st_x = 20;
    int st_y = 80;



    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES) {
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        }

        // Xóa nền
        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++) fb[i] = lcd_rgb565_bus(bg_color);

        // Header
        if (s_fb_y_offset < 60) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 60, header_bg);
            fb_draw_line(fb, 0, 59, BOARD_LCD_H_RES, 59, accent_color);
            fb_draw_str_centered(fb, 20, "MENU HE THONG", accent_color, header_bg, 3);
        }

        // 4 Tiles (2x2 Grid)
        for (int i = 0; i < 4; i++) {
            int row = i / 2;
            int col = i % 2;
            int bx = st_x + col * (t_w + sp_x);
            int by = st_y + row * (t_h + sp_y);

            if (s_fb_y_offset + s_fb_chunk_h > by && s_fb_y_offset <= by + t_h) {
                uint16_t current_bg = (i == active_item) ? tile_hi : tile_bg;
                
                // Draw tile background
                fb_draw_rect_filled(fb, bx, by, t_w, t_h, current_bg);
                fb_draw_line(fb, bx, by, bx+t_w, by, RGB565(100, 110, 140)); // Top highlight
                
                // Vector Icons centered
                int cx = bx + t_w / 2;
                int cy = by + t_h / 2;
                
                if (i == 0) {
                    // Gear (Settings) — 8 răng đẹp, vẽ bằng hình chữ nhật theo 8 góc
                    uint16_t i_col    = RGB565(255, 190, 40);
                    uint16_t i_shadow = RGB565(180, 120, 20);
                    // Thân bánh răng (vòng tròn solid r=26)
                    for(int r = 1; r <= 26; r++) fb_draw_circle(fb, cx, cy, r, i_col);
                    // 8 răng cưa — mỗi răng là hình chữ nhật 10x14 xoay quanh tâm
                    // góc: 0, 45, 90, 135, 180, 225, 270, 315 độ
                    int tooth_angles[8] = {0, 45, 90, 135, 180, 225, 270, 315};
                    for (int t = 0; t < 8; t++) {
                        float ang = tooth_angles[t] * 3.14159f / 180.0f;
                        float ca = cosf(ang), sa = sinf(ang);
                        // Tâm răng cách tâm bánh 30px
                        int tx = cx + (int)(30.0f * ca);
                        int ty = cy + (int)(30.0f * sa);
                        // Vẽ hình chữ nhật 12x9 (trục chính hướng ra ngoài)
                        for (int dy = -4; dy <= 4; dy++) {
                            for (int dx = -6; dx <= 6; dx++) {
                                int px = tx + (int)(dx * ca - dy * sa);
                                int py = ty + (int)(dx * sa + dy * ca);
                                fb_draw_pixel(fb, px, py, i_col);
                            }
                        }
                        // Viền bóng nhẹ cho răng
                        for (int dy = -4; dy <= 4; dy++) {
                            int px = tx + (int)( 6 * ca - dy * sa);
                            int py = ty + (int)( 6 * sa + dy * ca);
                            fb_draw_pixel(fb, px, py, i_shadow);
                        }
                    }
                    // Lỗ giữa (màu nền)
                    for(int r = 0; r <= 11; r++) fb_draw_circle(fb, cx, cy, r, current_bg);
                    // Vòng nhỏ trang trí trong lỗ
                    fb_draw_circle(fb, cx, cy, 11, i_shadow);
                } else if (i == 1) {
                    // Book (Logs) - Cyan
                    uint16_t i_col = RGB565(0, 220, 255);
                    uint16_t cover_col = RGB565(0, 150, 200);
                    fb_draw_rect_filled(fb, cx-35, cy-25, 70, 50, cover_col);
                    // Pages
                    fb_draw_rect_filled(fb, cx-32, cy-22, 64, 44, RGB565(240, 240, 240));
                    
                    // Center spine
                    fb_draw_line(fb, cx, cy-25, cx, cy+25, cover_col);
                    fb_draw_line(fb, cx-1, cy-25, cx-1, cy+25, cover_col);
                    fb_draw_line(fb, cx+1, cy-25, cx+1, cy+25, cover_col);
                    
                    // Text lines (Cyan)
                    fb_draw_line(fb, cx-25, cy-12, cx-5, cy-12, i_col);
                    fb_draw_line(fb, cx-25, cy, cx-5, cy, i_col);
                    fb_draw_line(fb, cx-25, cy+12, cx-5, cy+12, i_col);
                    
                    fb_draw_line(fb, cx+5, cy-12, cx+25, cy-12, i_col);
                    fb_draw_line(fb, cx+5, cy, cx+25, cy, i_col);
                    fb_draw_line(fb, cx+5, cy+12, cx+25, cy+12, i_col);
                } else if (i == 2 || i == 3) {
                    // Person - Pink/Purple (i=2) or Green (i=3)
                    uint16_t i_col = (i == 2) ? RGB565(255, 100, 200) : RGB565(50, 255, 100);
                    int p_cx = (i == 3) ? cx - 15 : cx;
                    
                    // Head
                    fb_draw_circle(fb, p_cx, cy-15, 16, i_col);
                    fb_draw_circle(fb, p_cx, cy-15, 15, i_col);
                    fb_draw_circle(fb, p_cx, cy-15, 14, i_col);
                    fb_draw_circle(fb, p_cx, cy-15, 13, i_col);
                    fb_draw_circle(fb, p_cx, cy-15, 12, i_col);
                    // Body
                    fb_draw_circle(fb, p_cx, cy+25, 28, i_col);
                    fb_draw_circle(fb, p_cx, cy+25, 27, i_col);
                    fb_draw_circle(fb, p_cx, cy+25, 26, i_col);
                    fb_draw_circle(fb, p_cx, cy+25, 25, i_col);
                    // Cut bottom
                    fb_draw_rect_filled(fb, bx, cy+25, t_w, t_h-(cy+25-by), current_bg);
                    
                    if (i == 3) {
                        // Plus Sign (Bright Yellow)
                        uint16_t plus_col = RGB565(255, 255, 50);
                        fb_draw_rect_filled(fb, cx+15, cy-5, 24, 8, plus_col);
                        fb_draw_rect_filled(fb, cx+23, cy-13, 8, 24, plus_col);
                    }
                }
               
            }
        }

        // Back Button
        int btn_y = 390;
        int btn_h = 60;
        if (s_fb_y_offset + s_fb_chunk_h > btn_y && s_fb_y_offset <= btn_y + btn_h) {
            uint16_t back_bg = (active_item == 4) ? RGB565(200, 50, 50) : RGB565(80, 20, 20);
            fb_draw_rect_filled(fb, 20, btn_y, 280, btn_h, back_bg);
            fb_draw_line(fb, 20, btn_y, 300, btn_y, RGB565(255, 100, 100));
            fb_draw_str_centered(fb, btn_y + 20, "QUAY LAI", RGB565(255,255,255), back_bg, 2);
        }

        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;
}

void lcd_ui_show_settings_menu(int active_item)
{
    if (!s_panel) return;

    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    uint16_t bg_color = RGB565(15, 20, 25);      
    uint16_t header_bg = RGB565(40, 50, 70);    
    uint16_t accent_color = RGB565(50, 255, 100); 
    
    const char* menu_items[] = {
        "QUAN LY WIFI",
        "DOI MAT KHAU",
        "QUAY LAI"
    };
    int num_items = 3;

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES) {
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        }

        // Xóa nền
        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++) fb[i] = lcd_rgb565_bus(bg_color);

        // Header
        if (s_fb_y_offset < 60) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 60, header_bg);
            fb_draw_line(fb, 0, 59, BOARD_LCD_H_RES, 59, accent_color);
            fb_draw_str_centered(fb, 20, "CAI DAT", accent_color, header_bg, 3);
        }

        // Các mục Menu
        for (int i = 0; i < num_items; i++) {
            int item_y = 100 + i * 80; // nut to hon
            if (s_fb_y_offset + s_fb_chunk_h > item_y && s_fb_y_offset <= item_y + 60) {
                uint16_t item_bg = (i == active_item) ? RGB565(50, 160, 255) : RGB565(40, 45, 60);
                if (i == 2 && i != active_item) item_bg = RGB565(120, 40, 40); // Quay lai
                uint16_t item_fg = (i == active_item) ? RGB565(255, 255, 255) : RGB565(220, 220, 220);
                
                int btn_x = 20;
                int btn_w = 280;
                int btn_h = 60;
                fb_draw_rect_filled(fb, btn_x, item_y, btn_w, btn_h, item_bg);
                
                // Viền nút
                fb_draw_line(fb, btn_x, item_y, btn_x + btn_w, item_y, RGB565(100, 150, 200));
                fb_draw_line(fb, btn_x, item_y + btn_h, btn_x + btn_w, item_y + btn_h, RGB565(20, 20, 30));
                
                // Text center
                fb_draw_str_centered(fb, item_y + 20, menu_items[i], item_fg, item_bg, 2);
            }
        }

        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;
}

void lcd_ui_show_change_password(const char *old_pin, const char *new_pin, int active_field, bool is_error)
{
    if (!s_panel) return;
    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    uint16_t bg_color = RGB565(20, 20, 30);
    uint16_t header_bg = RGB565(100, 50, 200);
    uint16_t text_color = RGB565(255, 255, 255);
    uint16_t box_bg = RGB565(40, 45, 65);
    uint16_t box_active = RGB565(60, 80, 120);
    uint16_t btn_bg = RGB565(50, 60, 80);

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES) {
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        }

        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++) fb[i] = lcd_rgb565_bus(bg_color);

        // Header
        if (s_fb_y_offset < 60) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 60, header_bg);
            fb_draw_str_centered(fb, 20, "DOI MAT KHAU", text_color, header_bg, 3);
        }

        // Box 1: Old Password
        if (s_fb_y_offset + s_fb_chunk_h > 70 && s_fb_y_offset <= 115) {
            uint16_t c_bg = (active_field == 0) ? box_active : box_bg;
            fb_draw_rect_filled(fb, 20, 70, 280, 45, c_bg);
            if (active_field == 0) fb_draw_line(fb, 20, 115, 300, 115, RGB565(100, 200, 255));
            
            int len = strlen(old_pin);
            if (is_error && active_field == 0) {
                fb_draw_str_centered(fb, 82, "SAI MAT KHAU CU", RGB565(255, 50, 50), c_bg, 2);
            } else if (len == 0) {
                fb_draw_str_centered(fb, 82, "MK Cu", RGB565(150,150,150), c_bg, 2);
            } else {
                fb_draw_str_centered(fb, 82, old_pin, text_color, c_bg, 3);
            }
        }

        // Box 2: New Password
        if (s_fb_y_offset + s_fb_chunk_h > 130 && s_fb_y_offset <= 175) {
            uint16_t c_bg = (active_field == 1) ? box_active : box_bg;
            fb_draw_rect_filled(fb, 20, 130, 280, 45, c_bg);
            if (active_field == 1) fb_draw_line(fb, 20, 175, 300, 175, RGB565(100, 200, 255));
            
            int len = strlen(new_pin);
            if (len == 0) {
                fb_draw_str_centered(fb, 142, "MK Moi", RGB565(150,150,150), c_bg, 2);
            } else {
                fb_draw_str_centered(fb, 142, new_pin, text_color, c_bg, 3);
            }
        }

        // Numpad 4x3 (giong man hinh dang nhap)
        // start_y=185, key_w=80, key_h=52, sp_x=15, sp_y=8, start_x=25
        int np_start_y = 185;
        int np_key_w   = 80;
        int np_key_h   = 52;
        int np_sp_x    = 15;
        int np_sp_y    = 8;
        int np_start_x = 25;
        int np_ok_y    = 433;
        int np_ok_h    = 40;

        const char* kmap_np4[4][3] = {
            {"1","2","3"},
            {"4","5","6"},
            {"7","8","9"},
            {"Huy","0","<"}
        };

        for (int r = 0; r < 4; r++) {
            int by = np_start_y + r * (np_key_h + np_sp_y);
            if (s_fb_y_offset + s_fb_chunk_h > by && s_fb_y_offset <= by + np_key_h) {
                for (int c = 0; c < 3; c++) {
                    int bx = np_start_x + c * (np_key_w + np_sp_x);
                    const char* key = kmap_np4[r][c];
                    uint16_t current_bg = btn_bg;
                    if (strcmp(key, "Huy") == 0 || strcmp(key, "<") == 0) current_bg = RGB565(200, 50, 50);

                    fb_draw_rect_filled(fb, bx, by, np_key_w, np_key_h, current_bg);
                    fb_draw_line(fb, bx, by, bx+np_key_w, by, RGB565(150,160,180));
                    fb_draw_line(fb, bx, by+np_key_h, bx+np_key_w, by+np_key_h, RGB565(20,30,50));

                    int text_len = strlen(key);
                    int text_scale = (strcmp(key, "Huy") == 0) ? 2 : 3;
                    int char_px = text_scale * 8;
                    int tx = bx + (np_key_w / 2) - (text_len * char_px / 2);
                    fb_draw_str(fb, tx, by + (np_key_h/2) - text_scale*4, key, text_color, current_bg, text_scale);
                }
            }
        }

        // Nut Luu (xanh la, full width)
        if (s_fb_y_offset + s_fb_chunk_h > np_ok_y && s_fb_y_offset <= np_ok_y + np_ok_h) {
            uint16_t ok_bg = RGB565(50, 200, 50);
            fb_draw_rect_filled(fb, 25, np_ok_y, 270, np_ok_h, ok_bg);
            fb_draw_line(fb, 25, np_ok_y, 295, np_ok_y, RGB565(150, 250, 150));
            fb_draw_line(fb, 25, np_ok_y+np_ok_h, 295, np_ok_y+np_ok_h, RGB565(20, 100, 20));
            fb_draw_str_centered(fb, np_ok_y + 12, "LUU", text_color, ok_bg, 3);
        }

        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;
}

#include "wifi_portal.h" // We need it for wifi_list_get_count()

void lcd_ui_show_wifi_manager(int state, const char *selected_ssid, const char *entered_pass)
{
    if (!s_panel) return;

    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    uint16_t bg_color = RGB565(15, 20, 30);      
    uint16_t header_bg = RGB565(0, 150, 255);    
    uint16_t text_color = RGB565(255, 255, 255);
    uint16_t list_bg = RGB565(40, 50, 70);
    uint16_t btn_bg = RGB565(50, 60, 80);

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES) {
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        }

        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++) fb[i] = lcd_rgb565_bus(bg_color);

        // Header
        if (s_fb_y_offset < 60) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 60, header_bg);
            if (state == 5) fb_draw_str_centered(fb, 20, "WIFI DA LUU", text_color, header_bg, 3);
            else if (state == 6) fb_draw_str_centered(fb, 20, "CHON WIFI", text_color, header_bg, 3);
            else if (state == 7) fb_draw_str_centered(fb, 20, "NHAP MK WIFI", text_color, header_bg, 3);
        }

        if (state == 5 || state == 6) {
            int display_cnt = (state == 5) ? wifi_list_get_count() : g_wifi_scan_count;
            if (display_cnt > 5) display_cnt = 5;

            for (int i = 0; i < 5; i++) {
                int item_y = 70 + i * 55;
                if (s_fb_y_offset + s_fb_chunk_h > item_y && s_fb_y_offset <= item_y + 45) {
                    if (i < display_cnt) {
                        fb_draw_rect_filled(fb, 20, item_y, 280, 45, list_bg);
                        fb_draw_line(fb, 20, item_y, 300, item_y, RGB565(100, 150, 200)); // highlight
                        
                        char ssid[33] = {0};
                        if (state == 5) {
                            wifi_list_get_item(i, ssid, NULL);
                        } else {
                            strncpy(ssid, g_wifi_scan_res[i], 32);
                        }
                        
                        // Icon nho
                        fb_draw_circle(fb, 40, item_y + 22, 6, RGB565(100, 255, 100));
                        fb_draw_str(fb, 60, item_y + 12, ssid, text_color, list_bg, 2);
                        
                        if (state == 5) {
                            // Nut Xoa (Red X)
                            fb_draw_rect_filled(fb, 250, item_y + 5, 40, 35, RGB565(200, 50, 50));
                            fb_draw_str(fb, 265, item_y + 10, "X", text_color, RGB565(200, 50, 50), 2);
                        }
                    } else if (i == display_cnt && state == 5) {
                        // Nut quét mới
                        fb_draw_rect_filled(fb, 20, item_y, 280, 45, RGB565(0, 150, 100));
                        fb_draw_str_centered(fb, item_y + 12, "+ QUET THEM WIFI", text_color, RGB565(0, 150, 100), 2);
                    }
                }
            }
        } else if (state == 7) {
            // Nhập MK WiFi
            if (s_fb_y_offset + s_fb_chunk_h > 70 && s_fb_y_offset <= 120) {
                fb_draw_rect_filled(fb, 10, 70, 300, 50, list_bg);
                fb_draw_str_centered(fb, 75, selected_ssid, RGB565(100, 255, 100), list_bg, 2);
                
                int len = strlen(entered_pass);
                if (len == 0) {
                    fb_draw_str_centered(fb, 100, "Nhap Mat Khau...", RGB565(150,150,150), list_bg, 2);
                } else {
                    fb_draw_str_centered(fb, 100, entered_pass, text_color, list_bg, 2);
                }
            }
            
            /* QWERTY + 3 trang ký tự (SYM) + Huy | SYM | SPACE | Luu */
            int start_y = 150;
            int num_keys[6] = {10, 10, 9, 9, 10, 4};
            int start_x[6]  = {10, 10, 25, 10, 10, 11};
            int key_w[6]    = {28, 28, 28, 28, 28, 70};
            int sp_x[6]     = {2, 2, 2, 2, 2, 6};
            int key_h = 42;
            int sp_y = 8;

            const char *km_row[4][10] = {
                {"1","2","3","4","5","6","7","8","9","0"},
                {"Q","W","E","R","T","Y","U","I","O","P"},
                {"A","S","D","F","G","H","J","K","L",""},
                {"^","Z","X","C","V","B","N","M","<",""},
            };
            const char *km_bot[4] = {"Huy", "SYM", "SPACE", "Luu"};

            for (int r = 0; r < 6; r++) {
                int by = start_y + r * (key_h + sp_y);
                if (s_fb_y_offset + s_fb_chunk_h > by && s_fb_y_offset <= by + key_h) {
                    for (int c = 0; c < num_keys[r]; c++) {
                        int bx = start_x[r] + c * (key_w[r] + sp_x[r]);
                        uint16_t current_bg = btn_bg;
                        char disp_one[8];
                        const char *draw = "";
                        const char *key = "";

                        if (r == 5) {
                            key = km_bot[c];
                            draw = key;
                            if (strcmp(key, "Huy") == 0) current_bg = RGB565(200, 50, 50);
                            else if (strcmp(key, "Luu") == 0) current_bg = RGB565(50, 200, 50);
                            else if (strcmp(key, "SYM") == 0) current_bg = RGB565(130, 95, 40);
                        } else if (r == 4) {
                            snprintf(disp_one, sizeof(disp_one), "%c", (char)g_soft_sym_chars[g_soft_kb_sym_page][c]);
                            draw = disp_one;
                        } else {
                            key = km_row[r][c];
                            draw = key;
                            if (strcmp(key, "Huy") == 0 || strcmp(key, "<") == 0) current_bg = RGB565(200, 50, 50);
                            if (strcmp(key, "^") == 0 && r == 3 && c == 0 && g_soft_kb_upper) current_bg = RGB565(70, 110, 170);
                            if ((unsigned char)key[0] >= 'A' && (unsigned char)key[0] <= 'Z' && key[1] == '\0') {
                                disp_one[0] = g_soft_kb_upper ? key[0] : (char)tolower((unsigned char)key[0]);
                                disp_one[1] = '\0';
                                draw = disp_one;
                            }
                        }

                        fb_draw_rect_filled(fb, bx, by, key_w[r], key_h, current_bg);
                        fb_draw_line(fb, bx, by, bx+key_w[r], by, RGB565(150,160,180));
                        fb_draw_line(fb, bx, by+key_h, bx+key_w[r], by+key_h, RGB565(20,30,50));

                        int text_len = strlen(draw);
                        int tx = bx + (key_w[r] / 2) - (text_len * 4);
                        fb_draw_str(fb, tx, by + 12, draw, text_color, current_bg, 2);
                    }
                }
            }
        }

        // Back Button (For state 5 and 6)
        if (state == 5 || state == 6) {
            int btn_y = 390;
            int btn_h = 60;
            if (s_fb_y_offset + s_fb_chunk_h > btn_y && s_fb_y_offset <= btn_y + btn_h) {
                uint16_t back_bg = RGB565(80, 20, 20);
                fb_draw_rect_filled(fb, 20, btn_y, 280, btn_h, back_bg);
                fb_draw_line(fb, 20, btn_y, 300, btn_y, RGB565(255, 100, 100));
                fb_draw_str_centered(fb, btn_y + 20, "QUAY LAI", RGB565(255,255,255), back_bg, 2);
            }
        }

        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;
}

/* ============================================================
 * MAN HINH NHAT KY (state 8)
 * Doc CSV tu SD, hien 5 dong/trang.
 * ============================================================ */

#include "sd_card.h"
#include "scan_log.h"
#include <stdio.h>

#define LOG_ROWS_PER_PAGE 5

typedef struct {
    char date[12]; /* DD/MM/YYYY */
    char time[10]; /* HH:MM:SS */
    char name[24];
    char id[20];
} LogRow_t;
static LogRow_t  s_log_rows[LOG_ROWS_PER_PAGE];
static int       s_log_count = 0;
static int       s_log_page_cached = -99;

void lcd_ui_invalidate_log_cache(void) { s_log_page_cached = -99; }

static void log_parse_ts_field(const char *f0, LogRow_t *r)
{
    int Y, M, D, h, mi, s2;
    if (f0 && sscanf(f0, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &s2) == 6) {
        snprintf(r->date, sizeof(r->date), "%02d/%02d/%04d", D, M, Y);
        snprintf(r->time, sizeof(r->time), "%02d:%02d:%02d", h, mi, s2);
    } else {
        strncpy(r->date, "--", sizeof(r->date) - 1);
        r->date[sizeof(r->date) - 1] = '\0';
        strncpy(r->time, (f0 && f0[0]) ? f0 : "?", sizeof(r->time) - 1);
        r->time[sizeof(r->time) - 1] = '\0';
    }
}

static void log_screen_load(int page)
{
    if (s_log_page_cached == page) return;
    s_log_page_cached = page;
    s_log_count = 0;
    if (!sd_card_is_mounted()) return;

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) { sd_card_unlock(); return; }

    int skip = page * LOG_ROWS_PER_PAGE;
    int cur  = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (cur++ < skip) continue;
        if (s_log_count >= LOG_ROWS_PER_PAGE) break;
        char buf[256];
        strncpy(buf, line, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
        char *nl = strchr(buf, '\n'); if (nl) *nl=0;
        char *f0 = strtok(buf, "|");
        strtok(NULL, "|"); // uid
        char *f2 = strtok(NULL, "|"); // name
        char *f3 = strtok(NULL, "|"); // id
        LogRow_t *r = &s_log_rows[s_log_count++];
        log_parse_ts_field(f0 ? f0 : "", r);
        strncpy(r->name, f2 ? f2 : "", 23); r->name[23]=0;
        strncpy(r->id,   f3 ? f3 : "", 19); r->id[19]=0;
    }
    fclose(fp);
    sd_card_unlock();
}

void lcd_ui_show_log_screen(int page)
{
    if (!s_panel) return;
    log_screen_load(page);

    const int lines_per_chunk = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t chunk_px = (size_t)BOARD_LCD_H_RES * lines_per_chunk;
    uint16_t *fb = lcd_ui_alloc_fb(chunk_px);
    if (!fb) return;

    uint16_t bg_color = RGB565(15, 15, 25);
    uint16_t hdr_bg   = RGB565(30, 80, 160);
    uint16_t row_even = RGB565(25, 30, 45);
    uint16_t row_odd  = RGB565(35, 40, 60);
    uint16_t sep_col  = RGB565(60, 70, 100);
    uint16_t text_col = RGB565(230, 235, 255);
    uint16_t dim_col  = RGB565(140, 150, 180);
    uint16_t nav_bg   = RGB565(50, 55, 80);
    uint16_t back_bg  = RGB565(80, 20, 20);
    uint16_t accent   = RGB565(80, 180, 255);

        int hdr_h      = 48;
    int col_hdr_h  = 18;
    int row_h      = 58;
    int row_start  = hdr_h + col_hdr_h;

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lines_per_chunk) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h  = lines_per_chunk;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES)
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;

        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++)
            fb[i] = lcd_rgb565_bus(bg_color);

        // Header
        if (s_fb_y_offset < hdr_h) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, hdr_h, hdr_bg);
            fb_draw_line(fb, 0, hdr_h-1, BOARD_LCD_H_RES, hdr_h-1, accent);
            char title[32];
            snprintf(title, sizeof(title), "NHAT KY - Trang %d", page+1);
            fb_draw_str_centered(fb, 14, title, RGB565(255,255,255), hdr_bg, 2);
        }

        // Column header
        int th = hdr_h;
        if (s_fb_y_offset + s_fb_chunk_h > th && s_fb_y_offset <= th + col_hdr_h) {
            uint16_t ch_bg = RGB565(25, 60, 120);
            fb_draw_rect_filled(fb, 0, th, BOARD_LCD_H_RES, col_hdr_h, ch_bg);
            fb_draw_str(fb,  4, th+4, "GIO",  accent, ch_bg, 1);
            fb_draw_str(fb, 68, th+4, "TEN",  accent, ch_bg, 1);
        }

        // Empty state
        if (s_log_count == 0) {
            int ey = row_start + 20;
            if (s_fb_y_offset + s_fb_chunk_h > ey && s_fb_y_offset <= ey + 30) {
                const char *msg = sd_card_is_mounted() ? "Chua co ban ghi" : "SD chua gan";
                fb_draw_str_centered(fb, ey, msg, dim_col, bg_color, 2);
            }
        }

        // Rows
        for (int ri = 0; ri < s_log_count; ri++) {
            int ry = row_start + ri * row_h;
            if (s_fb_y_offset + s_fb_chunk_h <= ry) break;
            if (s_fb_y_offset > ry + row_h) continue;

            uint16_t rb = (ri % 2 == 0) ? row_even : row_odd;
            fb_draw_rect_filled(fb, 0, ry, BOARD_LCD_H_RES, row_h-2, rb);
            fb_draw_line(fb, 0, ry+row_h-2, BOARD_LCD_H_RES, ry+row_h-2, sep_col);

            // DONG 1: TEN (Scale 2 - Lon, cho phep dai den 19 ky tu)
            char nm[20]; strncpy(nm, s_log_rows[ri].name, 19); nm[19]=0;
            fb_draw_str(fb, 4, ry+4, nm, text_col, rb, 2);

            // DONG 2: GIO | NGAY | MA (Scale 1 - Nho)
            char sub[64];
            char idv[12]; strncpy(idv, s_log_rows[ri].id, 11); idv[11]=0;
            snprintf(sub, sizeof(sub), "%s %s  Ma:%s", s_log_rows[ri].time, s_log_rows[ri].date, idv);
            fb_draw_str(fb, 4, ry+32, sub, dim_col, rb, 1);
        }

        // Navigation buttons
        int nav_y = 356;  // sau 5 rows: 64 + 5*58 = 354
        int nav_h = 36;
        if (s_fb_y_offset + s_fb_chunk_h > nav_y && s_fb_y_offset <= nav_y + nav_h) {
            if (page > 0) {
                fb_draw_rect_filled(fb, 5, nav_y, 145, nav_h, nav_bg);
                fb_draw_line(fb, 5, nav_y, 150, nav_y, RGB565(100,150,255));
                fb_draw_str(fb, 10, nav_y+10, "< TRUOC", dim_col, nav_bg, 2);
            }
            if (s_log_count == LOG_ROWS_PER_PAGE) {
                fb_draw_rect_filled(fb, 165, nav_y, 150, nav_h, nav_bg);
                fb_draw_line(fb, 165, nav_y, 315, nav_y, RGB565(100,150,255));
                fb_draw_str(fb, 170, nav_y+10, "SAU >", dim_col, nav_bg, 2);
            }
        }

        // Back button
        int back_y = 436;
        int back_h = 40;
        if (s_fb_y_offset + s_fb_chunk_h > back_y && s_fb_y_offset <= back_y + back_h) {
            fb_draw_rect_filled(fb, 20, back_y, 280, back_h, back_bg);
            fb_draw_line(fb, 20, back_y, 300, back_y, RGB565(255, 100, 100));
            fb_draw_str_centered(fb, back_y+12, "QUAY LAI", RGB565(255,255,255), back_bg, 2);
        }

        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h  = BOARD_LCD_V_RES;
}

/* ============================================================
 * MAN HINH DANH SACH THE (state 9 / state 10)
 * ============================================================ */

#include "card_profile.h"

#define CARD_ROW_H 58

static CardProfileEntry_t s_card_entries[CARD_PROFILE_PAGE_ROWS];
static int                s_card_count      = 0;
static int                s_card_total      = 0;
static int                s_card_page_cache = -99;
static bool               s_card_unreg_cache = false;

void lcd_ui_invalidate_card_cache(void) { s_card_page_cache = -99; }

int lcd_ui_card_list_total(void) { return s_card_total; }

static void card_list_load(int page, bool only_unreg)
{
    if (s_card_page_cache == page && s_card_unreg_cache == only_unreg) {
        return;
    }
    s_card_total = card_profile_count_matched(only_unreg);
    int max_page = (s_card_total <= 0) ? 0 : (s_card_total - 1) / CARD_PROFILE_PAGE_ROWS;
    if (page > max_page) {
        g_card_page = max_page;
        page        = max_page;
    }
    s_card_page_cache  = page;
    s_card_unreg_cache = only_unreg;
    int skip = page * CARD_PROFILE_PAGE_ROWS;
    s_card_count = card_profile_list_page(s_card_entries, CARD_PROFILE_PAGE_ROWS, only_unreg, skip);
}

void lcd_ui_show_card_list(int page, bool only_unregistered)
{
    if (!s_panel) return;
    card_list_load(page, only_unregistered);
    const int pg = g_card_page;

    const int lpc  = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t cpx = (size_t)BOARD_LCD_H_RES * lpc;
    uint16_t *fb = lcd_ui_alloc_fb(cpx);
    if (!fb) return;

    uint16_t bg     = RGB565(15, 15, 25);
    uint16_t hdr_bg = only_unregistered ? RGB565(160, 60, 20) : RGB565(20, 80, 150);
    uint16_t re     = RGB565(25, 30, 48);
    uint16_t ro     = RGB565(38, 42, 65);
    uint16_t sep    = RGB565(55, 65, 95);
    uint16_t tc     = RGB565(220, 230, 255);
    uint16_t dim    = RGB565(140, 155, 185);
    uint16_t accent = RGB565(80, 190, 255);
    uint16_t del_bg = RGB565(160, 30, 30);
    uint16_t nav_bg = RGB565(45, 50, 75);
    uint16_t bck_bg = RGB565(80, 20, 20);

    const char *title = only_unregistered ? "THE LA (Chua DK)" : "DANH SACH THE";
    int hdr_h  = 48;
    int col_h  = 16;
    int rs     = hdr_h + col_h;   /* row start y = 64 */
    int nav_y  = rs + CARD_PROFILE_PAGE_ROWS * CARD_ROW_H;  /* 64 + 290 = 354 */
    int nav_h  = 36;
    int bck_y  = 436;
    int bck_h  = 40;

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lpc) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h  = lpc;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES)
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++)
            fb[i] = lcd_rgb565_bus(bg);

        /* Header */
        if (s_fb_y_offset < hdr_h) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, hdr_h, hdr_bg);
            fb_draw_line(fb, 0, hdr_h-1, BOARD_LCD_H_RES, hdr_h-1, accent);
            char ttl[40]; snprintf(ttl, sizeof(ttl), "%s (tr.%d)", title, pg + 1);
            fb_draw_str_centered(fb, 14, ttl, RGB565(255,255,255), hdr_bg, 2);
        }
        /* Column header - simplified */
        if (s_fb_y_offset + s_fb_chunk_h > hdr_h && s_fb_y_offset <= hdr_h + col_h) {
            uint16_t ch = RGB565(25, 55, 115);
            fb_draw_rect_filled(fb, 0, hdr_h, BOARD_LCD_H_RES, col_h, ch);
            fb_draw_str(fb, 4, hdr_h+2, "THONG TIN THE", accent, ch, 1);
            fb_draw_str(fb, 280, hdr_h+2, "XOA", del_bg, ch, 1);
        }
        /* Empty */
        if (s_card_count == 0) {
            int ey = rs + 20;
            if (s_fb_y_offset + s_fb_chunk_h > ey && s_fb_y_offset <= ey+30)
                fb_draw_str_centered(fb, ey, sd_card_is_mounted() ? "Khong co the nao" : "SD chua gan", dim, bg, 2);
        }
        /* Rows */
        for (int ri = 0; ri < s_card_count; ri++) {
            int ry = rs + ri * CARD_ROW_H;
            if (s_fb_y_offset + s_fb_chunk_h <= ry) break;
            if (s_fb_y_offset > ry + CARD_ROW_H) continue;
            uint16_t rb = (ri % 2 == 0) ? re : ro;
            fb_draw_rect_filled(fb, 0, ry, BOARD_LCD_H_RES, CARD_ROW_H-2, rb);
            fb_draw_line(fb, 0, ry+CARD_ROW_H-2, BOARD_LCD_H_RES, ry+CARD_ROW_H-2, sep);

            // DONG 1: TEN (Scale 2 - Gioi han 16 ky tu de khong cham nut XOA)
            char nm[18]; strncpy(nm, s_card_entries[ri].name[0] ? s_card_entries[ri].name : "(Chua dat ten)", 16); nm[16]=0;
            fb_draw_str(fb, 4, ry+4, nm, s_card_entries[ri].registered ? tc : accent, rb, 2);

            // DONG 2: MA | UID (Scale 1, nho)
            char sub[64];
            char idv[16]; strncpy(idv, s_card_entries[ri].id[0] ? s_card_entries[ri].id : "---", 15); idv[15]=0;
            char uidv[12]; strncpy(uidv, s_card_entries[ri].uid, 11); uidv[11]=0;
            snprintf(sub, sizeof(sub), "ID:%s | UID:%s", idv, uidv);
            fb_draw_str(fb, 4, ry+34, sub, dim, rb, 1);

            // Nut X xoa: x=278, w=36, cao bang ca 2 dong
            fb_draw_rect_filled(fb, 278, ry+4, 38, CARD_ROW_H-8, del_bg);
            fb_draw_str(fb, 289, ry+20, "X", RGB565(255,255,255), del_bg, 2);
        }
        /* Nav */
        if (s_fb_y_offset + s_fb_chunk_h > nav_y && s_fb_y_offset <= nav_y + nav_h) {
            if (pg > 0) {
                fb_draw_rect_filled(fb, 5, nav_y, 145, nav_h, nav_bg);
                fb_draw_str(fb, 10, nav_y+10, "< TRUOC", dim, nav_bg, 2);
            }
            if (pg * CARD_PROFILE_PAGE_ROWS + s_card_count < s_card_total && s_card_total > 0) {
                fb_draw_rect_filled(fb, 165, nav_y, 150, nav_h, nav_bg);
                fb_draw_str(fb, 170, nav_y+10, "SAU >", dim, nav_bg, 2);
            }
        }
        /* Back */
        if (s_fb_y_offset + s_fb_chunk_h > bck_y && s_fb_y_offset <= bck_y + bck_h) {
            fb_draw_rect_filled(fb, 20, bck_y, 280, bck_h, bck_bg);
            fb_draw_line(fb, 20, bck_y, 300, bck_y, RGB565(255,100,100));
            fb_draw_str_centered(fb, bck_y+12, "QUAY LAI", RGB565(255,255,255), bck_bg, 2);
        }
        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h  = BOARD_LCD_V_RES;
}

/* ============================================================
 * MAN HINH FORM CHINH SUA THE (state 11)
 * ============================================================ */
void lcd_ui_show_card_edit(const char *uid, const char *name, const char *id, int active_field)
{
    if (!s_panel) return;
    const int lpc  = BOARD_LCD_SPI_CHUNK_LINES;
    const size_t cpx = (size_t)BOARD_LCD_H_RES * lpc;
    uint16_t *fb = lcd_ui_alloc_fb(cpx);
    if (!fb) return;

    uint16_t bg      = RGB565(15, 15, 25);
    uint16_t hdr_bg  = RGB565(40, 20, 120);
    uint16_t box_act = RGB565(50, 80, 140);
    uint16_t box_bg  = RGB565(35, 40, 60);
    uint16_t tc      = RGB565(230, 235, 255);
    uint16_t btn_bg  = RGB565(45, 52, 78);
    uint16_t accent  = RGB565(80, 190, 255);

    int qy       = 168;  /* duoi o MA (162), cung le san voi man WiFi */
    int qkey_h   = 42;
    int qstep    = 50;   /* key_h + sp_y */

    for (int y_off = 0; y_off < BOARD_LCD_V_RES; y_off += lpc) {
        s_fb_y_offset = y_off;
        s_fb_chunk_h  = lpc;
        if (s_fb_y_offset + s_fb_chunk_h > BOARD_LCD_V_RES)
            s_fb_chunk_h = BOARD_LCD_V_RES - s_fb_y_offset;
        for (size_t i = 0; i < (size_t)BOARD_LCD_H_RES * s_fb_chunk_h; i++)
            fb[i] = lcd_rgb565_bus(bg);

        /* Header */
        if (s_fb_y_offset < 48) {
            fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 48, hdr_bg);
            fb_draw_line(fb, 0, 47, BOARD_LCD_H_RES, 47, accent);
            char ttl[48]; snprintf(ttl, sizeof(ttl), "THE: %.10s", uid ? uid : "");
            fb_draw_str_centered(fb, 14, ttl, RGB565(255,255,255), hdr_bg, 2);
        }
        /* Box Name */
        if (s_fb_y_offset + s_fb_chunk_h > 56 && s_fb_y_offset <= 106) {
            uint16_t cb = (active_field == 0) ? box_act : box_bg;
            fb_draw_rect_filled(fb, 5, 56, 310, 48, cb);
            if (active_field == 0) fb_draw_line(fb, 5, 104, 315, 104, accent);
            fb_draw_str(fb, 10, 58, "TEN:", accent, cb, 2);
            const char *shown = (name && name[0]) ? name : "...";
            fb_draw_str(fb, 65, 60, shown, tc, cb, 2);
        }
        /* Box ID */
        if (s_fb_y_offset + s_fb_chunk_h > 114 && s_fb_y_offset <= 162) {
            uint16_t cb = (active_field == 1) ? box_act : box_bg;
            fb_draw_rect_filled(fb, 5, 114, 310, 48, cb);
            if (active_field == 1) fb_draw_line(fb, 5, 162, 315, 162, accent);
            fb_draw_str(fb, 10, 116, "MA: ", accent, cb, 2);
            const char *shown = (id && id[0]) ? id : "...";
            fb_draw_str(fb, 65, 118, shown, tc, cb, 2);
        }
        /* Giong WiFi: ^ + chu/so + 3 trang SYM + Huy|SYM|SPACE|Luu */
        int nk[6] = {10,10,9,9,10,4};
        int sx[6] = {10,10,25,10,10,11};
        int kw[6] = {28,28,28,28,28,70};
        int ksp[6]= {2, 2, 2, 2, 2, 6};
        const char *km_row[4][10] = {
            {"1","2","3","4","5","6","7","8","9","0"},
            {"Q","W","E","R","T","Y","U","I","O","P"},
            {"A","S","D","F","G","H","J","K","L",""},
            {"^","Z","X","C","V","B","N","M","<",""},
        };
        const char *km_bot[4] = {"Huy", "SYM", "SPACE", "Luu"};
        for (int r = 0; r < 6; r++) {
            int by = qy + r * qstep;
            if (s_fb_y_offset + s_fb_chunk_h > by && s_fb_y_offset <= by + qkey_h) {
                for (int c = 0; c < nk[r]; c++) {
                    int bx = sx[r] + c * (kw[r] + ksp[r]);
                    uint16_t kb = btn_bg;
                    char disp_one[8];
                    const char *draw = "";
                    const char *key = "";

                    if (r == 5) {
                        key = km_bot[c];
                        draw = key;
                        if (strcmp(key,"Huy")==0) kb = RGB565(160,30,30);
                        else if (strcmp(key,"Luu")==0) kb = RGB565(30,160,30);
                        else if (strcmp(key,"SYM")==0) kb = RGB565(130, 95, 40);
                    } else if (r == 4) {
                        snprintf(disp_one, sizeof(disp_one), "%c", (char)g_soft_sym_chars[g_soft_kb_sym_page][c]);
                        draw = disp_one;
                    } else {
                        key = km_row[r][c];
                        draw = key;
                        if (strcmp(key,"<")==0 || strcmp(key,"Huy")==0) kb = RGB565(160,30,30);
                        if (strcmp(key, "^") == 0 && r == 3 && c == 0 && g_soft_kb_upper) kb = RGB565(70, 110, 170);
                        if ((unsigned char)key[0] >= 'A' && (unsigned char)key[0] <= 'Z' && key[1] == '\0') {
                            disp_one[0] = g_soft_kb_upper ? key[0] : (char)tolower((unsigned char)key[0]);
                            disp_one[1] = '\0';
                            draw = disp_one;
                        }
                    }
                    fb_draw_rect_filled(fb, bx, by, kw[r], qkey_h, kb);
                    fb_draw_line(fb, bx, by, bx+kw[r], by, RGB565(140,150,175));
                    int tlen = strlen(draw);
                    int tx = bx + kw[r]/2 - tlen*4;
                    fb_draw_str(fb, tx, by+13, draw, tc, kb, 2);
                }
            }
        }
        lcd_ui_draw_chunk_wait(s_panel, 0, s_fb_y_offset, BOARD_LCD_H_RES, s_fb_y_offset + s_fb_chunk_h, fb);
    }
    heap_caps_free(fb);
    s_fb_y_offset = 0;
    s_fb_chunk_h  = BOARD_LCD_V_RES;
}

void lcd_ui_show_confirm_screen(int type, const char *arg1, const char *arg2) {
    if (!s_panel) return;
    uint16_t bg = RGB565(20, 20, 40);
    uint16_t tc = RGB565(255, 255, 255);
    uint16_t hdr_bg = RGB565(180, 50, 50); // Warning red by default
    const char *title = "XAC NHAN";
    const char *msg1 = "";
    const char *msg2 = "";

    if (type == 1) {
        title = "XOA THE";
        msg1 = "Ban chac chan xoa the nay?";
        msg2 = arg1 ? arg1 : "";
    } else if (type == 2) {
        title = "LUU THE";
        msg1 = "Ban chac chan luu the?";
        hdr_bg = RGB565(50, 150, 50); // Green for save
    } else if (type == 3) {
        title = "XOA WIFI";
        msg1 = "Ban chac chan xoa mang?";
        msg2 = arg1 ? arg1 : "";
    } else if (type == 4) {
        title = "LUU WIFI";
        msg1 = "Ban chac chan luu mang?";
        msg2 = arg1 ? arg1 : "";
        hdr_bg = RGB565(50, 150, 50); // Green for save
    } else if (type == 5) {
        title = "DOI MAT KHAU";
        msg1 = "Xac nhan doi mat khau?";
        hdr_bg = RGB565(50, 150, 50); // Green for save
    }

    uint16_t *fb = lcd_ui_alloc_fb((size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES);
    if (!fb) return;

    for (int y = 0; y < BOARD_LCD_V_RES; y++) {
        for (int x = 0; x < BOARD_LCD_H_RES; x++) {
            fb[y * BOARD_LCD_H_RES + x] = lcd_rgb565_bus(bg);
        }
    }
    
    s_fb_y_offset = 0;
    s_fb_chunk_h = BOARD_LCD_V_RES;

    // Header
    fb_draw_rect_filled(fb, 0, 0, BOARD_LCD_H_RES, 40, hdr_bg);
    fb_draw_str_centered(fb, 12, title, tc, hdr_bg, 2);

    // Messages
    fb_draw_str_centered(fb, 100, msg1, RGB565(255, 200, 100), bg, 2);
    if (msg2[0]) {
        fb_draw_str_centered(fb, 140, msg2, tc, bg, 2);
    }
    if (type == 1 && arg2 && arg2[0]) { // Show name for card delete
        fb_draw_str_centered(fb, 180, arg2, RGB565(150, 200, 255), bg, 2);
    }

    // Buttons
    // NO button
    uint16_t btn_no_bg = RGB565(180, 50, 50);
    fb_draw_rect_filled(fb, 30, 280, 110, 70, btn_no_bg);
    int no_w = strlen("HUY") * 16;
    fb_draw_str(fb, 30 + (110 - no_w)/2, 307, "HUY", tc, btn_no_bg, 2);

    // YES button
    uint16_t btn_yes_bg = RGB565(50, 180, 50);
    fb_draw_rect_filled(fb, 180, 280, 110, 70, btn_yes_bg);
    int yes_w = strlen("DONG Y") * 16;
    fb_draw_str(fb, 180 + (110 - yes_w)/2, 307, "DONG Y", tc, btn_yes_bg, 2);

    lcd_ui_draw_chunk_wait(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, fb);
    heap_caps_free(fb);
}
