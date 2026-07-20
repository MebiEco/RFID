#include "app_azure.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include <sys/stat.h>

#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "lv_port.h"
#include "lcd_panel_config.h"
#include "board_pins.h"
#include "sd_card.h"
#include "card_profile.h"
#include "app_audio.h"
#include "app_ota.h"
#include "wifi_portal.h"
#include "scan_log.h"

/** CA gốc Azure IoT Hub (Baltimore + DigiCert G2 + MS RSA 2017) — nhúng từ azure_iot_ca.pem. */
extern const uint8_t azure_iot_ca_pem_start[] asm("_binary_azure_iot_ca_pem_start");
extern const uint8_t azure_iot_ca_pem_end[] asm("_binary_azure_iot_ca_pem_end");

/** Tu lcd_ui.c — lam moi danh sach the sau khi Cloud sua/xoa profile */
extern void lcd_ui_invalidate_card_cache(void);

static const char *TAG = "azure_iot";

/** 2020-01-01 UTC — duoi nguong nay = chua NTP / epoch sai, khong gui backend. */
#define AZURE_TS_MIN_UTC 1577836800LL

static bool azure_ts_valid(int64_t ts)
{
    return ts >= AZURE_TS_MIN_UTC;
}

/** Timestamp da luu hop le, hoac time() hien tai neu NTP vua sync; 0 = chua gui duoc. */
static time_t azure_resolve_timestamp(int64_t stored)
{
    if (azure_ts_valid(stored)) {
        return (time_t)stored;
    }
    return wifi_portal_get_utc_sec();
}

/** Trim NVS/WiFi portal strings — trailing spaces break TLS hostname verify (CN/SNI). */
static void trim_inplace(char *s)
{
    if (!s || !s[0]) {
        return;
    }
    char *p = s;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    s[n] = '\0';
}

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
static volatile bool s_azure_config_reload = false;

/** Tham so doi chieu 605 (backend LastIdx / Missing). */
typedef struct {
    bool active;
    bool defer_response;
    bool have_last_swipe;
    int32_t last_swipe;
    bool have_last_unkn;
    int32_t last_unkn;
    bool have_last_admin;
    int32_t last_admin;
    int missing_count;
    struct {
        int code;
        int32_t index;
    } missing[SCAN_LOG_SYNC_MISSING_MAX];
} azure_sync_req_t;

static azure_sync_req_t s_sync_req;

/** Direct Method 605: tra loi sau khi flush + replay xong. */
static struct {
    volatile bool pending;
    char rid[32];
    esp_mqtt_client_handle_t client;
} s_sync_dm;
static TaskHandle_t s_azure_task_handle = NULL;
static SemaphoreHandle_t s_ntp_done_sem;       /* bao: SNTP vua cap nhat — danh thuc azure_task */
static int64_t s_mqtt_conn_ms = 0;             /* thoi diem CONNECT (ms) — chan doan ngat som */
static int64_t s_mqtt_disc_window_start_ms = 0;
static uint32_t s_mqtt_disc_count = 0;         /* dem ngat trong cua so 2 phut */

/** Azure IoT Hub khuyến nghị keepalive tối đa 240s — ping giữ kết nối liên tục. */
#define AZURE_MQTT_KEEPALIVE_SEC 240
/** Hub dong idle sau ~1.5×keepalive; 15.6s thuong la keepalive=10 hoac 400027 trung client. */
#define AZURE_MQTT_DISC_SHORT_MS_MIN 12000
#define AZURE_MQTT_DISC_SHORT_MS_MAX 20000
#define AZURE_PEND_SD_FILE  "/sdcard/az_pend.bin"
#define AZURE_PEND_SD_TMP   "/sdcard/az_pend.tmp"

static void sd_pend_init(void);
static int flush_pending_queue(esp_mqtt_client_handle_t client, const char *dev_id);
static bool azure_pend_has_data(void);

static int32_t azure_peek_msg_index(msg_idx_type_t type)
{
    static const char *const s_keys[3] = {"idx_swipe", "idx_unkn", "idx_admin"};
    int ti = (int)type;
    if (ti < 0 || ti >= 3) {
        ti = 0;
    }
    int32_t next = 1;
    nvs_handle_t h;
    if (nvs_open("wifi_portal", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, s_keys[ti], &next) != ESP_OK) {
            next = 1;
        }
        nvs_close(h);
    }
    return next - 1;
}

static int azure_publish_event_entry(esp_mqtt_client_handle_t client, const char *dev_id,
                                     const scan_log_replay_entry_t *ent)
{
    if (!client || !dev_id || !ent || dev_id[0] == '\0' || ent->uid[0] == '\0') {
        return -1;
    }
    time_t ts = azure_resolve_timestamp(ent->timestamp_utc);
    if (ts == 0) {
        ESP_LOGW(TAG, "Replay bo qua index=%ld — chua co timestamp hop le", (long)ent->index);
        return 0;
    }
    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", dev_id);

    static char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"Code\":%d,\"Index\":%ld,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\","
             "\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
             ent->event_code, (long)ent->index, (long long)ts, ent->uid, ent->name, ent->id);

    int pub_ret = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
    if (pub_ret < 0) {
        ESP_LOGW(TAG, "Replay publish loi code=%d index=%ld", ent->event_code, (long)ent->index);
        return -1;
    }
    ESP_LOGI(TAG, "Replay tu log: code=%d index=%ld uid=%s", ent->event_code, (long)ent->index, ent->uid);
    return 0;
}

