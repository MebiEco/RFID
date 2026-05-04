#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo và chạy bộ lập lịch Azure IoT Hub MQTT.
 * Nó sẽ đọc cấu hình từ NVS, chờ WiFi và SNTP để tạo SAS Token.
 */
void app_azure_start(void);

/**
 * Gọi từ callback SNTP khi thời gian đã đồng bộ (để azure_task dậy sớm, khỏi polling 5s).
 */
void app_azure_notify_sntp_synced(void);

/**
 * @brief Hàm hỗ trợ gửi telemetry (thông tin quẹt thẻ) lên Azure IoT
 */
void app_azure_send_telemetry(const char *uid, const char *name, const char *id, int is_registered);

/**
 * @brief Gửi sự kiện hệ thống lên Azure (vd: 603=Lưu/Sửa thẻ, 604=Xóa thẻ)
 */
void app_azure_send_card_event(const char *uid, const char *name, const char *id, int event_code);

/**
 * @brief Kiểm tra xem Azure IoT Hub đã kết nối (MQTT_EVENT_CONNECTED) thành công chưa
 */
int app_azure_is_connected(void);

#ifdef __cplusplus
}
#endif
