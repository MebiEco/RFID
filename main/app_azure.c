#include "app_azure.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include <sys/stat.h>

#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "board_pins.h"
#include "sd_card.h"
#include "card_profile.h"

/** Tu lcd_ui.c — lam moi danh sach the sau khi Cloud sua/xoa profile */
extern void lcd_ui_invalidate_card_cache(void);

static const char *TAG = "azure_iot";

/** Tra loi Direct Method Azure IoT Hub (HTTP status trong topic). */
static void azure_dm_response(esp_mqtt_client_handle_t client, const char *rid, int status_code,
                              const char *json_payload)
{
    if (!client || !rid || !rid[0] || !json_payload) {
        return;
    }
    char res_topic[160];
    snprintf(res_topic, sizeof(res_topic), "$iothub/methods/res/%d/?$rid=%s", status_code, rid);
    esp_mqtt_client_publish(client, res_topic, json_payload, 0, 1, 0);
}

#define NVS_NS "wifi_portal"
#define NVS_KEY "cred"
#define CRED_MAGIC 0x57494649u

typedef struct __attribute__((packed)) { 
    uint32_t magic;
    char ssid[32];
    char pass[64];
    char azure_host[64];
    char azure_dev[32];
    char azure_sas[128];
} wifi_cred_t; 

static esp_mqtt_client_handle_t s_mqtt_client    = NULL;
static bool        s_azure_connected  = false;
static volatile bool s_flush_requested = false; /* flag: azure_task se flush queue khi co mang */
static SemaphoreHandle_t s_ntp_done_sem;       /* bao: SNTP vua cap nhat — danh thuc azure_task */

/* ================================================================
 * File: /sdcard/az_pend.bin  (binary, append-only)
 * Flush: rename -> .tmp -> gui tung record -> xoa.tmp
 * ================================================================ */
#define AZURE_PEND_SD_FILE  "/sdcard/az_pend.bin"
#define AZURE_PEND_SD_TMP   "/sdcard/az_pend.tmp"

typedef struct {
    char    uid[32];
    char    name[48];
    char    id[48];
    int32_t event_code;   /* 601=checkout, 602=checkin, 603=save_card, 604=delete_card */
    int64_t timestamp_utc;
} azure_pend_rec_t;

static SemaphoreHandle_t s_sd_pend_mtx = NULL;

static void sd_pend_init(void)
{
    if (!s_sd_pend_mtx) s_sd_pend_mtx = xSemaphoreCreateMutex();
}

static void pending_enqueue(const char *uid, const char *name, const char *id, int event_code)
{
    if (!uid || !uid[0]) return;
    sd_pend_init();
    if (!s_sd_pend_mtx) return;

    azure_pend_rec_t r;
    memset(&r, 0, sizeof(r));
    strncpy(r.uid,  uid,              sizeof(r.uid)  - 1);
    strncpy(r.name, name ? name : "", sizeof(r.name) - 1);
    strncpy(r.id,   id   ? id   : "", sizeof(r.id)   - 1);
    r.event_code    = (int32_t)event_code;
    r.timestamp_utc = (int64_t)time(NULL);

    xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
    sd_card_lock(); /* bảo vệ SPI bus SD */
    FILE *f = fopen(AZURE_PEND_SD_FILE, "ab");
    if (f) {
        fwrite(&r, sizeof(r), 1, f);
        fclose(f);
        struct stat st;
        if (stat(AZURE_PEND_SD_FILE, &st) == 0) {
            ESP_LOGI(TAG, "SD queue: %ld ban ghi dang cho (code %d)",
                     (long)(st.st_size / (long)sizeof(azure_pend_rec_t)), event_code);
        }
    } else {
        ESP_LOGW(TAG, "SD queue: khong mo duoc file (SD san sang chua?)");
    }
    sd_card_unlock();
    xSemaphoreGive(s_sd_pend_mtx);
}