static int azure_replay_publish_cb(const scan_log_replay_entry_t *entry, void *ctx)
{
    typedef struct {
        esp_mqtt_client_handle_t client;
        const char *dev_id;
    } replay_ctx_t;
    replay_ctx_t *rc = (replay_ctx_t *)ctx;
    return azure_publish_event_entry(rc->client, rc->dev_id, entry);
}

static void azure_parse_sync_data(cJSON *root, azure_sync_req_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!root) {
        return;
    }
    cJSON *data = cJSON_GetObjectItem(root, "Data");
    if (!data || !cJSON_IsObject(data)) {
        return;
    }
    cJSON *j = cJSON_GetObjectItem(data, "LastIdxSwipe");
    if (j && cJSON_IsNumber(j)) {
        out->have_last_swipe = true;
        out->last_swipe = (int32_t)j->valuedouble;
    }
    j = cJSON_GetObjectItem(data, "LastIdxUnkn");
    if (j && cJSON_IsNumber(j)) {
        out->have_last_unkn = true;
        out->last_unkn = (int32_t)j->valuedouble;
    }
    j = cJSON_GetObjectItem(data, "LastIdxAdmin");
    if (j && cJSON_IsNumber(j)) {
        out->have_last_admin = true;
        out->last_admin = (int32_t)j->valuedouble;
    }
    j = cJSON_GetObjectItem(data, "Missing");
    if (j && cJSON_IsArray(j)) {
        int n = cJSON_GetArraySize(j);
        for (int i = 0; i < n && out->missing_count < SCAN_LOG_SYNC_MISSING_MAX; i++) {
            cJSON *it = cJSON_GetArrayItem(j, i);
            if (!it || !cJSON_IsObject(it)) {
                continue;
            }
            cJSON *c = cJSON_GetObjectItem(it, "Code");
            cJSON *ix = cJSON_GetObjectItem(it, "Index");
            if (c && cJSON_IsNumber(c) && ix && cJSON_IsNumber(ix)) {
                out->missing[out->missing_count].code = c->valueint;
                out->missing[out->missing_count].index = (int32_t)ix->valuedouble;
                out->missing_count++;
            }
        }
    }
    
    cJSON *s = cJSON_GetObjectItem(data, "StartIdx");
    cJSON *e = cJSON_GetObjectItem(data, "EndIdx");
    if (s && cJSON_IsNumber(s) && e && cJSON_IsNumber(e)) {
        int32_t start_idx = (int32_t)s->valuedouble;
        int32_t end_idx = (int32_t)e->valuedouble;
        if (start_idx > 0 && end_idx >= start_idx) {
            cJSON *code_obj = cJSON_GetObjectItem(data, "CodeRange");
            int range_code = (code_obj && cJSON_IsNumber(code_obj)) ? code_obj->valueint : 602;
            
            for (int32_t idx = start_idx; idx <= end_idx && out->missing_count < SCAN_LOG_SYNC_MISSING_MAX; idx++) {
                out->missing[out->missing_count].code = range_code;
                out->missing[out->missing_count].index = idx;
                out->missing_count++;
            }
        }
    }
    
    if (out->have_last_swipe || out->have_last_unkn || out->have_last_admin || out->missing_count > 0) {
        out->active = true;
        out->defer_response = true;
    }
}

static void azure_run_sync_work(esp_mqtt_client_handle_t client, const char *dev_id, const azure_sync_req_t *req,
                                int *pend_flushed_out, int *log_resent_out)
{
    int pend_flushed = 0;
    int log_resent = 0;
    if (pend_flushed_out) {
        *pend_flushed_out = 0;
    }
    if (log_resent_out) {
        *log_resent_out = 0;
    }
    if (!client || !dev_id) {
        return;
    }

    if (azure_pend_has_data()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        pend_flushed = flush_pending_queue(client, dev_id);
    }

    if (req && req->active) {
        scan_log_sync_filter_t filt;
        memset(&filt, 0, sizeof(filt));
        filt.have_last_swipe = req->have_last_swipe;
        filt.last_swipe = req->last_swipe;
        filt.have_last_unkn = req->have_last_unkn;
        filt.last_unkn = req->last_unkn;
        filt.have_last_admin = req->have_last_admin;
        filt.last_admin = req->last_admin;
        filt.missing_count = req->missing_count;
        for (int i = 0; i < req->missing_count; i++) {
            filt.missing[i].code = req->missing[i].code;
            filt.missing[i].index = req->missing[i].index;
        }

        struct {
            esp_mqtt_client_handle_t client;
            const char *dev_id;
        } rctx = {.client = client, .dev_id = dev_id};

        scan_log_replay_stats_t st;
        if (scan_log_replay_gaps(&filt, azure_replay_publish_cb, &rctx, &st) == ESP_OK) {
            log_resent = st.resent;
        }
    }

    if (pend_flushed_out) {
        *pend_flushed_out = pend_flushed;
    }
    if (log_resent_out) {
        *log_resent_out = log_resent;
    }
}

