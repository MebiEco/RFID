#pragma once

#include <stdint.h>

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

/** Gọi sau khi Lưu/Xóa cấu hình Azure trên portal — đọc lại NVS và connect/disconnect ngay. */
void app_azure_notify_config_changed(void);

/**
 * @brief Loại sự kiện để phân biệt bộ đếm index trong NVS.
 *   MSG_IDX_SWIPE  — quẹt thẻ đã đăng ký (code 602)
 *   MSG_IDX_UNKNOWN — thẻ lạ chưa đăng ký  (code 601)
 *   MSG_IDX_ADMIN  — lưu/xóa thẻ từ web   (code 603/604)
 */
typedef enum {
    MSG_IDX_SWIPE   = 0,
    MSG_IDX_UNKNOWN = 1,
    MSG_IDX_ADMIN   = 2,
} msg_idx_type_t;

/**
 * @brief Hàm hỗ trợ gửi telemetry (thông tin quẹt thẻ) lên Azure IoT
 */
void app_azure_send_telemetry(const char *uid, const char *name, const char *id, int is_registered, int32_t msg_idx);
int32_t app_azure_get_and_increment_msg_index(msg_idx_type_t type);

/**
 * @brief Gửi sự kiện hệ thống lên Azure (vd: 603=Lưu/Sửa thẻ, 604=Xóa thẻ)
 */
void app_azure_send_card_event(const char *uid, const char *name, const char *id, int event_code,
                               int32_t msg_idx);

/**
 * @brief Kiểm tra xem Azure IoT Hub đã kết nối (MQTT_EVENT_CONNECTED) thành công chưa
 */
int app_azure_is_connected(void);

/**
 * @brief Gửi lại dữ liệu (từ file CSV cục bộ) lên Azure trong một khoảng Index
 */
int app_azure_resend_range(int code, int32_t start_idx, int32_t end_idx);

#ifdef __cplusplus
}
#endif
