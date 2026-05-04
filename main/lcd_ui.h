#pragma once

#include "board_pins.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>

/** Số cột ký tự 8px vừa một hàng (không tràn ngang). */
#define LCD_UI_MAX_CHARS_PER_LINE ((BOARD_LCD_H_RES) / 8)

esp_err_t lcd_ui_init(void);
void lcd_ui_show_lines(const char *line1, const char *line2);
/** Mot dong, canh giua man hinh (vd: "Hello") */
void lcd_ui_show_centered(const char *text);

void lcd_ui_clear_screen(void);
/** line1: ma, line2: ten, line3: thoi gian */
void lcd_ui_show_attendance(const char *line1, const char *line2, const char *line3, int azure_st, int check_type);
/** NULL neu lcd_ui_init chua thanh cong */
esp_lcd_panel_handle_t lcd_ui_get_panel(void);

/**
 * Vẽ bitmap rồi chờ SPI/DMA hoàn tất — tránh đầy trans_queue của esp_lcd khi vẽ nhiều lần liên tiếp.
 */
esp_err_t lcd_ui_draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, const void *color_data);

/** Bat/tat den nen man hinh de tiet kiem pin / ngu */
void lcd_ui_set_backlight(bool on);

/**
 * Hiển thị màn hình chờ (Flicker-Free bằng Framebuffer): 
 * Dòng 1: Máy chấm công
 * Dòng 2: Trạng thái wifi
 * Dòng 3: Cột sóng WiFi (RSSI)
 * Dòng 4: Đồng hồ Analog + Kỹ thuật số + Ngày tháng
 */
void lcd_ui_show_idle_screen(const char *title, const char *wifi_status, int rssi, int azure_st, int d, int mo, int y, int h, int m, int s);
void lcd_ui_show_login_screen(const char *entered_pin, bool is_error);
void lcd_ui_show_menu_screen(int active_item);
void lcd_ui_show_settings_menu(int active_item);
void lcd_ui_show_change_password(const char *old_pin, const char *new_pin, int active_field, bool is_error);
void lcd_ui_show_wifi_manager(int state, const char *selected_ssid, const char *entered_pass);
void lcd_ui_show_log_screen(int page);
/** Goi sau khi file nhat ky tren SD thay doi (xoa ban ghi cu) de man LCD doc lai. */
void lcd_ui_invalidate_log_cache(void);
/** State 9 (only_unregistered=false) hoac 10 (only_unregistered=true) */
void lcd_ui_show_card_list(int page, bool only_unregistered);
/** Tong so the khop bo loc (cap nhat khi ve danh sach the). */
int lcd_ui_card_list_total(void);
/** State 11: form chinh sua/them moi */
void lcd_ui_show_card_edit(const char *uid, const char *name, const char *id, int active_field);
/** State 12: xac nhan chung */
void lcd_ui_show_confirm_screen(int type, const char *arg1, const char *arg2);

#ifdef __cplusplus
}
#endif
