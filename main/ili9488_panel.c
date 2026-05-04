/*
 * ILI9488 — SPI 4 dây (DC + CS), RGB565. Chuỗi init gần TFT_eSPI ILI9488 / fbcp-ili9488.
 */

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "board_pins.h"
#include "ili9488_panel.h"

static const char *TAG = "lcd_panel.ili9488";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
    bool spi_rgb666;
    uint8_t *rgb666_buf;
    size_t rgb666_buf_bytes;
} ili9488_panel_t;

static esp_err_t panel_ili9488_del(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9488_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9488_init(esp_lcd_panel_t *panel);
static esp_err_t panel_ili9488_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *color_data);
static esp_err_t panel_ili9488_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_ili9488_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_ili9488_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_ili9488_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_ili9488_disp_on_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t panel_ili9488_sleep(esp_lcd_panel_t *panel, bool sleep);

static void ili9488_rgb565_to_rgb666(const uint16_t *src, uint8_t *dst, size_t n)
{
    for (size_t i = 0, j = 0; i < n; i++) {
#if BOARD_LCD_RGB565_SWAP_BYTES
        uint16_t p = __builtin_bswap16(src[i]);
#else
        uint16_t p = src[i];
#endif
        dst[j++] = (uint8_t)(((p & 0xF800) >> 8) | ((p & 0x8000) >> 13));
        dst[j++] = (uint8_t)((p & 0x07E0) >> 3);
        dst[j++] = (uint8_t)(((p & 0x001F) << 3) | ((p & 0x0010) >> 2));
    }
}

#if BOARD_LCD_PANEL_PROFILE == 1
/**
 * ILI9486 SPI: shift register 16-bit — mỗi byte param/pixel phải pad thành
 * 2 byte (0x00, value) trước khi gửi lên bus. TFT_eSPI cũng làm vậy.
 */
static void ili9486_pad16(const uint8_t *src, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = 0x00;
        dst[i * 2 + 1] = src[i];
    }
}

/** Gửi param đã pad 16-bit cho ILI9486. Stack buf tối đa 64 byte (32 param). */
static esp_err_t ili9486_tx_param16(esp_lcd_panel_io_handle_t io, int cmd,
                                    const uint8_t *param, size_t param_len)
{
    if (!param || param_len == 0) {
        return esp_lcd_panel_io_tx_param(io, cmd, NULL, 0);
    }
    uint8_t buf[64];
    if (param_len > 32) param_len = 32;
    ili9486_pad16(param, buf, param_len);
    return esp_lcd_panel_io_tx_param(io, cmd, buf, param_len * 2);
}

/** Chuyển RGB565 → 18-bit parallel (16-bit bus). 2 pixel gộp thành 3 transfer (6 byte). */
static void ili9486_rgb565_to_18bit_parallel(const uint16_t *src, uint8_t *dst, size_t n_pixels)
{
    // Cứ 2 pixel (src) sẽ tạo ra 3 transfer 16-bit (6 byte dst).
    // Transfer 1: R1 (High), G1 (Low)
    // Transfer 2: B1 (High), R2 (Low)
    // Transfer 3: G2 (High), B2 (Low)
    // SPI gửi: Byte 0 (High), Byte 1 (Low)
    for (size_t i = 0; i < n_pixels / 2; i++) {
        uint16_t p1 = src[i * 2];
        uint16_t p2 = src[i * 2 + 1];

#if BOARD_LCD_RGB565_SWAP_BYTES
        p1 = __builtin_bswap16(p1);
        p2 = __builtin_bswap16(p2);
#endif

        uint8_t r1 = (uint8_t)(((p1 & 0xF800) >> 8) | ((p1 & 0x8000) >> 13));
        uint8_t g1 = (uint8_t)((p1 & 0x07E0) >> 3);
        uint8_t b1 = (uint8_t)(((p1 & 0x001F) << 3) | ((p1 & 0x0010) >> 2));

        uint8_t r2 = (uint8_t)(((p2 & 0xF800) >> 8) | ((p2 & 0x8000) >> 13));
        uint8_t g2 = (uint8_t)((p2 & 0x07E0) >> 3);
        uint8_t b2 = (uint8_t)(((p2 & 0x001F) << 3) | ((p2 & 0x0010) >> 2));

        size_t j = i * 6;
        dst[j]   = r1; dst[j+1] = g1; // Transfer 1
        dst[j+2] = b1; dst[j+3] = r2; // Transfer 2
        dst[j+4] = g2; dst[j+5] = b2; // Transfer 3
    }
}
#endif /* BOARD_LCD_PANEL_PROFILE == 1 */

