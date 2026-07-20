#include "app_ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "app_audio.h"

static const char *TAG = "app_ota";

#define OTA_NVS_NS       "app_ota"
#define OTA_NVS_SKIP_1WAV "skip_1wav"

static void ota_mark_skip_welcome_on_reboot(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, OTA_NVS_SKIP_1WAV, 1);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

bool app_ota_take_skip_welcome(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    esp_err_t e = nvs_get_u8(h, OTA_NVS_SKIP_1WAV, &v);
    if (e == ESP_OK && v) {
        (void)nvs_erase_key(h, OTA_NVS_SKIP_1WAV);
        (void)nvs_commit(h);
        nvs_close(h);
        return true;
    }
    nvs_close(h);
    return false;
}

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;
    ESP_LOGI(TAG, "Bat dau OTA cap nhat tu URL: %s", url);

    app_audio_pause();

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, // Dung bo chung chi mac dinh cua ESP-IDF
        .keep_alive_enable = true,
#if CONFIG_OTA_ALLOW_HTTP
        .skip_cert_common_name_check = true,
#endif
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    ESP_LOGI(TAG, "Dang tai firmware...");
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Cap nhat OTA thanh cong! Dang khoi dong lai thiet bi...");
        ota_mark_skip_welcome_on_reboot();
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Cap nhat OTA that bai! Ma loi: 0x%x", ret);
    }

    free(url);
    vTaskDelete(NULL);
}

void app_ota_start(const char *url)
{
    if (!url || url[0] == '\0') {
        ESP_LOGE(TAG, "URL OTA khong hop le");
        return;
    }

    char *url_copy = strdup(url);
    if (!url_copy) {
        ESP_LOGE(TAG, "Khong du bo nho cap phat cho URL copy");
        return;
    }

    // Khoi chay task OTA voi Stack 8KB de an toan cho HTTPS handshake
    BaseType_t res = xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Khong tao duoc ota_task");
        free(url_copy);
    }
}
