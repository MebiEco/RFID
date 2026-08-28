#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Khởi tạo SPI bus SPI2 trùng chân SD (gọi trước mfrc522_init_spi_device khi
 * BOARD_RC522_SHARE_SD_SPI_BUS). Idempotent.
 */
esp_err_t sd_spi_bus_ensure_init(void);

/**
 * Cap san bounce buffer DMA Internal (goi SOM, truoc LVGL/WiFi).
 * SD doc buffer PSRAM/khong align se dung buffer nay — khong malloc tung lan
 * (tranh allocate_dma_buf 0x101). Idempotent.
 */
esp_err_t sd_card_reserve_dma_bounce(void);

/** Mount SDSPI (SPI2) mot lan — dung fopen(BOARD_SD_MOUNT_POINT/...). */
esp_err_t sd_card_mount(void);
bool sd_card_is_mounted(void);

/**
 * Mutex bảo vệ SD bus: gọi lock trước khi fopen/fread/fclose bất kỳ file SD,
 * gọi unlock ngay sau khi fclose(). Timeout = portMAX_DELAY.
 * Tránh race condition giữa app_audio_task (đọc WAV) và rfid_task (đọc profile/log).
 */
void sd_card_lock(void);
/** true neu lay duoc mutex trong timeout_ms (dung cho rfid poll — khong block lau). */
bool sd_card_try_lock(uint32_t timeout_ms);
void sd_card_unlock(void);

/**
 * Lock uu tien dich vu (quet the / Azure): danh dau dang cho SD de portal
 * huy doc nang (api/log, tong quan) thay vi giu bus lau.
 */
void sd_card_lock_service(void);

/** Co task dich vu dang block cho SD mutex (portal nen abort). */
bool sd_card_service_waiting(void);
