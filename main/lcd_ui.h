#pragma once

#include "board_pins.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Số cột ký tự 8px vừa một hàng (tiện ích; UI chính dùng LVGL). */
#define LCD_UI_MAX_CHARS_PER_LINE ((BOARD_LCD_H_RES) / 8)

esp_err_t lcd_ui_init(void);
void lcd_ui_show_lines(const char *line1, const char *line2);
void lcd_ui_show_centered(const char *text);

void lcd_ui_clear_screen(void);
esp_lcd_panel_handle_t lcd_ui_get_panel(void);

/**
 * Vẽ bitmap rồi chờ SPI/DMA hoàn tất — dùng bởi LVGL flush và vài đường vẽ tối thiểu.
 */
esp_err_t lcd_ui_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, const void *color_data);

void lcd_ui_set_backlight(bool on);

/** Gọi khi file nhật ký trên SD thay đổi (model dùng chung với màn LVGL). */
void lcd_ui_invalidate_log_cache(void);
void lcd_ui_invalidate_card_cache(void);
/** Tổng số thẻ khớp bộ lọc (LVGL). */
int lcd_ui_card_list_total(void);

#ifdef __cplusplus
}
#endif