/**
 * x_end, y_end giống esp_lcd — không bao gồm điểm cuối.
 * Khi MADCTL có MV (swap_xy), nhiều ILI9488 cần đổi chỗ phạm vi gửi cho 0x2A / 0x2B.
 */
static esp_err_t ili9488_send_addr_window(esp_lcd_panel_io_handle_t io, int x_start, int x_end, int y_start, int y_end,
                                         bool swap_xy)
{
    const int xs = x_start;
    const int xe_incl = x_end - 1;
    const int ys = y_start;
    const int ye_incl = y_end - 1;

    int c0s, c0e, c1s, c1e;
    if (!swap_xy) {
        c0s = xs; c0e = xe_incl; c1s = ys; c1e = ye_incl;
    } else {
        c0s = ys; c0e = ye_incl; c1s = xs; c1e = xe_incl;
    }

#if BOARD_LCD_PANEL_PROFILE == 1
    /* ILI9486 16-bit SPI: mỗi byte addr phải pad 16-bit */
    uint8_t ca[4] = { (uint8_t)((c0s >> 8) & 0xFF), (uint8_t)(c0s & 0xFF),
                      (uint8_t)((c0e >> 8) & 0xFF), (uint8_t)(c0e & 0xFF) };
    uint8_t ra[4] = { (uint8_t)((c1s >> 8) & 0xFF), (uint8_t)(c1s & 0xFF),
                      (uint8_t)((c1e >> 8) & 0xFF), (uint8_t)(c1e & 0xFF) };
    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, LCD_CMD_CASET, ca, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, LCD_CMD_RASET, ra, 4), TAG, "raset");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (uint8_t)((c0s >> 8) & 0xFF), (uint8_t)(c0s & 0xFF),
        (uint8_t)((c0e >> 8) & 0xFF), (uint8_t)(c0e & 0xFF),
    }, 4), TAG, "caset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (uint8_t)((c1s >> 8) & 0xFF), (uint8_t)(c1s & 0xFF),
        (uint8_t)((c1e >> 8) & 0xFF), (uint8_t)(c1e & 0xFF),
    }, 4), TAG, "raset");
#endif
    return ESP_OK;
}

/** TFT_eSPI ILI9486_Init.h — nhánh SPI 16-bit: mỗi byte param pad thành 16-bit. */
static esp_err_t ili9486_send_init_spi(ili9488_panel_t *ili9488)
{
#if BOARD_LCD_PANEL_PROFILE != 1
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_lcd_panel_io_handle_t io = ili9488->io;
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, LCD_CMD_COLMOD,
        (uint8_t[]){ ili9488->colmod_val }, 1), TAG, "colmod");

    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, 0xC0,
        (uint8_t[]){ 0x0E, 0x0E }, 2), TAG, "c0");
    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, 0xC1,
        (uint8_t[]){ 0x41, 0x00 }, 2), TAG, "c1");
    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, 0xC2,
        (uint8_t[]){ 0x55 }, 1), TAG, "c2");
    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, 0xC5,
        (uint8_t[]){ 0x00, 0x00, 0x00, 0x00 }, 4), TAG, "c5");

    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, 0xE0, (uint8_t[]){
        0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
        0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00
    }, 15), TAG, "e0");
    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, 0xE1, (uint8_t[]){
        0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
        0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00
    }, 15), TAG, "e1");

    ESP_RETURN_ON_ERROR(ili9486_tx_param16(io, LCD_CMD_MADCTL,
        (uint8_t[]){ ili9488->madctl_val }, 1), TAG, "madctl");
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
#endif
}

