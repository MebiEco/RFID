#include "app_azure.h"
#include "command_status.h"


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_tls_errors.h"
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
#include "app_build_info.h"
#include "app_rfid.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_netif.h"

/** CA gốc Azure IoT Hub (Baltimore + DigiCert G2 + MS RSA 2017) — nhúng từ azure_iot_ca.pem. */
extern const uint8_t azure_iot_ca_pem_start[] asm("_binary_azure_iot_ca_pem_start");
extern const uint8_t azure_iot_ca_pem_end[] asm("_binary_azure_iot_ca_pem_end");

/** Tu lcd_ui.c — lam moi danh sach the sau khi Cloud sua/xoa profile */
extern void lcd_ui_invalidate_card_cache(void);

static const char *TAG = "app_azure";

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

/** Chi con hostname: ten-iot-hub.azure-devices.net (bo mqtts://, :8883, /, space). */
static void normalize_azure_host(char *host)
{
    if (!host || !host[0]) {
        return;
    }
    trim_inplace(host);
    char *scheme = strstr(host, "://");
    if (scheme) {
        memmove(host, scheme + 3, strlen(scheme + 3) + 1);
    }
    char *slash = strchr(host, '/');
    if (slash) {
        *slash = '\0';
    }
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
    }
    trim_inplace(host);
    for (char *p = host; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
}

static void azure_note_publish(int msg_id, const char *payload);

/** Tra loi Direct Method Azure IoT Hub (HTTP status trong topic). Tra ve msg_id publish (PUBACK). */
static int azure_dm_response(esp_mqtt_client_handle_t client, const char *rid, int status_code,
                             const char *json_payload)
{
    if (!client || !rid || !rid[0] || !json_payload) {
        return -1;
    }
    char res_topic[160];
    snprintf(res_topic, sizeof(res_topic), "$iothub/methods/res/%d/?$rid=%s", status_code, rid);
    int mid = esp_mqtt_client_publish(client, res_topic, json_payload, 0, 1, 0);
    azure_note_publish(mid, json_payload);
    return mid;
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
static volatile bool s_azure_connected  = false;
static volatile int  s_azure_tx_busy    = 0;
/** Mutex serialize MQTT: quet live uu tien — sync chi publish khi khong dang quet. */
static SemaphoreHandle_t s_azure_pub_mtx;
static volatile bool s_flush_requested = false; /* flag: azure_task se flush queue khi co mang */
static volatile bool s_azure_config_reload = false;
static volatile bool s_azure_ota_suspend = false; /* OTA: khong reconnect MQTT */

/** Tham so doi chieu 605 (backend LastIdx / Missing / StartIdx-EndIdx). */
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
    /** StartIdx..EndIdx — tu chia batch SCAN_LOG_SYNC_MISSING_MAX khi replay. */
    bool have_range;
    int32_t range_start;
    int32_t range_end;
    int range_code;
} azure_sync_req_t;

static azure_sync_req_t s_sync_req;

/** Direct Method 605: luon tra accepted ngay; sync chay nen (khong defer). */
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
#define AZURE_PEND_SD_FILE  "/sdcard/az_pend.bin"
#define AZURE_PEND_SD_TMP   "/sdcard/az_pend.tmp"

/** Cache payload publish + log JSON gui len Azure. */
#define AZURE_PUB_TRACE_MAX 8
#define AZURE_PUB_TRACE_PAYLOAD 384
typedef struct {
    int msg_id;
    char payload[AZURE_PUB_TRACE_PAYLOAD];
} azure_pub_trace_t;
static azure_pub_trace_t s_pub_trace[AZURE_PUB_TRACE_MAX];
static uint8_t s_pub_trace_i;

static void azure_format_payload(char *out, size_t out_sz, int code, int32_t idx, int64_t ts, const char *uid,
                                 const char *name, const char *id)
{
    snprintf(out, out_sz,
             "{\"Code\":%d,\"Index\":%ld,\"TimeStamp\":%lld,\"Data\":{\"DeviceName\":\"RFID_Scanner\",\"UID\":\"%s\","
             "\"Name\":\"%s\",\"ID\":\"%s\"}}",
             code, (long)idx, (long long)ts, uid, name ? name : "", id ? id : "");
}

/** Log JSON sap gui / dang gui len Azure (hien tren Log Terminal). */
static void azure_log_outbound(const char *payload, bool queued)
{
    if (!payload || !payload[0]) {
        return;
    }
    if (queued) {
        ESP_LOGI(TAG, "[cho gui] %s", payload);
    } else {
        ESP_LOGI(TAG, "%s", payload);
    }
}

static void azure_note_publish(int msg_id, const char *payload)
{
    if (msg_id < 0 || !payload || !payload[0]) {
        if (payload && payload[0]) {
            ESP_LOGW(TAG, "[gui loi] %s", payload);
        }
        return;
    }
    azure_log_outbound(payload, false);
    azure_pub_trace_t *t = &s_pub_trace[s_pub_trace_i % AZURE_PUB_TRACE_MAX];
    s_pub_trace_i++;
    t->msg_id = msg_id;
    snprintf(t->payload, sizeof(t->payload), "%s", payload);
}