static void azure_send_sync_dm_response(esp_mqtt_client_handle_t client, const char *rid, const azure_sync_req_t *req,
                                        int pend_flushed, int log_resent)
{
    if (!client || !rid || rid[0] == '\0') {
        return;
    }
    int32_t idx_sw = azure_peek_msg_index(MSG_IDX_SWIPE);
    int32_t idx_un = azure_peek_msg_index(MSG_IDX_UNKNOWN);
    int32_t idx_ad = azure_peek_msg_index(MSG_IDX_ADMIN);

    char res_payload[640];
    if (req && req->active) {
        snprintf(res_payload, sizeof(res_payload),
                 "{\"status\":200,\"payload\":{\"Code\":605,\"TimeStamp\":%lld,"
                 "\"IdxSwipe\":%ld,\"IdxUnkn\":%ld,\"IdxAdmin\":%ld,"
                 "\"LastIdxSwipe\":%ld,\"LastIdxUnkn\":%ld,\"LastIdxAdmin\":%ld,"
                 "\"PendingFlushed\":%d,\"ResentFromLog\":%d,"
                 "\"Message\":\"Sync done\"}}",
                 (long long)time(NULL), (long)idx_sw, (long)idx_un, (long)idx_ad,
                 req->have_last_swipe ? (long)req->last_swipe : -1L,
                 req->have_last_unkn ? (long)req->last_unkn : -1L,
                 req->have_last_admin ? (long)req->last_admin : -1L, pend_flushed, log_resent);
    } else {
        snprintf(res_payload, sizeof(res_payload),
                 "{\"status\":200,\"payload\":{\"Code\":605,\"TimeStamp\":%lld,"
                 "\"PendingFlushed\":%d,\"Message\":\"Flush trigger accepted\"}}",
                 (long long)time(NULL), pend_flushed);
    }
    azure_dm_response(client, rid, 200, res_payload);
}

static int64_t azure_now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000);
}

static const char *azure_mqtt_error_type_str(esp_mqtt_error_type_t t)
{
    switch (t) {
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:       return "TCP_TRANSPORT";
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:  return "CONNECTION_REFUSED";
    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:    return "SUBSCRIBE_FAILED";
    default:                                  return "NONE";
    }
}

static void azure_log_mqtt_error(const esp_mqtt_event_t *event, int64_t conn_uptime_ms)
{
    if (!event || !event->error_handle) {
        ESP_LOGE(TAG, "MQTT loi (uptime=%lld ms) — khong co error_handle", (long long)conn_uptime_ms);
        return;
    }
    const esp_mqtt_error_codes_t *e = event->error_handle;
    ESP_LOGE(TAG,
             "MQTT loi uptime=%lld ms type=%s conn_rc=%d sock_errno=%d tls=%s",
             (long long)conn_uptime_ms,
             azure_mqtt_error_type_str(e->error_type),
             (int)e->connect_return_code,
             e->esp_transport_sock_errno,
             esp_err_to_name(e->esp_tls_last_esp_err));
}

static void azure_note_disconnect(int64_t conn_uptime_ms)
{
    const int64_t now = azure_now_ms();
    if (s_mqtt_disc_window_start_ms == 0 || (now - s_mqtt_disc_window_start_ms) > 120000) {
        s_mqtt_disc_window_start_ms = now;
        s_mqtt_disc_count = 0;
    }
    s_mqtt_disc_count++;

    if (conn_uptime_ms >= AZURE_MQTT_DISC_SHORT_MS_MIN &&
        conn_uptime_ms <= AZURE_MQTT_DISC_SHORT_MS_MAX) {
        ESP_LOGW(TAG,
                 "MQTT ngat sau %lld ms (~15s) — thuong do: (1) trung Device ID "
                 "(IoT Explorer/backend khac, Azure 400027) hoac (2) keepalive ping khong kip",
                 (long long)conn_uptime_ms);
    }

    if (s_mqtt_disc_count >= 3) {
        ESP_LOGE(TAG,
                 "MQTT ngat %u lan/2 phut — KIEM TRA: dong Azure IoT Explorer, backend/service "
                 "dung cung deviceId, chi 1 thiet bi ESP ket noi",
                 (unsigned)s_mqtt_disc_count);
        s_mqtt_disc_count = 0;
        s_mqtt_disc_window_start_ms = now;
    }
}

static bool azure_pend_has_data(void)
{
    sd_pend_init();
    struct stat st;
    return (stat(AZURE_PEND_SD_FILE, &st) == 0 && st.st_size > 0);
}

static void azure_mqtt_disconnect(void)
{
    if (s_mqtt_client) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
    }
    s_azure_connected = false;
    s_mqtt_conn_ms = 0;
}

