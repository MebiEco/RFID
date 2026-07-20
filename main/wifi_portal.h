#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

/** SoftAP + web (192.168.4.1): form lưu / xóa WiFi STA vào NVS. Gọi sau nvs_flash_init. */
esp_err_t wifi_portal_start(void);

void wifi_list_add(const char *ssid, const char *pass);
void wifi_list_remove(const char *ssid);
int wifi_list_get_count(void);
typedef enum {
    WIFI_STATUS_IDLE,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAIL
} wifi_conn_status_t;

void wifi_list_get_item(int idx, char *ssid, char *pass);
void wifi_portal_connect_to(const char *ssid, const char *pass);
wifi_conn_status_t wifi_portal_get_conn_status(void);

/** Azure da luu trong NVS (sas_mask chi hien 4 ky tu cuoi neu co). */
void wifi_portal_get_azure(char *host, size_t host_sz, char *devid, size_t dev_sz, char *sas_mask,
                           size_t sas_sz);

/** true sau khi SNTP cap nhat time() hop le (nam >= 2020, gio VN). */
bool wifi_portal_time_is_valid(void);

/** Giay UTC (time_t); 0 neu chua dong bo NTP hop le — dung cho TimeStamp gui backend. */
time_t wifi_portal_get_utc_sec(void);

/**
 * @brief Đọc/ghi chữ thương hiệu hiển thị trên màn idle (NVS key "brand_txt").
 *        Mặc định "MEBISOFT" nếu chưa có giá trị.
 *        @p out tối đa 16 ký tự (bao gồm '\\0').
 */
void wifi_portal_get_brand_text(char *out, size_t sz);
esp_err_t wifi_portal_set_brand_text(const char *txt);