static void sd_pend_init(void);
static int flush_pending_queue(esp_mqtt_client_handle_t client, const char *dev_id);
static bool azure_pend_has_data(void);
static void azure_wait_swipe_priority(void);
static void azure_sync_send_begin(void);
static void azure_sync_send_end(void);
static void azure_live_send_begin(void);
static void azure_live_send_end(void);

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
    char topic[192];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8", dev_id);

    static char payload[448];
    azure_format_payload(payload, sizeof(payload), ent->event_code, ent->index, (int64_t)ts, ent->uid, ent->name,
                         ent->id);

    azure_sync_send_begin();
    int pub_ret = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
    azure_sync_send_end();
    if (pub_ret < 0) {
        ESP_LOGW(TAG, "Replay publish loi code=%d index=%ld", ent->event_code, (long)ent->index);
        return -1;
    }
    azure_note_publish(pub_ret, payload);
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

/** Doc so tu JSON: number hoac string "8300" (Azure Portal / backend hay gui string). */
static bool azure_json_get_i32(const cJSON *j, int32_t *out)
{
    if (!j || !out) {
        return false;
    }
    if (cJSON_IsNumber(j)) {
        *out = (int32_t)j->valuedouble;
        return true;
    }
    if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
        char *end = NULL;
        long v = strtol(j->valuestring, &end, 10);
        if (end != j->valuestring) {
            *out = (int32_t)v;
            return true;
        }
    }
    return false;
}

static void azure_parse_sync_data(cJSON *root, azure_sync_req_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!root) {
        return;
    }
    cJSON *data = cJSON_GetObjectItem(root, "Data");
    if (!data || !cJSON_IsObject(data)) {
        /* Mot so tool gui StartIdx o root — van chap nhan. */
        data = root;
    }

    cJSON *j = cJSON_GetObjectItem(data, "LastIdxSwipe");
    int32_t tmp = 0;
    if (azure_json_get_i32(j, &tmp)) {
        out->have_last_swipe = true;
        out->last_swipe = tmp;
    }
    j = cJSON_GetObjectItem(data, "LastIdxUnkn");
    if (azure_json_get_i32(j, &tmp)) {
        out->have_last_unkn = true;
        out->last_unkn = tmp;
    }
    j = cJSON_GetObjectItem(data, "LastIdxAdmin");
    if (azure_json_get_i32(j, &tmp)) {
        out->have_last_admin = true;
        out->last_admin = tmp;
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
            int32_t code_v = 0, idx_v = 0;
            if (azure_json_get_i32(c, &code_v) && azure_json_get_i32(ix, &idx_v)) {
                out->missing[out->missing_count].code = (int)code_v;
                out->missing[out->missing_count].index = idx_v;
                out->missing_count++;
            }
        }
    }

    cJSON *s = cJSON_GetObjectItem(data, "StartIdx");
    if (!s) {
        s = cJSON_GetObjectItem(data, "StartIndex");
    }
    cJSON *e = cJSON_GetObjectItem(data, "EndIdx");
    if (!e) {
        e = cJSON_GetObjectItem(data, "EndIndex");
    }
    int32_t start_idx = 0, end_idx = 0;
    if (azure_json_get_i32(s, &start_idx) && azure_json_get_i32(e, &end_idx)) {
        if (start_idx > 0 && end_idx >= start_idx) {
            cJSON *code_obj = cJSON_GetObjectItem(data, "CodeRange");
            int32_t code_range = 602;
            if (!azure_json_get_i32(code_obj, &code_range) || code_range <= 0) {
                code_range = 602;
            }
            out->have_range = true;
            out->range_start = start_idx;
            out->range_end = end_idx;
            out->range_code = (int)code_range;
        }
    }

    if (out->have_last_swipe || out->have_last_unkn || out->have_last_admin || out->missing_count > 0 ||
        out->have_range) {
        out->active = true;
        /* Tra Direct Method ngay (accepted); day MQTT/SD chay nen — tranh timeout Hub. */
        out->defer_response = false;
    }
}