static esp_err_t ili9488_send_init(ili9488_panel_t *ili9488)
{
    esp_lcd_panel_io_handle_t io = ili9488->io;
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));

    /*
     * TFT_eSPI ILI9488_Init.h (Bodmer) — RGB565 với 0x3A=0x55 cho RPi / parallel-type;
     * khác chuỗi F1/F2 cũ (một số bo copy ILI9341) có thể khiến GRAM không cập nhật → màn trắng.
     */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]) {
        0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F
    }, 15), TAG, "e0");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]) {
        0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F
    }, 15), TAG, "e1");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]) { 0x17, 0x15 }, 2), TAG, "c0");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC1, (uint8_t[]) { 0x41 }, 1), TAG, "c1");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC5, (uint8_t[]) { 0x00, 0x12, 0x80 }, 3), TAG, "c5");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) { ili9488->madctl_val }, 1), TAG,
                        "madctl");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) { ili9488->colmod_val }, 1), TAG,
                        "colmod");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB0, (uint8_t[]) { BOARD_LCD_ILI9488_B0 }, 1), TAG, "b0");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB1, (uint8_t[]) { 0xA0 }, 1), TAG, "b1");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB4, (uint8_t[]) { 0x02 }, 1), TAG, "b4");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB6, (uint8_t[]) { 0x02, 0x02, 0x3B }, 3), TAG, "b6");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]) { 0xC6 }, 1), TAG, "b7");
#if BOARD_LCD_ILI9488_SPI_RGB666
    /* LVGL/atanisoft SPI: byte cuối thường 0x02 (Bodmer parallel dùng 0x82). */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF7, (uint8_t[]) { 0xA9, 0x51, 0x2C, 0x02 }, 4), TAG, "f7");
#else
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF7, (uint8_t[]) { 0xA9, 0x51, 0x2C, 0x82 }, 4), TAG, "f7");
#endif

    /* DISPON + idle off: lcd_ui sau swap_xy/mirror/invert — xem lcd_ui_init */
    vTaskDelay(pdMS_TO_TICKS(10));

    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_ili9488(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    ili9488_panel_t *ili9488 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ili9488 = calloc(1, sizeof(ili9488_panel_t));
    ESP_GOTO_ON_FALSE(ili9488, ESP_ERR_NO_MEM, err, TAG, "no mem");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "rst gpio");
    }

    ili9488->madctl_val = BOARD_ILI9488_MADCTL_BASE;
    if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        ili9488->madctl_val |= LCD_CMD_BGR_BIT;
    } else {
        ili9488->madctl_val &= (uint8_t)~LCD_CMD_BGR_BIT;
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        ili9488->fb_bits_per_pixel = 16;
#if BOARD_LCD_PANEL_PROFILE == 1
        /* ILI9486 SPI (Bodmer): 0x3A = 0x66 + RGB666 lên dây. */
        ili9488->spi_rgb666 = true;
        ili9488->colmod_val = 0x66;
#elif BOARD_LCD_ILI9488_SPI_RGB666
        ili9488->spi_rgb666 = true;
        ili9488->colmod_val = 0x66;
#else
        ili9488->spi_rgb666 = false;
        ili9488->colmod_val = BOARD_ILI9488_COLMOD;
#endif
#if BOARD_LCD_PANEL_PROFILE == 1 || BOARD_LCD_ILI9488_SPI_RGB666
#if BOARD_LCD_PANEL_PROFILE == 1
        /* ILI9486 16-bit SPI: 6 byte/pixel (RGB666 padded) */
        ili9488->rgb666_buf_bytes = (size_t)BOARD_LCD_H_RES * (size_t)BOARD_LCD_SPI_CHUNK_LINES * 6u;
#else
        ili9488->rgb666_buf_bytes = (size_t)BOARD_LCD_H_RES * (size_t)BOARD_LCD_SPI_CHUNK_LINES * 3u;
#endif
        ili9488->rgb666_buf = heap_caps_malloc(ili9488->rgb666_buf_bytes, MALLOC_CAP_DMA);
        if (!ili9488->rgb666_buf) {
            ili9488->rgb666_buf = heap_caps_malloc(ili9488->rgb666_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        }
        ESP_GOTO_ON_FALSE(ili9488->rgb666_buf, ESP_ERR_NO_MEM, err, TAG, "rgb666 dma buf");
#endif
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "bpp");
        break;
    }

    ili9488->io = io;
    ili9488->reset_gpio_num = panel_dev_config->reset_gpio_num;
    ili9488->reset_level = panel_dev_config->flags.reset_active_high;
    ili9488->base.del = panel_ili9488_del;
    ili9488->base.reset = panel_ili9488_reset;
    ili9488->base.init = panel_ili9488_init;
    ili9488->base.draw_bitmap = panel_ili9488_draw_bitmap;
    ili9488->base.invert_color = panel_ili9488_invert_color;
    ili9488->base.set_gap = panel_ili9488_set_gap;
    ili9488->base.mirror = panel_ili9488_mirror;
    ili9488->base.swap_xy = panel_ili9488_swap_xy;
    ili9488->base.disp_on_off = panel_ili9488_disp_on_off;
    ili9488->base.disp_sleep = panel_ili9488_sleep;
    *ret_panel = &(ili9488->base);
    return ESP_OK;

