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

#endif // APP_OTA_H