static void azure_copy_json_str(cJSON *j, char *dst, size_t dstsz)
{
    /* Chi ghi de khi JSON co field string — khong xoa gia tri da parse (vd ConnectionString). */
    if (!dst || dstsz == 0 || !j || !cJSON_IsString(j) || !j->valuestring || !j->valuestring[0]) {
        return;
    }
    strncpy(dst, j->valuestring, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

/** Code 607: Host/DeviceId/SasKey rieng le, hoac 1 chuoi ConnectionString Azure IoT. */
static void azure_parse_change_hub_data(cJSON *data, char *host, size_t host_sz, char *devid, size_t devid_sz,
                                        char *sas, size_t sas_sz)
{
    if (!data || !cJSON_IsObject(data) || !host || !sas) {
        return;
    }
    host[0] = '\0';
    sas[0] = '\0';
    if (devid && devid_sz) {
        devid[0] = '\0';
    }

    static const char *const conn_keys[] = {"ConnectionString", "ConnString", "IoTHubConnectionString",
                                            "ConnectionStr", NULL};
    for (int i = 0; conn_keys[i]; i++) {
        cJSON *cj = cJSON_GetObjectItem(data, conn_keys[i]);
        if (cj && cJSON_IsString(cj) && cj->valuestring[0]) {
            (void)wifi_portal_parse_iot_conn_string(cj->valuestring, host, host_sz, devid, devid_sz, sas, sas_sz);
            break;
        }
    }

    cJSON *host_j = cJSON_GetObjectItem(data, "Host");
    if (!host_j) {
        host_j = cJSON_GetObjectItem(data, "HostName");
    }
    if (!host_j) {
        host_j = cJSON_GetObjectItem(data, "azure_host");
    }
    if (host_j && cJSON_IsString(host_j) && host_j->valuestring[0]) {
        if (strstr(host_j->valuestring, "HostName=") != NULL) {
            (void)wifi_portal_parse_iot_conn_string(host_j->valuestring, host, host_sz, devid, devid_sz, sas, sas_sz);
        } else {
            azure_copy_json_str(host_j, host, host_sz);
        }
    }

    cJSON *dev_j = cJSON_GetObjectItem(data, "DeviceId");
    if (!dev_j) {
        dev_j = cJSON_GetObjectItem(data, "DeviceID");
    }
    if (!dev_j) {
        dev_j = cJSON_GetObjectItem(data, "Device");
    }
    if (!dev_j) {
        dev_j = cJSON_GetObjectItem(data, "azure_devid");
    }
    azure_copy_json_str(dev_j, devid, devid_sz);

    cJSON *sas_j = cJSON_GetObjectItem(data, "SasKey");
    if (!sas_j) {
        sas_j = cJSON_GetObjectItem(data, "SASKey");
    }
    if (!sas_j) {
        sas_j = cJSON_GetObjectItem(data, "SharedAccessKey");
    }
    if (!sas_j) {
        sas_j = cJSON_GetObjectItem(data, "Key");
    }
    if (!sas_j) {
        sas_j = cJSON_GetObjectItem(data, "azure_sas_key");
    }
    azure_copy_json_str(sas_j, sas, sas_sz);
}

static void azure_pub_mtx_ensure(void)
{
    if (!s_azure_pub_mtx) {
        s_azure_pub_mtx = xSemaphoreCreateMutex();
    }
}

static void azure_wait_swipe_priority(void)
{
    for (int w = 0; (app_rfid_swipe_busy() || sd_card_service_waiting()) && w < 500; w++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/** Sync/replay: doi quet the gui xong, roi moi publish 1 ban tin. */
static void azure_sync_send_begin(void)
{
    azure_pub_mtx_ensure();
    for (;;) {
        /* Uu tien: dang quet / dang cho SD — dung sync, doi gui the xong. */
        azure_wait_swipe_priority();
        if (s_azure_pub_mtx) {
            (void)xSemaphoreTake(s_azure_pub_mtx, portMAX_DELAY);
        }
        if (app_rfid_swipe_busy() || sd_card_service_waiting()) {
            if (s_azure_pub_mtx) {
                xSemaphoreGive(s_azure_pub_mtx);
            }
            continue;
        }
        return;
    }
}

static void azure_sync_send_end(void)
{
    if (s_azure_pub_mtx) {
        xSemaphoreGive(s_azure_pub_mtx);
    }
}

/** Telemetry quet the live — lay MQTT ngay (sync phai doi). */
static void azure_live_send_begin(void)
{
    azure_pub_mtx_ensure();
    if (s_azure_pub_mtx) {
        (void)xSemaphoreTake(s_azure_pub_mtx, portMAX_DELAY);
    }
}

static void azure_live_send_end(void)
{
    if (s_azure_pub_mtx) {
        xSemaphoreGive(s_azure_pub_mtx);
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

    app_azure_tx_busy_begin();
    azure_wait_swipe_priority();

    if (azure_pend_has_data()) {
        azure_wait_swipe_priority();
        vTaskDelay(pdMS_TO_TICKS(3000));
        azure_wait_swipe_priority();
        pend_flushed = flush_pending_queue(client, dev_id);
    }

    if (req && req->active) {
        struct {
            esp_mqtt_client_handle_t client;
            const char *dev_id;
        } rctx = {.client = client, .dev_id = dev_id};

        /* LastIdx* + Missing[] (Missing van toi da 64 phan tu — khong doi). */
        if (req->have_last_swipe || req->have_last_unkn || req->have_last_admin || req->missing_count > 0) {
            azure_wait_swipe_priority();
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

            scan_log_replay_stats_t st;
            if (scan_log_replay_gaps(&filt, azure_replay_publish_cb, &rctx, &st) == ESP_OK) {
                log_resent += st.resent;
            }
        }

        /* StartIdx..EndIdx: tu chia batch 64, gui het khoang (API payload khong doi). */
        if (req->have_range) {
            int32_t cur = req->range_start;
            int batch = 0;
            while (cur <= req->range_end) {
                azure_wait_swipe_priority();
                scan_log_sync_filter_t filt;
                memset(&filt, 0, sizeof(filt));
                while (cur <= req->range_end && filt.missing_count < SCAN_LOG_SYNC_MISSING_MAX) {
                    filt.missing[filt.missing_count].code = req->range_code;
                    filt.missing[filt.missing_count].index = cur;
                    filt.missing_count++;
                    cur++;
                }
                batch++;
                ESP_LOGI(TAG, "605 range batch %d: code=%d idx %ld..%ld (%d)", batch, req->range_code,
                         (long)filt.missing[0].index,
                         (long)filt.missing[filt.missing_count - 1].index, filt.missing_count);
                scan_log_replay_stats_t st;
                if (scan_log_replay_gaps(&filt, azure_replay_publish_cb, &rctx, &st) == ESP_OK) {
                    log_resent += st.resent;
                }
            }
        }
    }

    app_azure_tx_busy_end();

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
        /* Accepted ngay — ResentFromLog/PendingFlushed = 0 (chua day); backend doi telemetry. */
        snprintf(res_payload, sizeof(res_payload),
                 "{\"status\":200,\"payload\":{\"Code\":605,\"TimeStamp\":%lld,"
                 "\"IdxSwipe\":%ld,\"IdxUnkn\":%ld,\"IdxAdmin\":%ld,"
                 "\"LastIdxSwipe\":%ld,\"LastIdxUnkn\":%ld,\"LastIdxAdmin\":%ld,"
                 "\"StartIdx\":%ld,\"EndIdx\":%ld,\"CodeRange\":%d,"
                 "\"PendingFlushed\":%d,\"ResentFromLog\":%d,"
                 "\"Accepted\":true,\"Message\":\"Sync accepted, pushing in background\"}}",
                 (long long)time(NULL), (long)idx_sw, (long)idx_un, (long)idx_ad,
                 req->have_last_swipe ? (long)req->last_swipe : -1L,
                 req->have_last_unkn ? (long)req->last_unkn : -1L,
                 req->have_last_admin ? (long)req->last_admin : -1L,
                 req->have_range ? (long)req->range_start : -1L,
                 req->have_range ? (long)req->range_end : -1L,
                 req->have_range ? req->range_code : 0, pend_flushed, log_resent);
    } else {
        snprintf(res_payload, sizeof(res_payload),
                 "{\"status\":200,\"payload\":{\"Code\":605,\"TimeStamp\":%lld,"
                 "\"PendingFlushed\":%d,\"Accepted\":true,\"Message\":\"Flush trigger accepted\"}}",
                 (long long)time(NULL), pend_flushed);
    }
    azure_dm_response(client, rid, COMMAND_STATUS_OK, res_payload);
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
        ESP_LOGI("boot", "Azure MQTT: LOI (khong co chi tiet)");
        return;
    }
    const esp_mqtt_error_codes_t *e = event->error_handle;
    const bool closed_fin = (e->esp_tls_last_esp_err == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN);
    ESP_LOGE(TAG,
             "MQTT loi uptime=%lld ms type=%s conn_rc=%d sock_errno=%d tls=%s",
             (long long)conn_uptime_ms,
             azure_mqtt_error_type_str(e->error_type),
             (int)e->connect_return_code,
             e->esp_transport_sock_errno,
             esp_err_to_name(e->esp_tls_last_esp_err));
    if (closed_fin) {
        /* Hub dong FIN sau khi da CONNECTED ~15-20s: gan nhu luon do client khac cung DeviceId. */
        ESP_LOGW(TAG,
                 "Hub dong ket noi (TCP FIN) — thuong do DeviceId dang dung boi Azure IoT Explorer / "
                 "backend / may ESP khac. Chi 1 client MQTT / device.");
        ESP_LOGI("boot", "Azure MQTT: Hub dong FIN — kiem tra DeviceId trung");
    } else {
        ESP_LOGI("boot", "Azure MQTT: LOI type=%s conn_rc=%d tls=%s",
                 azure_mqtt_error_type_str(e->error_type), (int)e->connect_return_code,
                 esp_err_to_name(e->esp_tls_last_esp_err));
    }
}

static void azure_note_disconnect(int64_t conn_uptime_ms)
{
    const int64_t now = azure_now_ms();
    static int64_t s_last_disc_log_ms;
    /* Log moi lan ngat (toi da 1 dong / 8s) — portal Terminal moi thay "Mat ket noi". */
    if (s_last_disc_log_ms == 0 || (now - s_last_disc_log_ms) >= 8000) {
        s_last_disc_log_ms = now;
        if (conn_uptime_ms < 0) {
            ESP_LOGW(TAG, "Azure MQTT: mat ket noi (chua tung CONNECTED — sai SAS/Host/Device hoac TLS)");
            ESP_LOGI("boot", "Azure MQTT: mat ket noi (chua CONNECTED)");
        } else {
            ESP_LOGW(TAG, "Azure MQTT: mat ket noi (da online %lld ms)", (long long)conn_uptime_ms);
            ESP_LOGI("boot", "Azure MQTT: mat ket noi");
        }
    }

    if (s_mqtt_disc_window_start_ms == 0 || (now - s_mqtt_disc_window_start_ms) > 120000) {
        s_mqtt_disc_window_start_ms = now;
        s_mqtt_disc_count = 0;
    }
    s_mqtt_disc_count++;

    if (s_mqtt_disc_count >= 3) {
        ESP_LOGE(TAG,
                 "MQTT ngat %u lan/2 phut — KIEM TRA: dong Azure IoT Explorer, backend/service "
                 "dung cung deviceId, chi 1 thiet bi ESP ket noi",
                 (unsigned)s_mqtt_disc_count);
        ESP_LOGI("boot", "Azure MQTT: ngat lap lai %u lan/2p — kiem tra deviceId trung",
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
    normalize_azure_host(cred->azure_host);
    trim_inplace(cred->azure_dev);
    trim_inplace(cred->azure_sas);
    return (cred->azure_host[0] != '\0' && cred->azure_dev[0] != '\0');
}

static void azure_wait_network_ready(void)
{
    ESP_LOGI(TAG, "Cho WiFi + gio NTP truoc khi ket noi Azure...");
    int64_t last_log_ms = 0;
    while (wifi_portal_get_conn_status() != WIFI_STATUS_CONNECTED ||
           !wifi_portal_time_is_valid()) {
        const int64_t now = azure_now_ms();
        if (last_log_ms == 0 || (now - last_log_ms) >= 10000) {
            last_log_ms = now;
            const bool wifi_ok = (wifi_portal_get_conn_status() == WIFI_STATUS_CONNECTED);
            const bool time_ok = wifi_portal_time_is_valid();
            ESP_LOGW(TAG, "Azure cho mang: WiFi=%s Gio=%s", wifi_ok ? "OK" : "chua",
                     time_ok ? "OK" : "chua NTP");
            ESP_LOGI("boot", "Azure cho: WiFi=%s Gio=%s", wifi_ok ? "OK" : "chua",
                     time_ok ? "OK" : "chua NTP");
        }
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

    {
        char payload[384];
        azure_format_payload(payload, sizeof(payload), event_code, msg_idx, r.timestamp_utc, uid, name, id);
        azure_log_outbound(payload, true);
    }

    xSemaphoreTake(s_sd_pend_mtx, portMAX_DELAY);
    sd_card_lock(); /* bảo vệ SPI bus SD */
    FILE *f = fopen(AZURE_PEND_SD_FILE, "ab");
    if (f) {
        fwrite(&r, sizeof(r), 1, f);
        fclose(f);
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

    char topic[192];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8", dev_id);

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
        static char payload[448];
        azure_format_payload(payload, sizeof(payload), (int)rec.event_code, rec.index, rec.timestamp_utc, rec.uid,
                             rec.name, rec.id);

        azure_sync_send_begin();
        int pub_ret = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
        azure_sync_send_end();
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
        azure_note_publish(pub_ret, payload);
        total_flushed++;
        /* Mo nhan the ~500ms; neu quet thi doi gui the xong moi ban tin tiep. */
        vTaskDelay(pdMS_TO_TICKS(500));
        azure_wait_swipe_priority();
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
            s_flush_requested = true;
            ESP_LOGI("boot", "Azure MQTT: OK");
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED: {
        const int64_t uptime = (s_mqtt_conn_ms > 0) ? (azure_now_ms() - s_mqtt_conn_ms) : -1;
        azure_note_disconnect(uptime);
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
                    bool is_ota_req = (strcmp(method_name, "OTA") == 0);
                    bool is_change_hub_req = (strcmp(method_name, "Hub") == 0);
                    bool is_dev_cmd = (strcmp(method_name, "Check") == 0);

                    // 1. Sai tên method (404 - không support)
                    if (!is_flush_req && !is_ota_req && !is_change_hub_req && !is_dev_cmd) {
                        if (rid[0] != '\0') {
                            char res_payload[192];
                            snprintf(res_payload, sizeof(res_payload),
                                     "{\"status\":404,\"payload\":{\"Message\":\"Method name not supported: %s\"}}",
                                     method_name);
                            azure_dm_response(event->client, rid, COMMAND_STATUS_NOT_FOUND, res_payload);
                        }
                        free(topic_str);
                        break;
                    }

                    // 2. Parse JSON & Kiem tra JSON loi (400 - Invalid JSON)
                    cJSON *root = NULL;
                    bool invalid_json = false;
                    if (event->data_len > 0) {
                        char *json_str = malloc(event->data_len + 1);
                        if (json_str) {
                            memcpy(json_str, event->data, event->data_len);
                            json_str[event->data_len] = '\0';
                            
                            char *trim_ptr = json_str;
                            while (*trim_ptr && isspace((unsigned char)*trim_ptr)) trim_ptr++;
                            if (strlen(trim_ptr) > 0) {
                                root = cJSON_Parse(json_str);
                                if (!root) {
                                    invalid_json = true;
                                }
                            }
                            free(json_str);
                        } else {
                            invalid_json = true;
                        }
                    }

                    if (invalid_json) {
                        if (rid[0] != '\0') {
                            char res_payload[128];
                            snprintf(res_payload, sizeof(res_payload),
                                     "{\"status\":400,\"payload\":{\"Message\":\"Invalid JSON\"}}");
                            azure_dm_response(event->client, rid, COMMAND_STATUS_BAD_REQUEST, res_payload);
                        }
                        free(topic_str);
                        break;
                    }

                    int method_code = 0;

                    // 3. Kiem tra Thieu Code / Data cho DeviceCommand (400)
                    if (is_dev_cmd) {
                        if (!root) {
                            if (rid[0] != '\0') {
                                char res_payload[128];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":400,\"payload\":{\"Message\":\"Missing Code or Data\"}}");
                                azure_dm_response(event->client, rid, COMMAND_STATUS_BAD_REQUEST, res_payload);
                            }
                            free(topic_str);
                            break;
                        }
                        cJSON *code = cJSON_GetObjectItem(root, "Code");
                        cJSON *data = cJSON_GetObjectItem(root, "Data");
                        if (!code || !cJSON_IsNumber(code) || !data || !cJSON_IsObject(data)) {
                            if (rid[0] != '\0') {
                                char res_payload[128];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":400,\"payload\":{\"Message\":\"Missing Code or Data\"}}");
                                azure_dm_response(event->client, rid, COMMAND_STATUS_BAD_REQUEST, res_payload);
                            }
                            cJSON_Delete(root);
                            free(topic_str);
                            break;
                        }
                        method_code = code->valueint;

                        // 4. Kiem tra Code la (400 - Unknown Code: ...)
                        bool known_code = (method_code == 500 || method_code == 600 || method_code == 603 ||
                                           method_code == 604 || method_code == 605 || method_code == 606 ||
                                           method_code == 607 || method_code == 611 || method_code == 612);
                        if (!known_code) {
                            if (rid[0] != '\0') {
                                char res_payload[192];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":400,\"payload\":{\"Message\":\"Unknown Code: %d\"}}",
                                         method_code);
                                azure_dm_response(event->client, rid, COMMAND_STATUS_BAD_REQUEST, res_payload);
                            }
                            cJSON_Delete(root);
                            free(topic_str);
                            break;
                        }
                    } else {
                        if (root) {
                            cJSON *code = cJSON_GetObjectItem(root, "Code");
                            if (code && cJSON_IsNumber(code)) {
                                method_code = code->valueint;
                            }
                        }
                    }

                    if (method_code == 605) {
                        is_flush_req = true;
                    } else if (method_code == 606) {
                        is_ota_req = true;
                    } else if (method_code == 607) {
                        is_change_hub_req = true;
                    }
                    
                    if (is_flush_req) {
                        azure_sync_req_t sync_copy;
                        memset(&sync_copy, 0, sizeof(sync_copy));
                        azure_parse_sync_data(root, &sync_copy);
                        s_sync_req = sync_copy;
                        s_flush_requested = true;

                        /* Log payload de doi chieu khi range=-1 (tool gui sai / string). */
                        if (root) {
                            char *dump = cJSON_PrintUnformatted(root);
                            if (dump) {
                                ESP_LOGI(TAG, "605 payload: %.400s", dump);
                                free(dump);
                            }
                        }
                        ESP_LOGI(TAG,
                                 "605 accepted (bg push): active=%d LastIdxSwipe=%ld LastIdxUnkn=%ld LastIdxAdmin=%ld "
                                 "missing=%d range=%ld..%ld codeRange=%d",
                                 sync_copy.active ? 1 : 0,
                                 sync_copy.have_last_swipe ? (long)sync_copy.last_swipe : -1L,
                                 sync_copy.have_last_unkn ? (long)sync_copy.last_unkn : -1L,
                                 sync_copy.have_last_admin ? (long)sync_copy.last_admin : -1L,
                                 sync_copy.missing_count,
                                 sync_copy.have_range ? (long)sync_copy.range_start : -1L,
                                 sync_copy.have_range ? (long)sync_copy.range_end : -1L,
                                 sync_copy.have_range ? sync_copy.range_code : 0);
                        if (!sync_copy.active) {
                            ESP_LOGW(TAG,
                                     "605: khong co StartIdx/EndIdx/LastIdx/Missing hop le — chi flush pending (neu co)");
                        }
                        if (rid[0] != '\0') {
                            /* Tra ngay — khong defer; day log chay sau trong azure_task. */
                            azure_send_sync_dm_response(event->client, rid, sync_copy.active ? &sync_copy : NULL, 0, 0);
                        }
                    } else if (is_ota_req) {
                        char ota_url[1024] = {0};
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
                                (void)azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
                            }
                            (void)app_ota_schedule_start(ota_url, 50);
                        } else {
                            ESP_LOGE(TAG, "Direct Method: Yeu cau TriggerOTA thieu tham so Url trong Data");
                            if (rid[0] != '\0') {
                                char res_payload[192];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":400,\"payload\":{\"Message\":\"Missing Url parameter\"}}");
                                azure_dm_response(event->client, rid, COMMAND_STATUS_BAD_REQUEST, res_payload);
                            }
                        }
                    } else if (is_change_hub_req) {
                        char new_host[64] = {0};
                        char new_dev[32] = {0};
                        char new_sas[128] = {0};

                        if (root) {
                            cJSON *data = cJSON_GetObjectItem(root, "Data");
                            azure_parse_change_hub_data(data, new_host, sizeof(new_host), new_dev, sizeof(new_dev),
                                                        new_sas, sizeof(new_sas));
                        }

                        if (new_host[0] != '\0' && new_sas[0] != '\0') {
                            ESP_LOGI(TAG, "Direct Method: Nhan lenh doi Hub Online (Code 607 / ChangeHub) sang Host: %s, DeviceId: %s",
                                     new_host, new_dev[0] ? new_dev : "(giu nguyen)");
                            esp_err_t err = wifi_portal_set_azure(new_host, new_dev[0] ? new_dev : NULL, new_sas);
                            if (err == ESP_OK) {
                                if (rid[0] != '\0') {
                                    char res_payload[256];
                                    snprintf(res_payload, sizeof(res_payload),
                                             "{\"status\":200,\"payload\":{\"Code\":607,\"TimeStamp\":%lld,"
                                             "\"Message\":\"Hub changed successfully. Rebooting...\"}}",
                                             (long long)time(NULL));
                                    azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
                                }
                                vTaskDelay(pdMS_TO_TICKS(1000));
                                esp_restart();
                            } else {
                                ESP_LOGE(TAG, "Direct Method ChangeHub: Luu NVS loi (%d)", err);
                                if (rid[0] != '\0') {
                                    char res_payload[192];
                                    snprintf(res_payload, sizeof(res_payload),
                                             "{\"status\":500,\"payload\":{\"Message\":\"Failed to save new Hub credentials\"}}");
                                    azure_dm_response(event->client, rid, COMMAND_STATUS_DEVICE_ERROR, res_payload);
                                }
                            }
                        } else {
                            ESP_LOGE(TAG, "Direct Method ChangeHub: Thieu Host/SasKey hoac ConnectionString");
                            if (rid[0] != '\0') {
                                char res_payload[192];
                                snprintf(res_payload, sizeof(res_payload),
                                         "{\"status\":400,\"payload\":{\"Message\":\"Missing Host/SasKey or ConnectionString in Data\"}}");
                                azure_dm_response(event->client, rid, COMMAND_STATUS_BAD_REQUEST, res_payload);
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
                                            azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
                                        }
                                    } else {
                                        ESP_LOGE(TAG, "603: Luu profile loi UID=%s", uid_str);
                                        if (rid[0] != '\0') {
                                            char res_payload[256];
                                            snprintf(res_payload, sizeof(res_payload),
                                                     "{\"status\":500,\"payload\":{\"Message\":\"Save failed\"}}");
                                            azure_dm_response(event->client, rid, COMMAND_STATUS_DEVICE_ERROR, res_payload);
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
                                        azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
                                    }
                                }
                            }
                        }
                    } else if (method_code == 500) {
                        /* Hoi phien ban firmware / build stamp / IP dang chay. */
                        const esp_app_desc_t *app = esp_app_get_description();
                        const esp_partition_t *run = esp_ota_get_running_partition();
                        const char *ver = (app && app->version[0]) ? app->version : "?";
                        const char *bld =
                            (g_app_build_stamp[0]) ? g_app_build_stamp : ((app && app->date[0]) ? app->date : "?");
                        const char *part = (run && run->label[0]) ? run->label : "?";
                        char sta_ip[20] = "";
                        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                        if (netif) {
                            esp_netif_ip_info_t ipi;
                            if (esp_netif_get_ip_info(netif, &ipi) == ESP_OK && ipi.ip.addr != 0) {
                                snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ipi.ip));
                            }
                        }
                        ESP_LOGI(TAG, "Direct Method 500: Version=%s Build=%s Part=%s IP=%s", ver, bld, part,
                                 sta_ip[0] ? sta_ip : "-");
                        if (rid[0] != '\0') {
                            char res_payload[384];
                            snprintf(res_payload, sizeof(res_payload),
                                     "{\"status\":200,\"payload\":{\"Code\":500,\"TimeStamp\":%lld,"
                                     "\"Version\":\"%s\",\"Build\":\"%s\",\"Partition\":\"%s\",\"IP\":\"%s\"}}",
                                     (long long)time(NULL), ver, bld, part, sta_ip);
                            azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
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
                            azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
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
                            azure_dm_response(event->client, rid, COMMAND_STATUS_OK, res_payload);
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
            ESP_LOGI("boot", "Azure: chua cau hinh");
            ESP_LOGW(TAG, "Chua thiet lap Azure thong qua WiFi Portal. Tam dung MQTT.");
            s_azure_task_handle = NULL;
            vTaskDeleteWithCaps(NULL);
            return;
        }

        ESP_LOGI("boot", "Azure: ket noi %s / %s", cred.azure_host, cred.azure_dev);
        ESP_LOGI(TAG, "Azure config: host=%s device=%s", cred.azure_host, cred.azure_dev);
        azure_wait_network_ready();

        while (!s_azure_config_reload) {
            while (s_azure_ota_suspend && !s_azure_config_reload) {
                azure_mqtt_disconnect();
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (s_azure_config_reload) {
                break;
            }
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
                ESP_LOGE(TAG, "Tao SAS token that bai — kiem tra SAS Key (Primary key). Thu lai sau 1 phut.");
                ESP_LOGI("boot", "Azure: SAS Key loi / decode fail");
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }

            char uri[128];
            snprintf(uri, sizeof(uri), "mqtts://%s:8883", cred.azure_host);
            ESP_LOGI(TAG, "MQTT TLS to host=%s (device=%s)", cred.azure_host, cred.azure_dev);
            ESP_LOGI("boot", "Azure dang ket noi MQTT...");

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
                        /* Mac dinh false: verify CA + CN/SAN khop azure_host (SNI tu URI). */
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
                    /* MQTT task Internal — 8KB du keepalive; azure_task (SPIRAM) lo phan nang. */
                    .stack_size = 8192,
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

            for (int i = 0; i < 720 * 12 && !s_azure_config_reload && !s_azure_ota_suspend; i++) {
                for (int t = 0; t < 100 && !s_azure_config_reload && !s_azure_ota_suspend; t++) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (s_azure_config_reload || s_azure_ota_suspend) {
                    break;
                }
                if (s_flush_requested && s_azure_connected && s_mqtt_client) {
                    s_flush_requested = false;
                    azure_sync_req_t sync_copy = s_sync_req;
                    memset(&s_sync_req, 0, sizeof(s_sync_req));
                    /* s_sync_dm khong dung defer nua — chi day nen. */
                    s_sync_dm.pending = false;
                    s_sync_dm.rid[0] = '\0';

                    if (azure_pend_has_data() || sync_copy.active) {
                        int pend_flushed = 0;
                        int log_resent = 0;
                        ESP_LOGI(TAG, "605/Flush bg: bat dau (pend=%d sync=%d)...", azure_pend_has_data() ? 1 : 0,
                                 sync_copy.active ? 1 : 0);
                        azure_run_sync_work(s_mqtt_client, cred.azure_dev, sync_copy.active ? &sync_copy : NULL,
                                            &pend_flushed, &log_resent);
                        ESP_LOGI(TAG, "605/Flush bg: xong PendingFlushed=%d ResentFromLog=%d", pend_flushed,
                                 log_resent);
                    }
                }
            }

            azure_mqtt_disconnect();

            if (s_azure_config_reload || s_azure_ota_suspend) {
                continue;
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

void app_azure_suspend_for_ota(void)
{
    ESP_LOGI(TAG, "OTA: yeu cau azure_task ngat MQTT");
    s_azure_ota_suspend = true;
}

bool app_azure_wait_suspended(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (s_mqtt_client == NULL && !s_azure_connected) {
            ESP_LOGI(TAG, "OTA: MQTT da ngat (%u ms)", (unsigned)elapsed);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }
    ESP_LOGW(TAG, "OTA: MQTT chua ngat het sau %u ms (client=%p connected=%d)",
             (unsigned)timeout_ms, (void *)s_mqtt_client, (int)s_azure_connected);
    return s_mqtt_client == NULL;
}

void app_azure_resume_after_ota(void)
{
    ESP_LOGI(TAG, "OTA fail: cho phep MQTT ket noi lai");
    s_azure_ota_suspend = false;
}

void app_azure_start(void)
{
    if (!s_ntp_done_sem) {
        s_ntp_done_sem = xSemaphoreCreateBinary();
    }
    if (s_azure_task_handle != NULL) {
        return;
    }
    BaseType_t res = xTaskCreatePinnedToCoreWithCaps(azure_task, "azure_task", 32768, NULL, 5,
                                                     &s_azure_task_handle, 0,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (res != pdPASS) {
        res = xTaskCreatePinnedToCoreWithCaps(azure_task, "azure_task", 32768, NULL, 5,
                                              &s_azure_task_handle, 0,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
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
        return;
    }

    /* Nếu chưa kết nối: lưu vào hàng đợi offline (SD), đợi flush khi có mạng */
    if (!s_azure_connected || !s_mqtt_client) {
        pending_enqueue(uid, name, id, code_val, msg_idx);
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

    char topic[192];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8", cred.azure_dev);

    char payload[384];
    azure_format_payload(payload, sizeof(payload), code_val, msg_idx, (int64_t)now, uid, name, id);

    app_azure_tx_busy_begin();
    azure_live_send_begin();
    int pub_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    azure_live_send_end();
    app_azure_tx_busy_end();
    azure_note_publish(pub_id, payload);
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
        return;
    }

    if (!s_azure_connected || !s_mqtt_client) {
        pending_enqueue(uid, name, id, event_code, msg_idx);
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

    char topic[192];
    snprintf(topic, sizeof(topic), "devices/%s/messages/events/$.ct=application%%2Fjson&$.ce=utf-8", cred.azure_dev);

    char payload[384];
    azure_format_payload(payload, sizeof(payload), event_code, msg_idx, (int64_t)now, uid, name, id);

    app_azure_tx_busy_begin();
    azure_live_send_begin();
    int pub_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    azure_live_send_end();
    app_azure_tx_busy_end();
    azure_note_publish(pub_id, payload);
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

    app_azure_tx_busy_begin();
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
    app_azure_tx_busy_end();
    return total_resent;
}

void app_azure_tx_busy_begin(void)
{
    s_azure_tx_busy++;
}

void app_azure_tx_busy_end(void)
{
    if (s_azure_tx_busy > 0) {
        s_azure_tx_busy--;
    }
}

bool app_azure_tx_busy(void)
{
    return s_azure_tx_busy > 0;
}

int app_azure_is_connected(void)
{
    return s_azure_connected ? 1 : 0;
}
