#ifndef APP_OTA_H
#define APP_OTA_H

#include <stdbool.h>

/**
 * @brief Bat dau tac vu OTA tai va nap file firmware moi tu URL
 * @param url Duong dan tai file firmware (http/https)
 */
void app_ota_start(const char *url);

/** true mot lan sau reboot do OTA — da xoa co trong NVS. */
bool app_ota_take_skip_welcome(void);

/** true khi ota_task dang chay (web van mo de theo doi). */
bool app_ota_is_busy(void);

/** Tien do 0..100; -1 neu chua biet / khong dang OTA. */
int app_ota_get_progress_pct(void);

#endif // APP_OTA_H
