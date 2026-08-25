#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Init I2C + probe DS3231 (BOARD_DS3231_SDA/SCL). An toàn gọi 1 lần. */
esp_err_t ds3231_init(void);

bool ds3231_is_ready(void);

/**
 * Đọc giờ từ DS3231 thành Unix UTC (time_t).
 * Fail nếu chưa init, mất pin (OSF), hoặc năm < 2020.
 */
esp_err_t ds3231_get_utc(time_t *utc_out);

/** Ghi Unix UTC vào DS3231 (xóa cờ OSF). */
esp_err_t ds3231_set_utc(time_t utc);

#ifdef __cplusplus
}
#endif
