#pragma once

#include "esp_err.h"

/** SoftAP + web (192.168.4.1): form lưu / xóa WiFi STA vào NVS. Gọi sau nvs_flash_init. */
esp_err_t wifi_portal_start(void);

void wifi_list_add(const char *ssid, const char *pass);
void wifi_list_remove(const char *ssid);
int wifi_list_get_count(void);
void wifi_list_get_item(int idx, char *ssid, char *pass);
