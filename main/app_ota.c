#include "app_ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "app_audio.h"
#include "app_azure.h"
#include "app_rfid.h"
#include "lv_port.h"
#include "lcd_ui.h"
#include "wifi_portal.h"

static const char *TAG = "app_ota";

#define OTA_NVS_NS        "app_ota"
#define OTA_NVS_SKIP_1WAV "skip_1wav"
#define OTA_NVS_PEND_URL  "pend_url"
#define OTA_NVS_PEND_RB   "pend_rb"
#define OTA_NVS_PEND_BF   "pend_bf"
#define OTA_TASK_STACK    8192
#define OTA_URL_MAX       1024
#define OTA_AZURE_DEFER_US   50000
#define OTA_VALIDATE_DELAY_US 45000000  /* 45s — tranh cancel rollback som */

static volatile bool s_ota_busy;
static volatile int s_ota_pct = -1;
static bool s_ota_resources_held_down;

static esp_timer_handle_t s_ota_defer_timer;
static esp_timer_handle_t s_ota_validate_timer;
static bool s_ota_timer_ready;
static bool s_ota_validate_timer_ready;
static char s_ota_defer_url[OTA_URL_MAX];

static void ota_defer_timer_cb(void *arg);
static void ota_kick_start(const char *url);
static bool ota_timers_init(void);
static bool ota_defer_start(uint64_t delay_us);
static bool ota_pending_exists(void);
static bool ota_pending_load(char *url_out, size_t url_sz, bool *reboot_fallback_used);
static bool ota_pending_save(const char *url, bool reboot_fallback_used);
static void ota_pending_clear(void);
static void ota_boot_fail_set(uint8_t v);
static void ota_validate_timer_cb(void *arg);

static bool ota_timers_init(void)
{
    if (s_ota_timer_ready) {
        return true;
    }
    const esp_timer_create_args_t defer_args = {
        .callback = ota_defer_timer_cb,
        .name = "ota_defer",
    };
    esp_err_t err = esp_timer_create(&defer_args, &s_ota_defer_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khong tao esp_timer OTA: %s", esp_err_to_name(err));
        return false;
    }
    s_ota_timer_ready = true;
    return true;
}

