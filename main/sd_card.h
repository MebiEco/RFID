#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Khởi tạo SPI bus SPI2 trùng chân SD (gọi trước mfrc522_init_spi_device khi
 * BOARD_RC522_SHARE_SD_SPI_BUS). Idempotent.
 */
esp_err_t sd_spi_bus_ensure_init(void);

/** Mount SDSPI (SPI2) mot lan — dung fopen(BOARD_SD_MOUNT_POINT/...). */
esp_err_t sd_card_mount(void);
bool sd_card_is_mounted(void);

/**
 * Mutex bảo vệ SD bus: gọi lock trước khi fopen/fread/fclose bất kỳ file SD,
 * gọi unlock ngay sau khi fclose(). Timeout = portMAX_DELAY.
 * Tránh race condition giữa app_audio_task (đọc WAV) và rfid_task (đọc profile/log).
 */
void sd_card_lock(void);
void sd_card_unlock(void);
