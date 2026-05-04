#include "sdkconfig.h"
#include "sd_wav.h"

#if !CONFIG_USE_SPIFFS_WAV

#include <stdio.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "wav_i2s.h"
#include "lcd_ui.h"

static const char *TAG = "sd_wav";

esp_err_t sd_wav_play(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#if defined(CONFIG_SD_FORMAT_IF_MOUNT_FAILED) && CONFIG_SD_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SD_SPI_HOST;
    host.max_freq_khz = 2000;

    gpio_pullup_en(BOARD_SD_MOSI_GPIO);
    gpio_pullup_en(BOARD_SD_MISO_GPIO);
    gpio_pullup_en(BOARD_SD_SCK_GPIO);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_SD_MOSI_GPIO,
        .miso_io_num = BOARD_SD_MISO_GPIO,
        .sclk_io_num = BOARD_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Khong mount duoc FAT (can format? bat format_if_mount_failed).");
        } else {
            ESP_LOGE(TAG, "esp_vfs_fat_sdspi_mount: %s", esp_err_to_name(ret));
        }
        spi_bus_free(host.slot);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD OK, phat: %s", BOARD_SD_WAV_PATH);

    lcd_ui_show_lines("SD OK", BOARD_SD_WAV_FILENAME);

    wav_i2s_play_file(BOARD_SD_WAV_PATH);

    esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, card);
    spi_bus_free(host.slot);
    ESP_LOGI(TAG, "SD unmount.");
    return ESP_OK;
}

#else

#include "esp_err.h"

esp_err_t sd_wav_play(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
