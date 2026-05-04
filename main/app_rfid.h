#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** Hàng ký tự đặc biệt (SYM): 3 trang × 10 phím — đủ ASCII thường dùng cho MK WiFi / mã. */
#define SOFT_KB_SYM_PAGES 3
extern const uint8_t g_soft_sym_chars[SOFT_KB_SYM_PAGES][10];
extern uint8_t g_soft_kb_sym_page;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo và chạy task quy trình RFID
 */
void app_rfid_start(void);

/** PIN NVS: goi mot lan khi boot task UI (truoc khi dang nhap). */
void app_login_pin_init(void);
/** Dang nhap: neu da luu PIN trong NVS thi chi khop chuoi luu; neu chua thi chap nhan ADMIN hoac 1234. */
bool app_login_verify_pin(const char *entered);
/** Luu PIN moi (sau khi da xac minh mat khau cu trong touch_task). */
esp_err_t app_login_save_new_pin(const char *new_pin);

extern int g_ui_state;
extern int g_menu_active_item;
extern char g_login_pin[16];

extern char g_old_pin[16];
extern char g_new_pin[16];
extern int g_pwd_active_field;
extern bool g_pwd_error;

extern char g_wifi_selected_ssid[33];
extern char g_wifi_entered_pass[64];
/** Nhap MK WiFi / sua the: false = chu thuong, true = chu hoa (toggle phim ^ cung hang chu). */
extern bool g_soft_kb_upper;
extern int g_wifi_scan_count;
extern char g_wifi_scan_res[5][33];
extern int g_log_page;

// Card management (state 9, 10, 11)
extern int g_card_page;            // trang danh sach the
extern char g_edit_uid[20];        // UID dang chinh sua
extern char g_edit_name[48];       // Ten dang nhap
extern char g_edit_id[48];         // Ma dang nhap
extern int g_edit_field;           // 0=Name, 1=ID
extern int g_edit_from_state;      // 9=doi the, 10=them moi

#ifdef __cplusplus
}
#endif
