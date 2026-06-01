#include "sd_card.h"

#include <sys/stat.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

static sdmmc_card_t *s_card;
static bool s_mounted;
static SemaphoreHandle_t s_sd_mutex;
static bool s_spi_bus_inited;

static void sd_mutex_ensure(void)
{
    if (!s_sd_mutex) {
        s_sd_mutex = xSemaphoreCreateRecursiveMutex();
    }
}

esp_err_t sd_spi_bus_ensure_init(void)
{
    if (s_spi_bus_inited) {
        sd_mutex_ensure();
        return ESP_OK;
    }

    gpio_pullup_en(BOARD_SD_CS_GPIO);
    gpio_set_direction(BOARD_SD_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_SD_CS_GPIO, 1);
#if BOARD_RC522_SHARE_SD_SPI_BUS
    gpio_pullup_en(RC522_CS_GPIO);
    gpio_set_direction(RC522_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RC522_CS_GPIO, 1);
#endif
    gpio_pullup_en(BOARD_SD_MOSI_GPIO);
    gpio_pullup_en(BOARD_SD_MISO_GPIO);
    /* Không kéo pull-up SCK: dễ làm méo cạnh xung ở bus dài / MHz cao, ảnh hưởng cả RC522. */
    gpio_pullup_dis(BOARD_SD_SCK_GPIO);

    vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_SD_MOSI_GPIO,
        .miso_io_num = BOARD_SD_MISO_GPIO,
        .sclk_io_num = BOARD_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SD_SPI_MAX_TRANSFER_SZ,
    };

    esp_err_t ret = spi_bus_initialize(BOARD_SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret == ESP_ERR_INVALID_STATE) {
        s_spi_bus_inited = true;
        sd_mutex_ensure();
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }
    s_spi_bus_inited = true;
    sd_mutex_ensure();
    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_err_t ret = sd_spi_bus_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    sd_card_lock();
    /* Cắm module / nguồn yếu: thêm ổn định trước CMD8 (0x108 khi thẻ chưa sẵn sàng). */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SD_SPI_HOST;
    host.max_freq_khz = BOARD_SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    /* Đưa thẻ về idle (CS cao), chờ SPI & nguồn ổn — giảm CID/CRC lỗi sau WiFi/MQTT. */
    gpio_set_level(BOARD_SD_CS_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOARD_SD_PRE_MOUNT_DELAY_MS));

    ret = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    /* Một lần retry SPI chậm: INVALID_CRC / timeout thường do MHz cao hoặc nhiễu bus. */
    if (ret != ESP_OK && (ret == ESP_ERR_INVALID_CRC || ret == ESP_ERR_TIMEOUT)) {
        const uint32_t slow_khz = (BOARD_SD_SPI_MAX_FREQ_KHZ > 2500) ? 2500u : BOARD_SD_SPI_MAX_FREQ_KHZ;
        ESP_LOGW(TAG, "mount loi %s — thu lai SPI %lu kHz", esp_err_to_name(ret), (unsigned long)slow_khz);
        gpio_set_level(BOARD_SD_CS_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        host.max_freq_khz = (int)slow_khz;
        ret = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount %s: %s (the van co the doc duoc tren PC — CRC mount khong xoa FAT)",
                 BOARD_SD_MOUNT_POINT, esp_err_to_name(ret));
        sd_card_unlock();
        /* Giữ bus SPI3: RC522 có thể đang dùng chung SPI3 với SD (BOARD_RC522_SHARE_SD_SPI_BUS). */
        return ret;
    }

    s_mounted = true;
    (void)mkdir(BOARD_SD_AUDIO_DIR, 0775);
    (void)mkdir(BOARD_SD_CHECKIN_DIR, 0775);
    (void)mkdir(BOARD_SD_PROFILES_DIR, 0775);
    ESP_LOGI(TAG, "SD OK %s (audio/checkin/profiles ok)", BOARD_SD_MOUNT_POINT);
    sd_card_unlock();
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

void sd_card_lock(void)
{
    if (s_sd_mutex) {
        if (xSemaphoreTakeRecursive(s_sd_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE("sd_card", "Lock timeout! Mutex held by other task too long.");
        }
    }
}

void sd_card_unlock(void)
{
    if (s_sd_mutex) {
        xSemaphoreGiveRecursive(s_sd_mutex);
    }
}