static bool ota_defer_start(uint64_t delay_us)
{
    if (!ota_timers_init()) {
        return false;
    }
    (void)esp_timer_stop(s_ota_defer_timer);
    esp_err_t err = esp_timer_start_once(s_ota_defer_timer, delay_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_once that bai: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void ota_store_defer_url(const char *url)
{
    if (!url) {
        s_ota_defer_url[0] = '\0';
        return;
    }
    strncpy(s_ota_defer_url, url, sizeof(s_ota_defer_url) - 1);
    s_ota_defer_url[sizeof(s_ota_defer_url) - 1] = '\0';
}

static void ota_kick_start(const char *url)
{
    if (!url || !url[0]) {
        ESP_LOGE(TAG, "ota_kick_start: URL rong");
        return;
    }
    if (s_ota_busy) {
        ESP_LOGI(TAG, "ota_kick_start: OTA da chay");
        return;
    }
    app_ota_start(url);
}

static void ota_defer_timer_cb(void *arg)
{
    (void)arg;
    char url[OTA_URL_MAX];
    strncpy(url, s_ota_defer_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    if (!url[0]) {
        ESP_LOGE(TAG, "OTA defer: URL trong");
        return;
    }
    ESP_LOGI(TAG, "OTA defer: khoi dong tai");
    ota_kick_start(url);
}

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

static void ota_pending_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, OTA_NVS_PEND_URL);
        (void)nvs_erase_key(h, OTA_NVS_PEND_RB);
        (void)nvs_erase_key(h, OTA_NVS_PEND_BF);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

static void ota_boot_fail_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (v == 0) {
        (void)nvs_erase_key(h, OTA_NVS_PEND_BF);
    } else {
        (void)nvs_set_u8(h, OTA_NVS_PEND_BF, v);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

void app_ota_clear_pending(void)
{
    ota_pending_clear();
}

void app_ota_boot_guard(void)
{
    /* Pending OTA tu boot truoc (TriggerOTA/crash) KHONG duoc tu resume — tranh reset vo han. */
    if (ota_pending_exists()) {
        ESP_LOGW(TAG, "Boot guard: xoa pending OTA (khong tu chay lai sau reset)");
        ota_pending_clear();
    }
    ota_boot_fail_set(0);
}

static void ota_validate_timer_cb(void *arg)
{
    (void)arg;
    app_ota_validate_running_firmware();
}

void app_ota_schedule_validate_delayed(void)
{
    /* Bỏ thời gian chờ 45s: Xác nhận hợp lệ (Cancel Rollback) ngay lập tức khi boot */
    app_ota_validate_running_firmware();
}


static bool ota_pending_save(const char *url, bool reboot_fallback_used)
{
    nvs_handle_t h;
    if (!url || !url[0]) {
        return false;
    }
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_set_str(h, OTA_NVS_PEND_URL, url) == ESP_OK &&
               nvs_set_u8(h, OTA_NVS_PEND_RB, reboot_fallback_used ? 1 : 0) == ESP_OK &&
               nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (ok && !reboot_fallback_used) {
        ota_boot_fail_set(0);
    }
    return ok;
}

static bool ota_pending_load(char *url_out, size_t url_sz, bool *reboot_fallback_used)
{
    if (!url_out || url_sz < 2) {
        return false;
    }
    url_out[0] = '\0';
    if (reboot_fallback_used) {
        *reboot_fallback_used = false;
    }
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = url_sz;
    esp_err_t e = nvs_get_str(h, OTA_NVS_PEND_URL, url_out, &sz);
    uint8_t rb = 0;
    (void)nvs_get_u8(h, OTA_NVS_PEND_RB, &rb);
    nvs_close(h);
    if (e != ESP_OK || !url_out[0]) {
        return false;
    }
    if (reboot_fallback_used) {
        *reboot_fallback_used = (rb != 0);
    }
    return true;
}

static bool ota_pending_exists(void)
{
    char url[OTA_URL_MAX];
    return ota_pending_load(url, sizeof(url), NULL);
}

static void ota_release_resources(void)
{
    if (s_ota_resources_held_down) {
        return;
    }
    ESP_LOGI(TAG, "OTA: dung dich vu khong can thiet (giu httpd)...");
    /* Tra DMA I2S ve heap truoc khi tao ota_task Internal. */
    if (!app_audio_wait_i2s_released(3000)) {
        ESP_LOGW(TAG, "OTA: I2S chua release het — tiep tuc (co the thieu Internal)");
    }
    app_audio_pause();
    app_azure_suspend_for_ota();
    (void)app_azure_wait_suspended(2500);
    vTaskDelay(pdMS_TO_TICKS(200));
    app_rfid_set_paused(true);
    lv_port_suspend_for_ota();
    vTaskDelay(pdMS_TO_TICKS(50));
    s_ota_resources_held_down = true;
    ota_log_heap("sau dung dich vu");
}

static void ota_restore_resources(void)
{
    ESP_LOGW(TAG, "OTA fail: khoi phuc dich vu");
    lv_port_resume_after_ota();
    app_rfid_set_paused(false);
    app_azure_resume_after_ota();
    app_audio_resume();
    s_ota_resources_held_down = false;
    s_ota_pct = -1;
    s_ota_busy = false;
}

static void ota_fail_exit(char *url, bool painted, const char *msg)
{
    ESP_LOGE(TAG, "OTA that bai: %s", msg ? msg : "?");
    ota_pending_clear();
    if (painted) {
        lcd_ui_show_ota_result(false, msg);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ota_restore_resources();
    free(url);
    vTaskDeleteWithCaps(NULL);
}

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Bat dau OTA tu URL: %s (running=%s)", url, run && run->label ? run->label : "?");
    s_ota_pct = 0;

    /* Image PENDING_VERIFY (cho 45s) thi esp_ota_begin se fail ROLLBACK_INVALID_STATE.
     * TriggerOTA luc nay = FW dang chay tot — xac nhan truoc khi ghi partition kia. */
    if (s_ota_validate_timer_ready) {
        (void)esp_timer_stop(s_ota_validate_timer);
    }
    app_ota_validate_running_firmware();

    ota_release_resources();

    const bool url_is_http = (strncmp(url, "http://", 7) == 0);
    const bool url_has_sig = (strstr(url, "sig=") != NULL) || (strstr(url, "Signature=") != NULL);
    const bool url_has_se = (strstr(url, "se=") != NULL);
    ESP_LOGI(TAG, "OTA URL len=%u http=%d has_sig=%d has_se=%d",
             (unsigned)strlen(url), (int)url_is_http, (int)url_has_sig, (int)url_has_se);
    if (strstr(url, "blob.core.windows.net") && !url_has_sig) {
        ESP_LOGW(TAG, "URL Azure Blob thieu sig= — thuong do shell cat tai & trong SAS. Can quote dung payload.");
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms = 30000,
        .buffer_size = 8192,
        /* Chi skip CN/SNI khi HTTP LAN debug — HTTPS Blob can SNI dung. */
#if CONFIG_OTA_ALLOW_HTTP
        .skip_cert_common_name_check = url_is_http,
#else
        .skip_cert_common_name_check = false,
#endif
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };

    ESP_LOGI(TAG, "HTTPS OTA begin (TLS toi blob)...");

    esp_https_ota_handle_t handle = NULL;
    bool painted = false;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &handle);
    ESP_LOGI(TAG, "HTTPS OTA begin xong: %s", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin that bai: %s (0x%x).", esp_err_to_name(ret), (unsigned)ret);
        ESP_LOGE(TAG, "Neu log co File not found(403): Azure Blob tu choi SAS (URL cat/& , het han, sai quyen, hoac gio thiet bi lech SNTP).");
        ota_fail_exit(url, false, esp_err_to_name(ret));
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Ket noi OTA thanh cong! Image size = %d bytes (~%.2f MB)",
             total, (float)total / (1024.0f * 1024.0f));

    int last_pct = -1;
    while (1) {
        ret = esp_https_ota_perform(handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        if (!painted) {
            lcd_ui_show_ota_progress(0, 0, total);
            painted = true;
        }
        int read = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            int pct = (int)((read * 100LL) / total);
            s_ota_pct = pct;
            if (pct != last_pct) {
                last_pct = pct;
                if ((pct % 5) == 0 || pct < 2 || pct == 100) {
                    lcd_ui_show_ota_progress(pct, read, total);
                }
                if ((pct % 10) == 0 || pct == 100) {
                    ESP_LOGI(TAG, "OTA tien do: %d%% (%d/%d bytes)", pct, read, total);
                }
            }
        } else if (read > 0) {
            s_ota_pct = -1;
            lcd_ui_show_ota_progress(-1, read, 0);
        }
    }

    if (ret == ESP_OK) {
        if (!esp_https_ota_is_complete_data_received(handle)) {
            ESP_LOGE(TAG, "OTA: chua nhan du image (doc=%d size=%d)", esp_https_ota_get_image_len_read(handle),
                     esp_https_ota_get_image_size(handle));
            (void)esp_https_ota_abort(handle);
            ota_fail_exit(url, painted, "File chua du");
            return;
        }
        ESP_LOGI(TAG, "OTA: verify + set boot partition...");
        ret = esp_https_ota_finish(handle);
        handle = NULL;
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA thanh cong — reboot...");
            ota_pending_clear();
            if (!painted) {
                lcd_ui_show_ota_progress(100, total, total);
            }
            lcd_ui_show_ota_result(true, "Dang khoi dong lai...");
            ota_mark_skip_welcome_on_reboot();
            free(url);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
    } else if (handle) {
        (void)esp_https_ota_abort(handle);
        handle = NULL;
    }

    ota_fail_exit(url, painted, esp_err_to_name(ret));
}

void app_ota_arm_after_mqtt_puback(const char *url, int msg_id)
{
    (void)msg_id;
    if (!url || !url[0]) {
        ESP_LOGE(TAG, "arm OTA: URL rong");
        return;
    }
    if (s_ota_busy) {
        ESP_LOGW(TAG, "arm OTA: da busy — pending van con trong NVS");
        return;
    }
    ota_store_defer_url(url);
    ESP_LOGI(TAG, "OTA Azure: defer %u ms", (unsigned)(OTA_AZURE_DEFER_US / 1000));
    if (!ota_defer_start(OTA_AZURE_DEFER_US)) {
        ESP_LOGW(TAG, "OTA Azure: timer defer that bai, khoi dong ngay");
        ota_kick_start(url);
    }
}

void app_ota_on_mqtt_published(int msg_id)
{
    (void)msg_id;
    /* Khong dung PUBACK/msg_id — defer co dinh 100ms dam bao luon khoi dong. */
}

bool app_ota_schedule_start(const char *url, uint32_t delay_ms)
{
    if (!url || !url[0]) {
        return false;
    }
    if (s_ota_busy) {
        ESP_LOGW(TAG, "OTA dang chay — bo qua schedule");
        return false;
    }
    ota_store_defer_url(url);
    ESP_LOGI(TAG, "OTA: defer %u ms", (unsigned)delay_ms);
    if (!ota_defer_start((uint64_t)delay_ms * 1000ULL)) {
        ota_kick_start(url);
        return s_ota_busy;
    }
    return true;
}

void app_ota_start(const char *url)
{
    if (!url || !url[0]) {
        ESP_LOGE(TAG, "URL OTA khong hop le");
        return;
    }
    if (s_ota_busy) {
        ESP_LOGW(TAG, "OTA dang chay — bo qua yeu cau moi");
        return;
    }

    s_ota_busy = true;

    /* Giai phong Azure/I2S/LVGL DMA TRUOC khi tao ota_task (can ~8KB Internal lien tuc). */
    ota_release_resources();
    ota_log_heap("truoc tao ota_task");

    char *url_copy = (char *)heap_caps_malloc(strlen(url) + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!url_copy) {
        url_copy = strdup(url);
    } else {
        memcpy(url_copy, url, strlen(url) + 1);
    }
    if (!url_copy) {
        ESP_LOGE(TAG, "Het RAM copy URL (internal_free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ota_restore_resources();
        return;
    }

    BaseType_t res = xTaskCreateWithCaps(ota_task, "ota_task", OTA_TASK_STACK, url_copy, 5, NULL,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        /* Thu stack nho hon neu Internal manh. */
        ESP_LOGW(TAG, "ota_task 8KB fail (int_free=%u largest=%u) — thu 6144",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        res = xTaskCreateWithCaps(ota_task, "ota_task", 6144, url_copy, 5, NULL,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Khong du Internal RAM tao ota_task (int_free=%u largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        free(url_copy);
        ota_pending_clear();
        ota_restore_resources();
        return;
    }

    ESP_LOGI(TAG, "ota_task da tao");
}

bool app_ota_request_persistent(const char *url)
{
    if (!url || !url[0]) {
        ESP_LOGE(TAG, "Persistent OTA: URL khong hop le");
        return false;
    }
    if (s_ota_busy) {
        ESP_LOGW(TAG, "Persistent OTA: OTA dang chay");
        return false;
    }
    if (!ota_pending_save(url, false)) {
        ESP_LOGE(TAG, "Persistent OTA: khong luu duoc pending job vao NVS");
        return false;
    }
    ESP_LOGI(TAG, "Persistent OTA: da luu URL vao NVS");
    return true;
}

bool app_ota_resume_pending(void)
{
    /* Khong tu resume OTA sau boot — day la nguyen nhan reset lien tuc khi TriggerOTA crash. */
    return false;
}

void app_ota_validate_running_firmware(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Firmware OTA moi (%s) dang o trang thai PENDING_VERIFY -> Xac nhan hop le (Cancel Rollback)",
                     running->label ? running->label : "?");
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Firmware da duoc danh dau VALID. Khong con rollback.");
            } else {
                ESP_LOGE(TAG, "Loi khi mark valid: %s", esp_err_to_name(err));
            }
        }
    }
}