err:
    if (ili9488) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        if (ili9488->rgb666_buf) {
            heap_caps_free(ili9488->rgb666_buf);
        }
        free(ili9488);
    }
    return ret;
}

static esp_err_t panel_ili9488_del(esp_lcd_panel_t *panel)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (ili9488->reset_gpio_num >= 0) {
        gpio_reset_pin(ili9488->reset_gpio_num);
    }
    if (ili9488->rgb666_buf) {
        heap_caps_free(ili9488->rgb666_buf);
    }
    free(ili9488);
    return ESP_OK;
}

static esp_err_t panel_ili9488_reset(esp_lcd_panel_t *panel)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    esp_lcd_panel_io_handle_t io = ili9488->io;

    if (ili9488->reset_gpio_num >= 0) {
        gpio_set_level(ili9488->reset_gpio_num, ili9488->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(ili9488->reset_gpio_num, !ili9488->reset_level);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

static esp_err_t panel_ili9488_init(esp_lcd_panel_t *panel)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
#if BOARD_LCD_PANEL_PROFILE == 1
    return ili9486_send_init_spi(ili9488);
#else
    return ili9488_send_init(ili9488);
#endif
}

static esp_err_t panel_ili9488_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *color_data)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    esp_lcd_panel_io_handle_t io = ili9488->io;

    x_start += ili9488->x_gap;
    x_end += ili9488->x_gap;
    y_start += ili9488->y_gap;
    y_end += ili9488->y_gap;

    const int w = x_end - x_start;
    const int h = y_end - y_start;
    const uint16_t *src_u16 = (const uint16_t *)color_data;

#if BOARD_LCD_ILI9488_SWAP_ADDR_WINDOW_WITH_MV
    const bool addr_swap = (ili9488->madctl_val & LCD_CMD_MV_BIT) != 0;
#else
    const bool addr_swap = false;
#endif

    if (!ili9488->spi_rgb666) {
        ESP_RETURN_ON_ERROR(ili9488_send_addr_window(io, x_start, x_end, y_start, y_end, addr_swap), TAG, "addr");

        size_t len = (size_t)w * (size_t)h * (size_t)ili9488->fb_bits_per_pixel / 8;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "ramwr");
        return ESP_OK;
    }

    /* SPI RGB666: từng strip để vừa buffer DMA. */
    ESP_RETURN_ON_FALSE(ili9488->rgb666_buf && ili9488->rgb666_buf_bytes > 0, ESP_ERR_INVALID_STATE, TAG, "no rgb666 buf");