static void flush_pending_queue(esp_mqtt_client_handle_t client, const char *dev_id)
{
    if (!client || !dev_id || dev_id[0] == '\0') return;
    sd_pend_init();
    if (!s_sd_pend_mtx) return;

    /* Kiem tra co file khong */
    xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
    sd_card_lock();
    struct stat st;
    bool has_data = (stat(AZURE_PEND_SD_FILE, &st) == 0 && st.st_size > 0);
    if (has_data) {
        remove(AZURE_PEND_SD_TMP);
        rename(AZURE_PEND_SD_FILE, AZURE_PEND_SD_TMP);
    }
    sd_card_unlock();
    xSemaphoreGive(s_sd_pend_mtx);

    if (!has_data) return;

    sd_card_lock();
    FILE *f = fopen(AZURE_PEND_SD_TMP, "rb");
    sd_card_unlock();
    if (!f) return;

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", dev_id);

    int total_flushed = 0;
    bool had_error = false;
    azure_pend_rec_t rec;

    while (true) {
        sd_card_lock();
        bool got = (fread(&rec, sizeof(rec), 1, f) == 1);
        sd_card_unlock();
        if (!got) break;

        if (had_error) {
            xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
            sd_card_lock();
            FILE *rf = fopen(AZURE_PEND_SD_FILE, "ab");
            if (rf) { fwrite(&rec, sizeof(rec), 1, rf); fclose(rf); }
            sd_card_unlock();
            xSemaphoreGive(s_sd_pend_mtx);
            continue;
        }

        char payload[320];
        snprintf(payload, sizeof(payload),
                 "{\"Code\":%d,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\","
                 "\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
                 (int)rec.event_code, (long long)rec.timestamp_utc,
                 rec.uid, rec.name, rec.id);

        int pub_ret = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
        if (pub_ret < 0) {
            ESP_LOGW(TAG, "Publish loi — giu lai cac ban ghi con");
            had_error = true;
            xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
            sd_card_lock();
            FILE *rf = fopen(AZURE_PEND_SD_FILE, "ab");
            if (rf) { fwrite(&rec, sizeof(rec), 1, rf); fclose(rf); }
            sd_card_unlock();
            xSemaphoreGive(s_sd_pend_mtx);
            continue;
        }
        total_flushed++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    sd_card_lock();
    fclose(f);
    remove(AZURE_PEND_SD_TMP);
    sd_card_unlock();

    if (total_flushed > 0) {
        ESP_LOGI(TAG, "Da flush %d ban ghi SD len Azure", total_flushed);
    }
}

/* Mã hóa ký tự đặc biệt theo chuẩn URL Encode để chèn vào URL */
static void url_encode(const char *src, char *dst, size_t dst_len)
{
    const char *hex = "0123456789ABCDEF";
    size_t d = 0;
    for (size_t i = 0; src[i] && d < dst_len - 3; i++) {
        unsigned char c = src[i];
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
            ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[c >> 4];
            dst[d++] = hex[c & 15];
        }
    }
    dst[d] = '\0';
}

