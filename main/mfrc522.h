#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

/** Dùng sau mfrc522_init_bitbang — không phải handle SPI thật */
#define MFRC522_HANDLE_BITBANG ((spi_device_handle_t)(uintptr_t)0x4D4652u)

typedef enum {
    MFRC522_OK = 0,
    MFRC522_ERROR,
    MFRC522_COLLISION,
    MFRC522_TIMEOUT,
    MFRC522_NO_ROOM,
    MFRC522_INTERNAL,
    MFRC522_INVALID,
    MFRC522_CRC_WRONG,
} mfrc522_status_t;

typedef struct {
    uint8_t size;
    uint8_t uid_byte[10];
    uint8_t sak;
} mfrc522_uid_t;

/** RC522 trên SPI phần cứng (SPI2/SPI3) */
esp_err_t mfrc522_init(spi_device_handle_t spi, int rst_gpio);
/**
 * RC522 SPI bit-bang (chân riêng; SD=SPI3, LCD=SPI2 trên board_pins.h).
 * Gọi mfrc522_spi() trong mfrc522_picc_* (bit-bang hoặc SPI HW).
 */
esp_err_t mfrc522_init_bitbang(gpio_num_t sck, gpio_num_t mosi, gpio_num_t miso, gpio_num_t cs, int rst_gpio);
/** RC522 đã add_device trên bus SPI — gọi sau sd_spi_bus_ensure_init (chung bus SD). */
esp_err_t mfrc522_init_spi_device(spi_device_handle_t spi, int rst_gpio);

/** Handle SPI cho picc_* — bit-bang hoặc device sau mfrc522_init_spi_device. */
spi_device_handle_t mfrc522_spi(void);

/** Chuỗi mô tả lỗi PICC (debug serial). */
const char *mfrc522_status_name(mfrc522_status_t s);

mfrc522_status_t mfrc522_picc_is_new_card_present(spi_device_handle_t spi);
mfrc522_status_t mfrc522_picc_read_card_serial(spi_device_handle_t spi, mfrc522_uid_t *uid);
