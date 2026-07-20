#pragma once
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo LVGL (Display, Touch, Timer, Task)
 * 
 * @param panel Handle của màn hình đã được khởi tạo bởi esp_lcd
 */
void lv_port_init(esp_lcd_panel_handle_t panel);

/** Màn idle (sau build_idle_screen) — con trỏ LVGL `lv_obj_t*`. */
void *lv_port_get_idle_scr(void);

/** Set idle screen layout version (1 = old, 2 = new) */
void lv_port_set_idle_layout(int v);

typedef struct {
    char wifi_st[40]; /* SSID hiển thị + '\0' */
    int rssi;          /* dBm từ esp_wifi_sta_get_ap_info, hoặc -127 */
    int azure_on;
    int wday; /* 0=CN … 6=T7 (SNTP đã đồng bộ); -1 khi chưa có lịch hợp lệ */
    int d, mo, y, h, m, s;
} idle_data_t;

extern idle_data_t g_idle_data;

/** @param display_name Tên nhân viên (hiển thị chính, wrap nhiều dòng). */
void lv_port_show_swipe_result(const char *display_name, const char *emp_id_line, const char *time, bool success,
                               int check_type);
/** Giữ API cũ; img_path/show_photo bị bỏ qua — chỉ hiển thị chữ. */
void lv_port_show_swipe_result_with_img(const char *display_name, const char *emp_id_line, const char *time,
                                        const char *img_path, bool show_photo, bool success, int check_type);

void lv_port_show_menu_screen(int active_item);
void lv_port_show_settings_menu(int active_item);
void lv_port_show_change_password(const char *old_pin, const char *new_pin, int active_field, bool is_error);
void lv_port_show_wifi_manager(int state, const char *selected_ssid, const char *entered_pass);
/** Quét WiFi không chặn LVGL (tránh WDT reset khi bấm "Quét thêm"). */
void lv_port_wifi_request_scan(void);
/** Gọi từ `lvgl_task` — cập nhật UI sau quét (không dùng lv_async_call từ task khác: LVGL không thread-safe). */
void lv_port_wifi_scan_poll_ui(void);
/** Gọi từ `lvgl_task` — cập nhật UI sau khi kết nối WiFi (thành công/thất bại). */
void lv_port_wifi_conn_poll_ui(void);
/** Gọi từ `lvgl_task` khi `g_force_ui_update` — tuyệt đối không gọi `lv_port_show_*` từ `rfid_task` / task khác. */
void lv_port_refresh_current_screen(void);
void lv_port_show_log_screen(int page);
void lv_port_show_card_list(int page, bool only_unregistered);
void lv_port_show_card_edit(const char *uid, const char *name, const char *id, int active_field);
void lv_port_show_confirm_screen(int type, const char *arg1, const char *arg2);
void lv_port_feed_wdt(void);

/**
 * @brief Cập nhật chữ thương hiệu trên màn idle ngay lập tức (gọi từ LVGL task).
 *        Nếu màn idle đang hiển thị sẽ cập nhật label ngay; nếu không thì ghi buffer,
 *        sẽ được áp dụng lần sau khi màn idle được rebuild.
 */
void lv_port_set_brand_text(const char *txt);

/** Hiển thị JPEG (RGB888 decode → RGB565) — parent = `lv_obj_t*` hoặc NULL = màn hiện tại. */
bool lv_port_show_jpeg_file(void *parent, const char *path, int x, int y, int max_w, int max_h);

#ifdef __cplusplus
}
#endif
