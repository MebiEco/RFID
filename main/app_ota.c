#include "app_ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "app_audio.h"
#include "app_azure.h"
#include "app_rfid.h"
#include "lv_port.h"

static const char *TAG = "app_ota";

#define OTA_NVS_NS        "app_ota"
#define OTA_NVS_SKIP_1WAV "skip_1wav"
/* Stack OTA: uu tien Internal (an toan khi ghi flash); SPIRAM fallback. */
#define OTA_TASK_STACK    20480

static volatile bool s_ota_busy;
static volatile int s_ota_pct = -1;

bool app_ota_is_busy(void)
{
    return s_ota_busy;
}

int app_ota_get_progress_pct(void)
{
    return s_ota_busy ? s_ota_pct : -1;
}

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

static void ota_log_heap(const char *when)
{
    ESP_LOGI(TAG, "Heap %s: internal_free=%u largest=%u spiram_free=%u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/** Giam dich vu nang — Azure/LVGL/RFID/audio; GIU web de theo doi. */
static void ota_release_resources(void)
{
    ESP_LOGI(TAG, "OTA: giam dich vu (giu httpd)...");
    app_audio_stop_and_clear();
    app_audio_pause();
    /* Cho MQTT PUBACK Direct Method xong roi moi ngat. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    app_azure_suspend_for_ota();
    app_rfid_set_paused(true);
    lv_port_suspend_for_ota();
    vTaskDelay(pdMS_TO_TICKS(500));
    ota_log_heap("sau giam dich vu");
}

static void ota_restore_resources(void)
{
    ESP_LOGW(TAG, "OTA fail: khoi phuc dich vu");
    lv_port_resume_after_ota();
    app_rfid_set_paused(false);
    app_azure_resume_after_ota();
    s_ota_pct = -1;
    s_ota_busy = false;
}

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Bat dau OTA tu URL: %s (running=%s)", url, run && run->label ? run->label : "?");
    s_ota_pct = 0;

    ota_release_resources();

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms = 120000,
        .buffer_size = 4096,
#if CONFIG_OTA_ALLOW_HTTP
        .skip_cert_common_name_check = true,
#endif
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };

    ESP_LOGI(TAG, "Dang tai firmware...");
    esp_https_ota_handle_t handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(ret));
        ota_restore_resources();
        free(url);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    int last_pct = -1;
    while (1) {
        ret = esp_https_ota_perform(handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int total = esp_https_ota_get_image_size(handle);
        int read = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            int pct = (int)((read * 100LL) / total);
            s_ota_pct = pct;
            if (pct != last_pct && (pct % 10) == 0) {
                last_pct = pct;
                ESP_LOGI(TAG, "OTA tien do: %d%% (%d/%d)", pct, read, total);
            }
        } else if (read > 0) {
            s_ota_pct = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (ret == ESP_OK) {
        if (!esp_https_ota_is_complete_data_received(handle)) {
            ESP_LOGE(TAG, "OTA: chua nhan du image (doc=%d size=%d)", esp_https_ota_get_image_len_read(handle),
                     esp_https_ota_get_image_size(handle));
            ret = ESP_ERR_INVALID_SIZE;
            (void)esp_https_ota_abort(handle);
            handle = NULL;
        } else {
            ESP_LOGI(TAG, "OTA: verify + set boot partition...");
            ret = esp_https_ota_finish(handle);
            handle = NULL;
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "OTA thanh cong — reboot...");
                ota_mark_skip_welcome_on_reboot();
                free(url);
                esp_restart();
            }
        }
    } else {
        if (handle) {
            (void)esp_https_ota_abort(handle);
            handle = NULL;
        }
    }

    ESP_LOGE(TAG, "OTA that bai: %s (0x%x)", esp_err_to_name(ret), (unsigned)ret);
    ota_restore_resources();
    free(url);
    vTaskDeleteWithCaps(NULL);
}

void app_ota_start(const char *url)
{
    if (!url || url[0] == '\0') {
        ESP_LOGE(TAG, "URL OTA khong hop le");
        return;
    }
    if (s_ota_busy) {
        ESP_LOGW(TAG, "OTA dang chay — bo qua yeu cau moi");
        return;
    }

    s_ota_busy = true;
    app_audio_stop_and_clear();
    app_audio_pause();

    char *url_copy = (char *)heap_caps_malloc(strlen(url) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!url_copy) {
        url_copy = strdup(url);
    } else {
        memcpy(url_copy, url, strlen(url) + 1);
    }
    if (!url_copy) {
        ESP_LOGE(TAG, "Het RAM copy URL (internal_free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_ota_busy = false;
        return;
    }

    /* Uu tien stack Internal — tranh loi khi flash cache disable luc ghi OTA. */
    BaseType_t res = xTaskCreateWithCaps(ota_task, "ota_task", OTA_TASK_STACK, url_copy, 5, NULL,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        ESP_LOGW(TAG, "ota_task INTERNAL fail — thu SPIRAM");
        res = xTaskCreateWithCaps(ota_task, "ota_task", OTA_TASK_STACK, url_copy, 5, NULL,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Khong tao duoc ota_task (int_free=%u largest=%u spiram_free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        free(url_copy);
        s_ota_busy = false;
        return;
    }

    ESP_LOGI(TAG, "ota_task da tao");
}
