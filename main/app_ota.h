#ifndef APP_OTA_H
#define APP_OTA_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Bat dau tac vu OTA tai va nap file firmware moi tu URL
 * @param url Duong dan tai file firmware (http/https)
 */
void app_ota_start(const char *url);

/**
 * @brief Khoi dong OTA sau delay_ms (defer bang esp_timer, khong tao task phu).
 * @return true neu da len lich.
 */
bool app_ota_schedule_start(const char *url, uint32_t delay_ms);

/** Azure: len lich OTA ngan sau khi luu pending (msg_id khong dung). */
void app_ota_arm_after_mqtt_puback(const char *url, int msg_id);

/** Giu API tuong thich — khong lam gi (defer co dinh thay PUBACK). */
void app_ota_on_mqtt_published(int msg_id);

/**
 * @brief Luu yeu cau OTA ben vung va thu khoi dong OTA.
 *        Dung cho lenh tu xa (vd. Azure) de sau reset van tu tiep tuc OTA.
 * @return true neu yeu cau da duoc chap nhan.
 */
bool app_ota_request_persistent(const char *url);

/**
 * @brief Thu tiep tuc OTA dang cho trong NVS.
 * @return true neu da tim thay pending OTA va da bat dau/len lich xu ly.
 */
bool app_ota_resume_pending(void);

/** Xoa pending OTA trong NVS (thoat boot loop / huy lenh cu). */
void app_ota_clear_pending(void);

/** Goi som trong app_main: xoa pending neu boot loop / da reboot-fallback. */
void app_ota_boot_guard(void);

/** true mot lan sau reboot do OTA — da xoa co trong NVS. */
bool app_ota_take_skip_welcome(void);

/** true khi ota_task dang chay (web van mo de theo doi). */
bool app_ota_is_busy(void);

/** Tien do 0..100; -1 neu chua biet / khong dang OTA. */
int app_ota_get_progress_pct(void);

/**
 * @brief Xac nhan firmware moi chay on dinh (Cancel Rollback).
 * Neu firmware moi bi crash/reset lien tuc truoc khi goi ham nay,
 * Bootloader se tu dong rollback ve firmware cu an toan.
 */
void app_ota_validate_running_firmware(void);

/** Len lich validate sau 45s — goi tu main thay vi validate ngay luc boot. */
void app_ota_schedule_validate_delayed(void);

#endif // APP_OTA_H