static bool azure_load_cred(wifi_cred_t *cred)
{
    if (!cred) {
        return false;
    }
    memset(cred, 0, sizeof(*cred));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = sizeof(*cred);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, cred, &sz);
    nvs_close(h);
    if (err != ESP_OK || cred->magic != CRED_MAGIC ||
        cred->azure_host[0] == '\0' || cred->azure_dev[0] == '\0') {
        return false;
    }
    trim_inplace(cred->azure_host);
    trim_inplace(cred->azure_dev);
    trim_inplace(cred->azure_sas);
    return (cred->azure_host[0] != '\0' && cred->azure_dev[0] != '\0');
}

static void azure_wait_network_ready(void)
{
    ESP_LOGI(TAG, "Cho WiFi + gio NTP truoc khi ket noi Azure...");
    while (wifi_portal_get_conn_status() != WIFI_STATUS_CONNECTED ||
           !wifi_portal_time_is_valid()) {
        if (s_ntp_done_sem) {
            (void)xSemaphoreTake(s_ntp_done_sem, pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    /* WiFi PS (modem sleep) hay lam MQTT mat keepalive — tat khi dung Azure MQTT. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

/* ================================================================
 * File: /sdcard/az_pend.bin  (binary, append-only)
 * Flush: rename -> .tmp -> gui tung record -> xoa.tmp
 * ================================================================ */

typedef struct {
    char    uid[32];
    char    name[48];
    char    id[48];
    int32_t event_code;   /* 601=checkout, 602=checkin, 603=save_card, 604=delete_card */
    int64_t timestamp_utc;
    int32_t index;        /* index message */
} azure_pend_rec_t;

static SemaphoreHandle_t s_sd_pend_mtx = NULL;

static void sd_pend_init(void)
{
    if (!s_sd_pend_mtx) s_sd_pend_mtx = xSemaphoreCreateMutex();
}

int32_t app_azure_get_and_increment_msg_index(msg_idx_type_t type)
{
    /* Mỗi loại sự kiện có bộ đếm NVS riêng để không bị lẫn nhau. */
    static const char * const s_keys[3] = { "idx_swipe", "idx_unkn", "idx_admin" };
    static int32_t s_fallback[3] = { 1, 1, 1 };

    int ti = (int)type;
    if (ti < 0 || ti >= 3) ti = 0;

    nvs_handle_t h;
    int32_t idx = 0;
    if (nvs_open("wifi_portal", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_i32(h, s_keys[ti], &idx) != ESP_OK) {
            idx = 1;
        }
        int32_t next_idx = idx + 1;
        if (next_idx < 0) next_idx = 1;
        nvs_set_i32(h, s_keys[ti], next_idx);
        nvs_commit(h);
        nvs_close(h);
    } else {
        idx = s_fallback[ti]++;
    }
    return idx;
}

static void pending_enqueue(const char *uid, const char *name, const char *id, int event_code, int32_t msg_idx)
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
    r.timestamp_utc = (int64_t)wifi_portal_get_utc_sec();
    r.index         = msg_idx;

    xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
    sd_card_lock(); /* bảo vệ SPI bus SD */
    FILE *f = fopen(AZURE_PEND_SD_FILE, "ab");
    if (f) {
        fwrite(&r, sizeof(r), 1, f);
        fclose(f);
        struct stat st;
        if (stat(AZURE_PEND_SD_FILE, &st) == 0) {
            ESP_LOGI(TAG, "SD queue: %ld ban ghi dang cho (code %d, index %ld)",
                     (long)(st.st_size / (long)sizeof(azure_pend_rec_t)), event_code, (long)msg_idx);
        }
    } else {
        ESP_LOGW(TAG, "SD queue: khong mo duoc file (SD san sang chua?)");
    }
    sd_card_unlock();
    xSemaphoreGive(s_sd_pend_mtx);
}

static int flush_pending_queue(esp_mqtt_client_handle_t client, const char *dev_id)
{
    if (!client || !dev_id || dev_id[0] == '\0') {
        return 0;
    }
    sd_pend_init();
    if (!s_sd_pend_mtx) {
        return 0;
    }

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

    if (!has_data) {
        return 0;
    }

    sd_card_lock();
    FILE *f = fopen(AZURE_PEND_SD_TMP, "rb");
    sd_card_unlock();
    if (!f) {
        return 0;
    }

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

        time_t ts = azure_resolve_timestamp(rec.timestamp_utc);
        if (ts == 0) {
            ESP_LOGW(TAG, "SD queue: chua co NTP — giu lai code=%d index=%ld", (int)rec.event_code,
                     (long)rec.index);
            had_error = true;
            xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
            sd_card_lock();
            FILE *rf = fopen(AZURE_PEND_SD_FILE, "ab");
            if (rf) {
                fwrite(&rec, sizeof(rec), 1, rf);
                fclose(rf);
            }
            sd_card_unlock();
            xSemaphoreGive(s_sd_pend_mtx);
            continue;
        }
        rec.timestamp_utc = (int64_t)ts;

        /* Dùng static để giảm áp lực lên stack của task */
        static char payload[384];
        snprintf(payload, sizeof(payload),
                 "{\"Code\":%d,\"Index\":%ld,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\","
                 "\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
                 (int)rec.event_code, (long)rec.index, (long long)rec.timestamp_utc,
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
        /* Tăng delay lên 500ms để thẻ SD và bus SPI có thời gian nghỉ, nhường cho task RFID */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    sd_card_lock();
    fclose(f);
    remove(AZURE_PEND_SD_TMP);
    sd_card_unlock();

    if (total_flushed > 0) {
        ESP_LOGI(TAG, "Da flush %d ban ghi SD len Azure", total_flushed);
    }
    return total_flushed;
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
        s_mqtt_conn_ms = azure_now_ms();
        ESP_LOGI(TAG, "MQTT Connected to Azure IoT Hub! (keepalive=%ds)", AZURE_MQTT_KEEPALIVE_SEC);
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
    case MQTT_EVENT_DISCONNECTED: {
        const int64_t uptime = (s_mqtt_conn_ms > 0) ? (azure_now_ms() - s_mqtt_conn_ms) : -1;
        azure_note_disconnect(uptime);
        ESP_LOGW(TAG, "MQTT ngat tam thoi (uptime=%lld ms) — ESP-MQTT tu reconnect",
                 (long long)uptime);
        s_azure_connected = false;
        s_mqtt_conn_ms = 0;
        break;
    }
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

                    // Trich xuat method_name tu topic
                    char method_name[64] = {0};
                    char *m_start = topic_str + 21;
                    char *m_end = strchr(m_start, '/');
                    if (m_end) {
                        size_t len = m_end - m_start;
                        if (len < sizeof(method_name)) {
                            memcpy(method_name, m_start, len);
                            method_name[len] = '\0';
                        }
                    }
                    
                    bool is_flush_req = (strcmp(method_name, "FlushQueue") == 0);
                    bool is_ota_req = (strcmp(method_name, "TriggerOTA") == 0);
                    int method_code = 0;
                    
                    cJSON *root = NULL;
                    if (event->data_len > 0) {
                        char *json_str = malloc(event->data_len + 1);
                        if (json_str) {
                            memcpy(json_str, event->data, event->data_len);
                            json_str[event->data_len] = '\0';
                            root = cJSON_Parse(json_str);
                            free(json_str);
                        }
                    }
                    
                    if (root) {
                        cJSON *code = cJSON_GetObjectItem(root, "Code");
                        if (code && cJSON_IsNumber(code)) {
                            method_code = code->valueint;
                        }
                    }
                    
                    if (method_code == 605) {
                        is_flush_req = true;
                    } else if (method_code == 606) {
                        is_ota_req = true;
                    }
                    
                    if (is_flush_req) {
                        azure_sync_req_t sync_copy;
                        memset(&sync_copy, 0, sizeof(sync_copy));
                        azure_parse_sync_data(root, &sync_copy);
                        s_sync_req = sync_copy;
                        s_flush_requested = true;

                        if (sync_copy.defer_response && rid[0] != '\0') {
                            s_sync_dm.pending = true;
                            snprintf(s_sync_dm.rid, sizeof(s_sync_dm.rid), "%s", rid);
                            s_sync_dm.client = event->client;
                            ESP_LOGI(TAG,
                                     "605 Sync: LastIdxSwipe=%ld LastIdxUnkn=%ld LastIdxAdmin=%ld missing=%d",
                                     sync_copy.have_last_swipe ? (long)sync_copy.last_swipe : -1L,
                                     sync_copy.have_last_unkn ? (long)sync_copy.last_unkn : -1L,
                                     sync_copy.have_last_admin ? (long)sync_copy.last_admin : -1L,
                                     sync_copy.missing_count);
                        } else {
                            ESP_LOGI(TAG,
                                     "Direct Method: Flush queue (605 / %s). Se thuc hien trong giay lat...",
                                     method_name);
                            if (rid[0] != '\0') {
                                char res_payload[192];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":200,\"payload\":{\"Code\":605,\"TimeStamp\":%lld,"
                                         "\"Message\":\"Flush trigger accepted\"}}",
                                         (long long)time(NULL));
                                azure_dm_response(event->client, rid, 200, res_payload);
                            }
                        }
                    } else if (is_ota_req) {
                        char ota_url[256] = {0};
                        if (root) {
                            cJSON *data = cJSON_GetObjectItem(root, "Data");
                            if (data && cJSON_IsObject(data)) {
                                cJSON *url_j = cJSON_GetObjectItem(data, "Url");
                                if (url_j && cJSON_IsString(url_j)) {
                                    strncpy(ota_url, url_j->valuestring, sizeof(ota_url) - 1);
                                }
                            }
                        }
                        if (ota_url[0] != '\0') {
                            ESP_LOGI(TAG, "Direct Method: Nhan yeu cau TriggerOTA (Code 606 / Method name: %s) URL: %s", method_name, ota_url);
                            if (rid[0] != '\0') {
                                char res_payload[256];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":200,\"payload\":{\"Code\":606,\"TimeStamp\":%lld,\"Message\":\"OTA trigger accepted, starting download\"}}",
                                         (long long)time(NULL));
                                azure_dm_response(event->client, rid, 200, res_payload);
                            }
                            app_ota_start(ota_url);
                        } else {
                            ESP_LOGE(TAG, "Direct Method: Yeu cau TriggerOTA thieu tham so Url trong Data");
                            if (rid[0] != '\0') {
                                char res_payload[192];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":400,\"payload\":{\"Message\":\"Missing Url parameter\"}}");
                                azure_dm_response(event->client, rid, 400, res_payload);
                            }
                        }
                    } else if (root && (method_code == 603 || method_code == 604)) {
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
                                        int32_t msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
                                        app_azure_send_card_event(uid_str, nm, idd, 603, msg_idx);
                                        scan_log_append_admin(uid_str, nm, idd, "SAVE", msg_idx);
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
                                    char nm[48] = {0};
                                    char idd[48] = {0};
                                    bool reg = false;
                                    (void)card_profile_lookup(uid_str, nm, sizeof(nm), idd, sizeof(idd), &reg, NULL);
                                    int32_t msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
                                    esp_err_t delr = card_profile_delete(uid_str);
                                    app_azure_send_card_event(uid_str, nm, idd, 604, msg_idx);
                                    scan_log_append_admin(uid_str, nm, idd, "DEL", msg_idx);
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
                    } else if (method_code == 600) {
                        char reset_type[32] = "all";
                        if (root) {
                            cJSON *data = cJSON_GetObjectItem(root, "Data");
                            if (data && cJSON_IsObject(data)) {
                                cJSON *rst = cJSON_GetObjectItem(data, "Reset");
                                if (rst && cJSON_IsString(rst)) {
                                    strncpy(reset_type, rst->valuestring, sizeof(reset_type) - 1);
                                }
                            }
                        }

                        ESP_LOGW(TAG, "Direct Method: Nhan lenh Reset (Code 600), type: %s", reset_type);

                        if (strcmp(reset_type, "wifi") == 0 || strcmp(reset_type, "azure") == 0) {
                            wifi_cred_t cred;
                            memset(&cred, 0, sizeof(cred));
                            nvs_handle_t h;
                            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                                size_t sz = sizeof(cred);
                                if (nvs_get_blob(h, NVS_KEY, &cred, &sz) == ESP_OK) {
                                    if (strcmp(reset_type, "wifi") == 0) {
                                        memset(cred.ssid, 0, sizeof(cred.ssid));
                                        memset(cred.pass, 0, sizeof(cred.pass));
                                    } else {
                                        memset(cred.azure_host, 0, sizeof(cred.azure_host));
                                        memset(cred.azure_dev, 0, sizeof(cred.azure_dev));
                                        memset(cred.azure_sas, 0, sizeof(cred.azure_sas));
                                    }
                                    nvs_set_blob(h, NVS_KEY, &cred, sizeof(cred));
                                    nvs_commit(h);
                                }
                                nvs_close(h);
                            }
                        } else if (strcmp(reset_type, "sd") == 0) {
                            remove(BOARD_SD_RFID_LOG_PATH);
                            remove(AZURE_PEND_SD_FILE);
                            remove(AZURE_PEND_SD_TMP);
                            card_profile_delete_all();

                            nvs_handle_t h;
                            if (nvs_open("wifi_portal", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_erase_key(h, "idx_swipe");
                                nvs_erase_key(h, "idx_unkn");
                                nvs_erase_key(h, "idx_admin");
                                nvs_commit(h);
                                nvs_close(h);
                            }
                        } else {
                            nvs_flash_erase();
                        }

                        if (rid[0] != '\0') {
                            char res_payload[256];
                            snprintf(res_payload, sizeof(res_payload), "{\"status\":200,\"payload\":{\"Code\":600,\"Reset\":\"%s\",\"Message\":\"Reset initiated. Rebooting...\"}}", reset_type);
                            azure_dm_response(event->client, rid, 200, res_payload);
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    } else if (method_code == 611 || method_code == 612) {
                        // 611: Màn hình cũ (ILI9341 Legacy - ID 2)
                        // 612: Màn hình mới (GMT028 - ID 1)
                        int v = (method_code == 611) ? 2 : 1; 
                        lcd_panel_set_id((uint8_t)v);
                        ESP_LOGI(TAG, "Direct Method: Doi loai man hinh sang %s. Khoi dong lai...", v == 2 ? "CU (611)" : "MOI (612)");
                        if (rid[0] != '\0') {
                            char res_payload[128];
                            snprintf(res_payload, sizeof(res_payload), "{\"status\":200,\"payload\":{\"Message\":\"Switched to screen type %d. Rebooting...\"}}", v);
                            azure_dm_response(event->client, rid, 200, res_payload);
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        esp_restart();
                    }

                    if (root) {
                        cJSON_Delete(root);
                    }
                }
                free(topic_str);
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR: {
        const int64_t uptime = (s_mqtt_conn_ms > 0) ? (azure_now_ms() - s_mqtt_conn_ms) : -1;
        azure_log_mqtt_error(event, uptime);
        break;
    }
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d - Azure da xac nhan nhan tin nhan (QoS 1 PUBACK)", event->msg_id);
        break;
    default:
        break;
    }
}

static void azure_task(void *arg)
{
    (void)arg;
    s_azure_task_handle = xTaskGetCurrentTaskHandle();

    for (;;) {
        s_azure_config_reload = false;

        wifi_cred_t cred;
        if (!azure_load_cred(&cred)) {
            ESP_LOGW(TAG, "Chua thiet lap Azure thong qua WiFi Portal. Tam dung MQTT.");
            s_azure_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "Azure config: host=%s device=%s", cred.azure_host, cred.azure_dev);
        azure_wait_network_ready();

        while (!s_azure_config_reload) {
            while ((wifi_portal_get_conn_status() != WIFI_STATUS_CONNECTED ||
                    !wifi_portal_time_is_valid()) &&
                   !s_azure_config_reload) {
                azure_mqtt_disconnect();
                azure_wait_network_ready();
            }
            if (s_azure_config_reload) {
                break;
            }

            char sas_token[256];
            if (generate_sas_token(cred.azure_host, cred.azure_dev, cred.azure_sas, sas_token,
                                   sizeof(sas_token)) != ESP_OK) {
                ESP_LOGE(TAG, "Tạo SAS token thất bại. Thử lại sau 1 phút.");
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }

            char uri[128];
            snprintf(uri, sizeof(uri), "mqtts://%s:8883", cred.azure_host);
            ESP_LOGI(TAG, "MQTT TLS to host=%s (device=%s)", cred.azure_host, cred.azure_dev);

            char username[128];
            snprintf(username, sizeof(username), "%s/%s/?api-version=2021-04-12", cred.azure_host,
                     cred.azure_dev);

            const char *azure_ca = (const char *)azure_iot_ca_pem_start;
            size_t azure_ca_len = (size_t)(azure_iot_ca_pem_end - azure_iot_ca_pem_start);
            if (azure_ca_len < 200) {
                ESP_LOGE(TAG, "azure_iot_ca.pem loi nhung/link — rebuild project");
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }

            esp_mqtt_client_config_t mqtt_cfg = {
                .broker = {
                    .address.uri = uri,
                    .verification = {
                        .certificate = azure_ca,
                        .certificate_len = azure_ca_len,
                        .skip_cert_common_name_check = true,
                    },
                },
                .credentials = {
                    .client_id = cred.azure_dev,
                    .username = username,
                    .authentication.password = sas_token,
                },
                .session = {
                    .keepalive = AZURE_MQTT_KEEPALIVE_SEC,
                },
                .network = {
                    .reconnect_timeout_ms = 10000,
                    .timeout_ms = 20000,
                },
                .task = {
                    .priority = 6,
                    .stack_size = 10240,
                },
            };

            s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
            if (!s_mqtt_client) {
                ESP_LOGE(TAG, "esp_mqtt_client_init fail");
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }

            esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_mqtt_client_start(s_mqtt_client);

            for (int i = 0; i < 720 * 12 && !s_azure_config_reload; i++) {
                for (int t = 0; t < 10 && !s_azure_config_reload; t++) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                if (s_azure_config_reload) {
                    break;
                }
                if (s_flush_requested && s_azure_connected && s_mqtt_client) {
                    s_flush_requested = false;
                    azure_sync_req_t sync_copy = s_sync_req;
                    bool defer_dm = s_sync_dm.pending;
                    char dm_rid[32];
                    esp_mqtt_client_handle_t dm_client = s_sync_dm.client;
                    if (defer_dm) {
                        snprintf(dm_rid, sizeof(dm_rid), "%s", s_sync_dm.rid);
                        s_sync_dm.pending = false;
                        s_sync_dm.rid[0] = '\0';
                    }
                    memset(&s_sync_req, 0, sizeof(s_sync_req));

                    if (azure_pend_has_data() || sync_copy.active) {
                        int pend_flushed = 0;
                        int log_resent = 0;
                        ESP_LOGI(TAG, "605/Flush: bat dau (pend=%d sync=%d)...", azure_pend_has_data() ? 1 : 0,
                                 sync_copy.active ? 1 : 0);
                        azure_run_sync_work(s_mqtt_client, cred.azure_dev, sync_copy.active ? &sync_copy : NULL,
                                            &pend_flushed, &log_resent);
                        if (defer_dm) {
                            azure_send_sync_dm_response(dm_client, dm_rid, sync_copy.active ? &sync_copy : NULL,
                                                          pend_flushed, log_resent);
                        }
                    } else if (defer_dm) {
                        azure_send_sync_dm_response(dm_client, dm_rid, sync_copy.active ? &sync_copy : NULL, 0, 0);
                    }
                }
            }

            azure_mqtt_disconnect();

            if (s_azure_config_reload) {
                break;
            }

            ESP_LOGI(TAG, "Refreshing Azure SAS Token sau 12h...");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        azure_mqtt_disconnect();
        if (s_azure_config_reload) {
            ESP_LOGI(TAG, "Cau hinh Azure thay doi — doc lai tu NVS");
            continue;
        }
    }
}

static void azure_try_flush_pending(void)
{
    if (!s_azure_connected || !s_mqtt_client || !wifi_portal_time_is_valid()) {
        return;
    }
    wifi_cred_t cred;
    if (!azure_load_cred(&cred)) {
        return;
    }
    (void)flush_pending_queue(s_mqtt_client, cred.azure_dev);
}

void app_azure_notify_sntp_synced(void)
{
    if (s_ntp_done_sem) {
        (void)xSemaphoreGive(s_ntp_done_sem);
    }
    azure_try_flush_pending();
}

void app_azure_notify_config_changed(void)
{
    s_azure_config_reload = true;
    azure_mqtt_disconnect();
    if (s_azure_task_handle == NULL) {
        app_azure_start();
    }
}

void app_azure_start(void)
{
    if (!s_ntp_done_sem) {
        s_ntp_done_sem = xSemaphoreCreateBinary();
    }
    if (s_azure_task_handle != NULL) {
        return;
    }
    BaseType_t res = xTaskCreatePinnedToCore(azure_task, "azure_task", 20480, NULL, 5,
                                             &s_azure_task_handle, 0);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Không tạo được azure_task");
        s_azure_task_handle = NULL;
    }
}

void app_azure_send_telemetry(const char *uid, const char *name, const char *id, int is_registered, int32_t msg_idx)
{
    if (!uid || !uid[0]) return;

    int code_val = is_registered ? 602 : 601;
    time_t now = wifi_portal_get_utc_sec();

    /* Chua NTP: xep hang, gui sau khi dong bo (timestamp=0 -> gan lai luc flush). */
    if (now == 0) {
        pending_enqueue(uid, name, id, code_val, msg_idx);
        ESP_LOGW(TAG, "Chua co NTP — xep hang telemetry: UID=%s index=%ld", uid, (long)msg_idx);
        return;
    }

    /* Nếu chưa kết nối: lưu vào hàng đợi offline (SD), đợi flush khi có mạng */
    if (!s_azure_connected || !s_mqtt_client) {
        pending_enqueue(uid, name, id, code_val, msg_idx);
        ESP_LOGW(TAG, "Azure offline — queued telemetry: UID=%s code=%d index=%ld", uid, code_val, (long)msg_idx);
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
        pending_enqueue(uid, name, id, code_val, msg_idx);
        return;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", cred.azure_dev);

    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"Code\":%d,\"Index\":%ld,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\",\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
             code_val, (long)msg_idx, (long long)now, uid, name ? name : "", id ? id : "");

    int pub_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Da day telemetry len Azure (msg_id=%d, index=%ld): %s", pub_id, (long)msg_idx, payload);
}

void app_azure_send_card_event(const char *uid, const char *name, const char *id, int event_code,
                               int32_t msg_idx)
{
    if (!uid || !uid[0] || msg_idx <= 0) {
        return;
    }

    time_t now = wifi_portal_get_utc_sec();
    if (now == 0) {
        pending_enqueue(uid, name, id, event_code, msg_idx);
        ESP_LOGW(TAG, "Chua co NTP — xep hang event %d: UID=%s index=%ld", event_code, uid, (long)msg_idx);
        return;
    }

    /* Nếu chưa kết nối: lưu vào SD queue, flush tự động khi kết nối lại */
    if (!s_azure_connected || !s_mqtt_client) {
        pending_enqueue(uid, name, id, event_code, msg_idx);
        ESP_LOGW(TAG, "Azure offline — queued card event: UID=%s code=%d index=%ld", uid, event_code, (long)msg_idx);
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
        pending_enqueue(uid, name, id, event_code, msg_idx);
        return;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/", cred.azure_dev);

    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"Code\":%d,\"Index\":%ld,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\",\"UID\":\"%s\",\"Name\":\"%s\",\"ID\":\"%s\"}}",
             event_code, (long)msg_idx, (long long)now, uid, name ? name : "", id ? id : "");

    int pub_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "Da day event %d len Azure (msg_id=%d, index=%ld): %s", event_code, pub_id, (long)msg_idx, payload);
}

int app_azure_resend_range(int code, int32_t start_idx, int32_t end_idx)
{
    if (start_idx > end_idx || start_idx <= 0) return 0;
    if (!s_mqtt_client || !s_azure_connected) return 0;

    wifi_cred_t cred;
    if (!azure_load_cred(&cred)) return 0;

    int total_resent = 0;
    int32_t current = start_idx;
    
    struct {
        esp_mqtt_client_handle_t client;
        const char *dev_id;
    } rctx = { .client = s_mqtt_client, .dev_id = cred.azure_dev };

    while (current <= end_idx) {
        scan_log_sync_filter_t filt;
        memset(&filt, 0, sizeof(filt));
        
        while (current <= end_idx && filt.missing_count < SCAN_LOG_SYNC_MISSING_MAX) {
            filt.missing[filt.missing_count].code = code;
            filt.missing[filt.missing_count].index = current;
            filt.missing_count++;
            current++;
        }
        
        scan_log_replay_stats_t st;
        if (scan_log_replay_gaps(&filt, azure_replay_publish_cb, &rctx, &st) == ESP_OK) {
            total_resent += st.resent;
        }
    }
    return total_resent;
}

int app_azure_is_connected(void)
{
    return s_azure_connected ? 1 : 0;
}