#if BOARD_LCD_PANEL_PROFILE == 1
    /* ILI9486 16-bit SPI: mỗi byte RGB666 → 2 byte (0x00, val) → 6 byte/pixel */
    const size_t bytes_per_pixel = 6u;
#else
    const size_t bytes_per_pixel = 3u;
#endif

    const int max_strip = BOARD_LCD_SPI_CHUNK_LINES;
    for (int row = 0; row < h;) {
        int sh = h - row;
        if (sh > max_strip) {
            sh = max_strip;
        }
        while (sh > 1 && (size_t)w * (size_t)sh * bytes_per_pixel > ili9488->rgb666_buf_bytes) {
            sh--;
        }
        ESP_RETURN_ON_FALSE((size_t)w * (size_t)sh * bytes_per_pixel <= ili9488->rgb666_buf_bytes, ESP_ERR_INVALID_STATE, TAG,
                            "rect too wide for rgb666 buf");

        const int ys = y_start + row;
        const int ye = y_start + row + sh;
        ESP_RETURN_ON_ERROR(ili9488_send_addr_window(io, x_start, x_end, ys, ye, addr_swap), TAG, "addr");

        const size_t pix = (size_t)w * (size_t)sh;
#if BOARD_LCD_PANEL_PROFILE == 1
        ili9486_rgb565_to_18bit_parallel(src_u16 + (size_t)row * (size_t)w, ili9488->rgb666_buf, pix);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, ili9488->rgb666_buf, pix * 3u), TAG, "ramwr");
#else
        ili9488_rgb565_to_rgb666(src_u16 + (size_t)row * (size_t)w, ili9488->rgb666_buf, pix);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, ili9488->rgb666_buf, pix * 3u), TAG, "ramwr");
#endif
        row += sh;
    }
    return ESP_OK;
}

static esp_err_t panel_ili9488_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    return esp_lcd_panel_io_tx_param(ili9488->io, command, NULL, 0);
}

static esp_err_t panel_ili9488_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (mirror_x) {
        ili9488->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        ili9488->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        ili9488->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        ili9488->madctl_val &= ~LCD_CMD_MY_BIT;
    }
#if BOARD_LCD_PANEL_PROFILE == 1
    return ili9486_tx_param16(ili9488->io, LCD_CMD_MADCTL, (uint8_t[]){ ili9488->madctl_val }, 1);
#else
    return esp_lcd_panel_io_tx_param(ili9488->io, LCD_CMD_MADCTL, (uint8_t[]) { ili9488->madctl_val }, 1);
#endif
}

static esp_err_t panel_ili9488_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    if (swap_axes) {
        ili9488->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        ili9488->madctl_val &= ~LCD_CMD_MV_BIT;
    }
#if BOARD_LCD_PANEL_PROFILE == 1
    return ili9486_tx_param16(ili9488->io, LCD_CMD_MADCTL, (uint8_t[]){ ili9488->madctl_val }, 1);
#else
    return esp_lcd_panel_io_tx_param(ili9488->io, LCD_CMD_MADCTL, (uint8_t[]) { ili9488->madctl_val }, 1);
#endif
}

static esp_err_t panel_ili9488_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    ili9488->x_gap = x_gap;
    ili9488->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_ili9488_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    esp_lcd_panel_io_handle_t io = ili9488->io;
    if (on_off) {
#if BOARD_LCD_IDMOFF_BEFORE_DISPON
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_IDMOFF, NULL, 0), TAG, "idmoff");
        vTaskDelay(pdMS_TO_TICKS(10));
#endif
        return esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0);
    }
    return esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPOFF, NULL, 0);
}

static esp_err_t panel_ili9488_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    ili9488_panel_t *ili9488 = __containerof(panel, ili9488_panel_t, base);
    int command = sleep ? LCD_CMD_SLPIN : LCD_CMD_SLPOUT;
    esp_err_t e = esp_lcd_panel_io_tx_param(ili9488->io, command, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    return e;
}