static esp_err_t generate_sas_token(const char *host, const char *dev_id, const char *sas_key, char *out_token, size_t token_len)
{
    // Xác định thời gian hết hạn: hiện tại + 24 giờ (86400s)
    time_t now;
    time(&now); 
    if (now < 1000000) {
        ESP_LOGE(TAG, "Lỗi: SNTP chưa đồng bộ thời gian. (now=%lld)", (long long)now);
        return ESP_FAIL;
    }
    time_t expiry = now + 86400;

    // sr = URI-encoded (hostname/devices/deviceId)
    char uri[128];
    snprintf(uri, sizeof(uri), "%s/devices/%s", host, dev_id);
    char sr[192];
    url_encode(uri, sr, sizeof(sr));

    // string_to_sign = sr + "\n" + se
    char string_to_sign[256];
    snprintf(string_to_sign, sizeof(string_to_sign), "%s\n%lld", sr, (long long)expiry);

    // Giải mã Base64 của sas_key
    unsigned char key_bin[128];
    size_t key_len = 0;
    int ret = mbedtls_base64_decode(key_bin, sizeof(key_bin), &key_len, (const unsigned char *)sas_key, strlen(sas_key));
    if (ret != 0) {
        ESP_LOGE(TAG, "MbedTLS Base64 decode sas_key lỗi: -0x%x", -ret);
        return ESP_FAIL;
    }

    // Hash HMAC-SHA256
    unsigned char mac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key_bin, key_len);
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)string_to_sign, strlen(string_to_sign));
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);

    // Base64 Encode cục Hash
    unsigned char sig_b64[64];
    size_t sig_len = 0;
    mbedtls_base64_encode(sig_b64, sizeof(sig_b64), &sig_len, mac, sizeof(mac));

    // URL-encode Sig
    char sig_url[128];
    url_encode((const char*)sig_b64, sig_url, sizeof(sig_url));

    // Ráp thành SAS Token
    snprintf(out_token, token_len, "SharedAccessSignature sr=%s&sig=%s&se=%lld", sr, sig_url, (long long)expiry);
    
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT Connected to Azure IoT Hub!");
        s_azure_connected = true;
        
        wifi_cred_t cred;
        memset(&cred, 0, sizeof(cred));
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t sz = sizeof(cred);
            nvs_get_blob(h, NVS_KEY, &cred, &sz);
            nvs_close(h);
        }
        if (cred.azure_dev[0] != '\0') {
            esp_mqtt_client_subscribe(event->client, "$iothub/methods/POST/#", 0);
            ESP_LOGI(TAG, "Da dang ky nhan Direct Method: 603 cap nhat the, 604 xoa the");
            /* Dat flag de azure_task flush queue an toan tu task context */
            s_flush_requested = true;
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT Disconnected");
        s_azure_connected = false;
        break;
    case MQTT_EVENT_DATA: { 
        if (event->data_len > 0 && event->data_len < 1024) {
            char *topic_str = malloc(event->topic_len + 1);
            if (topic_str) {
                memcpy(topic_str, event->topic, event->topic_len);
                topic_str[event->topic_len] = '\0';
                
                if (strncmp(topic_str, "$iothub/methods/POST/", 21) == 0) {
                    char *rid_ptr = strstr(topic_str, "?$rid=");
                    char rid[32] = {0};
                    if (rid_ptr) {
                        snprintf(rid, sizeof(rid), "%s", rid_ptr + 6);
                    }
                    
                    char *json_str = malloc(event->data_len + 1);
                    if (json_str) {
                        memcpy(json_str, event->data, event->data_len);
                        json_str[event->data_len] = '\0';
                        
                        cJSON *root = cJSON_Parse(json_str);
                        if (root) {
                            cJSON *code = cJSON_GetObjectItem(root, "Code");
                            if (code && cJSON_IsNumber(code)
                                && (code->valueint == 603 || code->valueint == 604)) {
                                const int method_code = code->valueint;
                                cJSON *data = cJSON_GetObjectItem(root, "Data");
                                if (data && cJSON_IsObject(data)) {
                                    cJSON *uid_j = cJSON_GetObjectItem(data, "UID");
                                    cJSON *name_j = cJSON_GetObjectItem(data, "Name");
                                    cJSON *id_j = cJSON_GetObjectItem(data, "ID");

                                    if (uid_j && cJSON_IsString(uid_j) && uid_j->valuestring[0] != '\0') {
                                        const char *uid_str = uid_j->valuestring;

                                        if (method_code == 603) {
                                            const char *nm = (name_j && cJSON_IsString(name_j)) ? name_j->valuestring : "";
                                            const char *idd = (id_j && cJSON_IsString(id_j)) ? id_j->valuestring : "";
                                            esp_err_t saver = card_profile_save(uid_str, nm, idd);
                                            if (saver == ESP_OK) {
                                                lcd_ui_invalidate_card_cache();
                                                ESP_LOGI(TAG, "603: Da cap nhat profile UID=%s", uid_str);
                                                if (rid[0] != '\0') {
                                                    char res_payload[320];
                                                    snprintf(res_payload, sizeof(res_payload),
                                                             "{\"status\":200,\"payload\":{\"Code\":603,\"TimeStamp\":%lld,"
                                                             "\"Message\":\"Updated UID %s\"}}",
                                                             (long long)time(NULL), uid_str);
                                                    azure_dm_response(event->client, rid, 200, res_payload);
                                                }
                                            } else {
                                                ESP_LOGE(TAG, "603: Luu profile loi UID=%s", uid_str);
                                                if (rid[0] != '\0') {
                                                    char res_payload[256];
                                                    snprintf(res_payload, sizeof(res_payload),
                                                             "{\"status\":500,\"payload\":{\"Message\":\"Save failed\"}}");
                                                    azure_dm_response(event->client, rid, 500, res_payload);
                                                }
                                            }
                                        } else {
                                            /* 604: xoa file profile tren SD */
                                            esp_err_t delr = card_profile_delete(uid_str);
                                            lcd_ui_invalidate_card_cache();
                                            if (delr == ESP_OK) {
                                                ESP_LOGI(TAG, "604: Da xoa profile UID=%s", uid_str);
                                            } else {
                                                ESP_LOGW(TAG, "604: Xoa profile UID=%s (file co the khong ton tai)", uid_str);
                                            }
                                            if (rid[0] != '\0') {
                                                char res_payload[320];
                                                snprintf(res_payload, sizeof(res_payload),
                                                         "{\"status\":200,\"payload\":{\"Code\":604,\"TimeStamp\":%lld,"
                                                         "\"Message\":\"Deleted or absent UID %s\"}}",
                                                         (long long)time(NULL), uid_str);
                                                azure_dm_response(event->client, rid, 200, res_payload);
                                            }
                                        }
                                    }
                                }
                            }
                            cJSON_Delete(root);
                        }
                        free(json_str);
                    }
                }
                free(topic_str);
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT Lỗi");
        break;
    default:
        break;
    }
}

static void azure_task(void *arg)
{
    (void)arg;
    
    // 1. Đọc credentials
    wifi_cred_t cred;
    memset(&cred, 0, sizeof(cred));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(cred);
        nvs_get_blob(h, NVS_KEY, &cred, &sz);
        nvs_close(h);
    }

    if (cred.magic != CRED_MAGIC || cred.azure_host[0] == '\0' || cred.azure_dev[0] == '\0') {
        ESP_LOGW(TAG, "Chưa thiết lập Azure thông qua WiFi Portal. Tắt Task.");
        vTaskDelete(NULL);
        return;
    }

    // 2. Chờ SNTP (time() hợp lệ) — tín hiệu từ time_synced_cb, hoặc poll tối đa 500ms/lần
    while (time(NULL) <= 1000000) {
        if (s_ntp_done_sem) {
            (void)xSemaphoreTake(s_ntp_done_sem, pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    /* Ngắn: stack TLS/MQTT sau khi có IP + giờ */
    vTaskDelay(pdMS_TO_TICKS(800));

    // Vòng lặp duy trì cấu hình & tái tạo bảo mật mỗi 12 giờ
    while (1) {
        // 3. Sử dụng SAS Token
        char sas_token[256];
        if (generate_sas_token(cred.azure_host, cred.azure_dev, cred.azure_sas, sas_token, sizeof(sas_token)) != ESP_OK) {
            ESP_LOGE(TAG, "Tạo SAS token thất bại. Thử lại sau 1 phút.");
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        // 4. Khởi tạo MQTT client
        char uri[128];
        snprintf(uri, sizeof(uri), "mqtts://%s:8883", cred.azure_host);

        char username[128];
        snprintf(username, sizeof(username), "%s/%s/?api-version=2021-04-12", cred.azure_host, cred.azure_dev);

        esp_mqtt_client_config_t mqtt_cfg = {
            .broker = {
                .address.uri = uri,
                .verification.crt_bundle_attach = esp_crt_bundle_attach
            },
            .credentials = {
                .client_id = cred.azure_dev,
                .username = username,
                .authentication.password = sas_token
            }
        };

        s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        if (!s_mqtt_client) {
            ESP_LOGE(TAG, "esp_mqtt_client_init fail");
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(s_mqtt_client);

        // Doi 12 tieng, kiem tra flush queue moi 5 giay
        for (int i = 0; i < 720 * 12; i++) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            /* Flush queue offline khi co flag (set boi MQTT_EVENT_CONNECTED) */
            if (s_flush_requested && s_azure_connected && s_mqtt_client) {
                s_flush_requested = false;
                /* Cho 3 giay de MQTT on dinh truoc khi flush */
                vTaskDelay(pdMS_TO_TICKS(3000));
                ESP_LOGI(TAG, "Flush offline queue sau khi ket noi...");
                flush_pending_queue(s_mqtt_client, cred.azure_dev);
            }
            /* Log stack watermark mỗi 60s (12 × 5s) */
            // if (i % 12 == 0) {
            //     UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
            //     ESP_LOGW(TAG, "[STACK] azure_task free: %4u words (%5u bytes) / 8192 total",
            //              (unsigned)wm, (unsigned)(wm * sizeof(StackType_t)));
            //     if (wm * sizeof(StackType_t) < 512) {
            //         ESP_LOGE(TAG, "[STACK] azure_task SAP STACK OVERFLOW!");
            //     }
            // }
        }

        // Gỡ client cũ để vòng lặp quay lại khởi tạo token mới cho 24h kế tiếp
        ESP_LOGI(TAG, "Refreshing Azure SAS Token sau 12h...");
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_azure_connected = false;
        
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_azure_notify_sntp_synced(void)
{
    if (s_ntp_done_sem) {
        (void)xSemaphoreGive(s_ntp_done_sem);
    }
}

void app_azure_start(void)
{
    if (!s_ntp_done_sem) {
        s_ntp_done_sem = xSemaphoreCreateBinary();
    }
    BaseType_t res = xTaskCreate(azure_task, "azure_task", 8192, NULL, 5, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Không tạo được azure_task");
    }
}

void app_azure_send_telemetry(const char *uid, const char *name, const char *id, int is_registered)
{
    if (!uid || !uid[0]) return;

    int code_val = is_registered ? 602 : 601;

    /* Nếu chưa kết nối: lưu vào hàng đợi offline (SD), đợi flush khi có mạng */
    if (!s_azure_connected || !s_mqtt_client) {
        pending_enqueue(uid, name, id, code_val);
        ESP_LOGW(TAG, "Azure offline — queued telemetry: UID=%s code=%d", uid, code_val);
        return;
    }

    wifi_cred_t cred;
    memset(&cred, 0, sizeof(cred));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(cred);
        nvs_get_blob(h, NVS_KEY, &cred, &sz);
        nvs_close(h);
    }
    if (cred.azure_dev[0] == '\0') {
        pending_enqueue(uid, name, id, code_val);
        return;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", cred.azure_dev);

    char payload[320];
    time_t now = time(NULL);

    snprintf(payload, sizeof(payload),
             "{\"Code\":%d,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\",\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
             code_val, (long long)now, uid, name ? name : "", id ? id : "");

    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Da day telemetry len Azure: %s", payload);
}

void app_azure_send_card_event(const char *uid, const char *name, const char *id, int event_code)
{
    if (!uid || !uid[0]) return;

    /* Nếu chưa kết nối: lưu vào SD queue, flush tự động khi kết nối lại */
    if (!s_azure_connected || !s_mqtt_client) {
        pending_enqueue(uid, name, id, event_code);
        ESP_LOGW(TAG, "Azure offline — queued card event: UID=%s code=%d", uid, event_code);
        return;
    }

    wifi_cred_t cred;
    memset(&cred, 0, sizeof(cred));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(cred);
        nvs_get_blob(h, NVS_KEY, &cred, &sz);
        nvs_close(h);
    }
    if (cred.azure_dev[0] == '\0') {
        pending_enqueue(uid, name, id, event_code);
        return;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", cred.azure_dev);

    char payload[320];
    time_t now = time(NULL);

    snprintf(payload, sizeof(payload),
             "{\"Code\":%d,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\",\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
             event_code, (long long)now, uid, name ? name : "", id ? id : "");

    esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Da day event %d len Azure: %s", event_code, payload);
}

int app_azure_is_connected(void)
{
    return s_azure_connected ? 1 : 0;
}
