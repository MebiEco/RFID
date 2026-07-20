#include "wifi_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "scan_log.h"
#include "app_azure.h"
#include "app_rfid.h"
#include "portal_web.h"

static const char *TAG = "wifi_portal";

#define NVS_NS "wifi_portal"
#define NVS_KEY "cred"
#define NVS_KEY_WALL_UTC "wall_utc"
#define CRED_MAGIC 0x57494649u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    char ssid[32];
    char pass[64];
    char azure_host[64];
    char azure_dev[32];
    char azure_sas[128];
} wifi_cred_t;

#define NVS_KEY_WIFIS "wifis"
#define NVS_KEY_STA_IDX "sta_idx" /** Chi so AP trong wifis[] da ket noi tot (tiet kiem boot). */
#define WIFIS_MAGIC 0x57494C53u // 'WILS'
#define MAX_WIFIS 5

typedef struct {
    char ssid[32];
    char pass[64];
} saved_wifi_t;

typedef struct {
    uint32_t magic;
    uint8_t count;
    saved_wifi_t wifis[MAX_WIFIS];
} wifi_list_t;

static wifi_list_t s_wifi_list;
static uint8_t s_current_wifi_idx = 0;

static void copy_field(char *dst, size_t dstsz, const char *src)
{
    snprintf(dst, dstsz, "%s", src ? src : "");
}

/** HostName Azure: bỏ khoảng trắng, mqtts://, :8883 — tránh TLS verify fail do CN/SAN lệch. */
static void normalize_azure_host(char *host)
{
    if (!host || !host[0]) {
        return;
    }
    char *s = host;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    if (s != host) {
        memmove(host, s, strlen(s) + 1);
    }
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
    size_t n = strlen(host);
    while (n > 0 && (host[n - 1] == ' ' || host[n - 1] == '\t')) {
        host[--n] = '\0';
    }
    for (char *p = host; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
}

static void wifi_list_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        s_wifi_list.magic = WIFIS_MAGIC;
        nvs_set_blob(h, NVS_KEY_WIFIS, &s_wifi_list, sizeof(s_wifi_list));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void wifi_list_load(void)
{
    memset(&s_wifi_list, 0, sizeof(s_wifi_list));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_wifi_list);
        if (nvs_get_blob(h, NVS_KEY_WIFIS, &s_wifi_list, &sz) != ESP_OK || sz != sizeof(s_wifi_list) || s_wifi_list.magic != WIFIS_MAGIC) {
            s_wifi_list.count = 0;
        }
        nvs_close(h);
    }
    s_wifi_list.magic = WIFIS_MAGIC;
}

void wifi_list_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    for (int i = 0; i < s_wifi_list.count; i++) {
        if (strcmp(s_wifi_list.wifis[i].ssid, ssid) == 0) {
            copy_field(s_wifi_list.wifis[i].pass, sizeof(s_wifi_list.wifis[i].pass), pass);
            wifi_list_save();
            return;
        }
    }
    if (s_wifi_list.count < MAX_WIFIS) {
        copy_field(s_wifi_list.wifis[s_wifi_list.count].ssid, 32, ssid);
        copy_field(s_wifi_list.wifis[s_wifi_list.count].pass, 64, pass);
        s_wifi_list.count++;
    } else {
        for (int i = 1; i < MAX_WIFIS; i++) s_wifi_list.wifis[i-1] = s_wifi_list.wifis[i];
        copy_field(s_wifi_list.wifis[MAX_WIFIS-1].ssid, 32, ssid);
        copy_field(s_wifi_list.wifis[MAX_WIFIS-1].pass, 64, pass);
    }
    wifi_list_save();
}

int wifi_list_get_count(void) {
    return s_wifi_list.count;
}

void wifi_list_get_item(int idx, char *ssid, char *pass) {
    if (idx >= 0 && idx < s_wifi_list.count) {
        if (ssid) strcpy(ssid, s_wifi_list.wifis[idx].ssid);
        if (pass) strcpy(pass, s_wifi_list.wifis[idx].pass);
    } else {
        if (ssid) ssid[0] = '\0';
        if (pass) pass[0] = '\0';
    }
}

/** SoftAP WPA2 — mật khẩu AP_PASS; vào http://192.168.4.1 sau khi nối */
#define AP_SSID "Defaufl-AP"
#define AP_PASS "12345678"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4

/** Tu kiem tra portal; chi restart httpd khi mat phan hoi (khong reboot ESP). */
#define PORTAL_HEALTH_INTERVAL_SEC      3600
#define PORTAL_HEALTH_INITIAL_DELAY_SEC  600
#define PORTAL_HEALTH_FAIL_RESTART       1
/* LWIP_MAX_SOCKETS=10 → httpd can dung toi da 7 (7+3 noi bo). */
#define PORTAL_HTTP_MAX_SOCKETS           7

static httpd_handle_t s_server;
static bool s_sntp_started;
static bool s_time_synced;
static esp_netif_t *s_sta_netif;
static bool s_sntp_retry_task_live;
static uint8_t s_ntp_server_idx;

/** Xoay vong khi retry (LWIP chi cho 1 server trong config). */
static const char *s_ntp_servers[] = {
    "time.google.com",
    "pool.ntp.org",
    "vn.pool.ntp.org",
};

/** true: co NVS WiFi — cho phep tu ket noi lai sau khi mat STA */
static bool s_sta_auto_reconnect;
/** So lan da tu goi lai connect sau disconnect (reset khi co IP) */
static uint8_t s_sta_reconnect_count;
static volatile bool s_sta_reconnect_task_live;
static wifi_conn_status_t s_wifi_conn_status = WIFI_STATUS_IDLE;
static char s_pending_ssid[32];
static char s_pending_pass[64];

static void sta_apply_current_wifi(void);

static void sta_current_idx_load(void)
{
    if (s_wifi_list.count == 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = 0;
    esp_err_t e = nvs_get_u8(h, NVS_KEY_STA_IDX, &v);
    nvs_close(h);
    if (e == ESP_OK && v < s_wifi_list.count) {
        s_current_wifi_idx = v;
    }
}

static void sta_current_idx_save(void)
{
    if (s_wifi_list.count == 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(h, NVS_KEY_STA_IDX, s_current_wifi_idx);
    (void)nvs_commit(h);
    nvs_close(h);
}

/** Scan chon AP manh nhat — hien khong goi (boot nhanh). Giu de mo rong (sau N lan that bai). */
__attribute__((unused)) static int match_strongest_wifi(void)
{
    if (s_wifi_list.count <= 1) return s_wifi_list.count == 1 ? 0 : -1;

    wifi_scan_config_t scan_config = {
        .ssid = 0, .bssid = 0, .channel = 0, .show_hidden = false
    };
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return -1;

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return -1;

    wifi_ap_record_t *aps = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!aps) return -1;

    esp_wifi_scan_get_ap_records(&ap_count, aps);

    int best_idx = -1;
    int best_rssi = -1000;

    for (int i = 0; i < s_wifi_list.count; i++) {
        for (int j = 0; j < ap_count; j++) {
            if (strcmp((char *)aps[j].ssid, s_wifi_list.wifis[i].ssid) == 0) {
                if (aps[j].rssi > best_rssi) {
                    best_rssi = aps[j].rssi;
                    best_idx = i;
                }
            }
        }
    }
    free(aps);
    return best_idx;
}

static void sta_delayed_connect_task(void *arg)
{
    (void)arg;
    uint32_t dly = 1500;
    if (s_wifi_list.count > 0 && s_sta_reconnect_count > s_wifi_list.count * 3) {
        dly = 15000;
    }
    vTaskDelay(pdMS_TO_TICKS(dly));
    s_sta_reconnect_task_live = false;
    
    if (s_sta_auto_reconnect && s_wifi_list.count > 0) {
        if (s_sta_reconnect_count % 3 == 0) { // Try each AP 3 times then switch
            s_current_wifi_idx = (s_current_wifi_idx + 1) % s_wifi_list.count;
            sta_apply_current_wifi();
        } else {
            s_wifi_conn_status = WIFI_STATUS_CONNECTING;
            esp_wifi_connect();
        }
    }
    vTaskDelete(NULL);
}

/** Goi tu WIFI_EVENT_STA_DISCONNECTED — khong block event loop */
static void schedule_sta_reconnect(void)
{
    if (!s_sta_auto_reconnect || s_sta_reconnect_task_live || s_wifi_list.count == 0) {
        return;
    }
    s_sta_reconnect_count++;
    s_sta_reconnect_task_live = true;
    if (xTaskCreate(sta_delayed_connect_task, "sta_reco", 3072, NULL, 4, NULL) != pdPASS) {
        s_sta_reconnect_task_live = false;
        ESP_LOGW(TAG, "xTaskCreate sta_reco failed");
    }
}

static esp_err_t cred_load(wifi_cred_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(*out);
    err = nvs_get_blob(h, NVS_KEY, out, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(*out) || out->magic != CRED_MAGIC) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t cred_save(const wifi_cred_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, in, sizeof(*in));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t cred_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void sta_apply_current_wifi(void)
{
    if (s_wifi_list.count == 0) {
        s_sta_auto_reconnect = false;
        return;
    }
    s_sta_auto_reconnect = true;
    if (s_current_wifi_idx >= s_wifi_list.count) s_current_wifi_idx = 0;
    
    wifi_config_t w = {0};
    copy_field((char *)w.sta.ssid, sizeof(w.sta.ssid), s_wifi_list.wifis[s_current_wifi_idx].ssid);
    copy_field((char *)w.sta.password, sizeof(w.sta.password), s_wifi_list.wifis[s_current_wifi_idx].pass);
    w.sta.threshold.authmode = WIFI_AUTH_OPEN;
    w.sta.pmf_cfg.capable = true;
    w.sta.pmf_cfg.required = false;
    ESP_LOGI(TAG, "STA se ket noi WiFi %d/%d: %s", s_current_wifi_idx+1, s_wifi_list.count, w.sta.ssid);
    s_wifi_conn_status = WIFI_STATUS_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    esp_wifi_disconnect();
    esp_wifi_connect();
}
void wifi_portal_connect_to(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    wifi_config_t w = {0};
    strncpy((char *)w.sta.ssid, ssid, sizeof(w.sta.ssid) - 1);
    if (pass) strncpy((char *)w.sta.password, pass, sizeof(w.sta.password) - 1);
    w.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    w.sta.pmf_cfg.capable = true;
    w.sta.pmf_cfg.required = false;
    
    ESP_LOGI(TAG, "Yeu cau ket noi ngay lap tuc den: %s", ssid);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &w);
    s_wifi_conn_status = WIFI_STATUS_CONNECTING;
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    strncpy(s_pending_pass, pass ? pass : "", sizeof(s_pending_pass) - 1);
    esp_wifi_connect();
}

wifi_conn_status_t wifi_portal_get_conn_status(void)
{
    return s_wifi_conn_status;
}

void wifi_portal_get_azure(char *host, size_t host_sz, char *devid, size_t dev_sz, char *sas_mask,
                           size_t sas_sz)
{
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    if (host && host_sz) {
        host[0] = '\0';
    }
    if (devid && dev_sz) {
        devid[0] = '\0';
    }
    if (sas_mask && sas_sz) {
        sas_mask[0] = '\0';
    }
    if (cred_load(&c) != ESP_OK) {
        return;
    }
    if (host && host_sz) {
        copy_field(host, host_sz, c.azure_host);
    }
    if (devid && dev_sz) {
        copy_field(devid, dev_sz, c.azure_dev);
    }
    if (sas_mask && sas_sz && c.azure_sas[0]) {
        size_t n = strlen(c.azure_sas);
        if (n <= 4) {
            snprintf(sas_mask, sas_sz, "****");
        } else {
            snprintf(sas_mask, sas_sz, "****%s", c.azure_sas + n - 4);
        }
    }
}

static void sta_clear_config(void)
{
    s_sta_auto_reconnect = false;
    wifi_config_t w;
    memset(&w, 0, sizeof(w));
    esp_wifi_set_config(WIFI_IF_STA, &w);
    esp_wifi_disconnect();
}

void wifi_list_remove(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return;
    }
    int found = -1;
    for (int i = 0; i < s_wifi_list.count; i++) {
        if (strcmp(s_wifi_list.wifis[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return;
    }

    if (found < (int)s_current_wifi_idx) {
        s_current_wifi_idx--;
    } else if (found == (int)s_current_wifi_idx && s_wifi_list.count > 1 &&
               found == (int)s_wifi_list.count - 1) {
        s_current_wifi_idx = (uint8_t)(s_wifi_list.count - 2);
    }

    for (int i = found; i < s_wifi_list.count - 1; i++) {
        s_wifi_list.wifis[i] = s_wifi_list.wifis[i + 1];
    }
    s_wifi_list.count--;
    wifi_list_save();

    if (strcmp(s_pending_ssid, ssid) == 0) {
        s_pending_ssid[0] = '\0';
    }

    wifi_cred_t c;
    if (cred_load(&c) == ESP_OK && strcmp(c.ssid, ssid) == 0) {
        memset(c.ssid, 0, sizeof(c.ssid));
        memset(c.pass, 0, sizeof(c.pass));
        cred_save(&c);
    }

    if (s_current_wifi_idx >= s_wifi_list.count && s_wifi_list.count > 0) {
        s_current_wifi_idx = (uint8_t)(s_wifi_list.count - 1);
    }

    if (s_wifi_list.count == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, NVS_KEY_STA_IDX);
            nvs_commit(h);
            nvs_close(h);
        }
        s_wifi_conn_status = WIFI_STATUS_IDLE;
        sta_clear_config();
    } else {
        sta_current_idx_save();
        sta_apply_current_wifi();
    }
}

/** Giải mã %XX đơn giản (form ngắn). */
static void json_escape_ssid(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in) {
        in = "";
    }
    for (size_t i = 0; in[i] && j + 2 < outsz; i++) {
        char ch = in[i];
        if (ch == '"' || ch == '\\') {
            out[j++] = '\\';
            out[j++] = ch;
        } else if ((unsigned char)ch >= 0x20) {
            out[j++] = ch;
        }
    }
    out[j] = '\0';
}

/** Giải mã %XX đơn giản (form ngắn). */
static void url_decode_inplace(char *s)
{
    char *dst = s;
    while (*s) {
        if (*s == '+') {
            *dst++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            unsigned v;
            if (sscanf(s + 1, "%2x", &v) == 1) {
                *dst++ = (char)v;
                s += 3;
            } else {
                *dst++ = *s++;
            }
        } else {
            *dst++ = *s++;
        }
    }
    *dst = '\0';
}

static int form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) {
        return -1;
    }
    char prefix1[64];
    snprintf(prefix1, sizeof(prefix1), "%s=", key);
    char prefix2[64];
    snprintf(prefix2, sizeof(prefix2), "&%s=", key);
    const char *p = NULL;
    if (strncmp(body, prefix1, strlen(prefix1)) == 0) {
        p = body + strlen(prefix1);
    } else {
        p = strstr(body, prefix2);
        if (p) {
            p += strlen(prefix2);
        }
    }
    if (!p) {
        return -1;
    }
    size_t i = 0;
    while (i < out_len - 1 && *p && *p != '&') {
        out[i++] = *p++;
    }
    out[i] = '\0';
    url_decode_inplace(out);
    return 0;
}

static esp_err_t save_wifi_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "wifi")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Forbidden");
    }

    char buf[384];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) return ESP_FAIL;
    buf[rlen] = '\0';

    char ssid[32] = {0}, pass[64] = {0};
    if (form_get(buf, "ssid", ssid, sizeof(ssid)) != 0 || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Thieu SSID");
        return ESP_OK;
    }
    (void)form_get(buf, "pass", pass, sizeof(pass));

    wifi_list_add(ssid, pass);
    s_current_wifi_idx = s_wifi_list.count - 1; // Ưu tiên mạng vừa thêm
    s_sta_reconnect_count = 0;
    sta_apply_current_wifi();

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t saved_wifis_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "wifi")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "[]");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    char buf[128];
    char esc[68];
    for (int i = 0; i < s_wifi_list.count; i++) {
        json_escape_ssid(s_wifi_list.wifis[i].ssid, esc, sizeof(esc));
        snprintf(buf, sizeof(buf), "\"%s\"%s", esc, (i < s_wifi_list.count - 1) ? "," : "");
        httpd_resp_send_chunk(req, buf, strlen(buf));
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t del_wifi_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "wifi")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Forbidden");
    }
    char buf[128];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    if (rlen <= 0) {
        return ESP_FAIL;
    }

    char ssid[32] = {0};
    if (form_get(buf, "ssid", ssid, sizeof(ssid)) == 0 && ssid[0] != '\0') {
        wifi_list_remove(ssid);
        httpd_resp_sendstr(req, "OK");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
    }
    return ESP_OK;
}

static esp_err_t save_azure_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "azure")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Forbidden");
    }

    char buf[384];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) return ESP_FAIL;
    buf[rlen] = '\0';

    char host[64] = {0}, devid[32] = {0}, sas[128] = {0};
    (void)form_get(buf, "azure_host", host, sizeof(host));
    (void)form_get(buf, "azure_devid", devid, sizeof(devid));
    (void)form_get(buf, "azure_sas_key", sas, sizeof(sas));
    normalize_azure_host(host);

    if (host[0] == '\0' || devid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "HostName và Device ID không được để trống!");
    }
    if (sas[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Cần nhập SAS Key (Primary key)!");
    }

    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    cred_load(&c); // Giữ info WiFi cũ

    c.magic = CRED_MAGIC;
    copy_field(c.azure_host, sizeof(c.azure_host), host);
    copy_field(c.azure_dev, sizeof(c.azure_dev), devid);
    copy_field(c.azure_sas, sizeof(c.azure_sas), sas);
    cred_save(&c);

    app_azure_notify_config_changed();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t clear_azure_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "azure")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Forbidden");
    }
    
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    cred_load(&c);
    c.magic = CRED_MAGIC;
    memset(c.azure_host, 0, sizeof(c.azure_host));
    memset(c.azure_dev, 0, sizeof(c.azure_dev));
    memset(c.azure_sas, 0, sizeof(c.azure_sas));
    cred_save(&c);
    
    ESP_LOGI(TAG, "Da xoa cau hinh Azure khoi NVS");
    app_azure_notify_config_changed();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t clear_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "wifi")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Forbidden");
    }
    char discard[64];
    if (req->content_len > 0) {
        (void)httpd_req_recv(req, discard, sizeof(discard) < req->content_len ? sizeof(discard) : req->content_len);
    }
    
    // Xóa list wifi
    s_wifi_list.count = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_WIFIS);
        nvs_commit(h);
        nvs_close(h);
    }
    
    /* Chỉ xóa ssid/pass — GIỮ NGUYÊN Azure credentials trong cùng blob */
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    cred_load(&c);              /* Đọc blob hiện tại (giữ azure_host/dev/sas) */
    memset(c.ssid, 0, sizeof(c.ssid));
    memset(c.pass, 0, sizeof(c.pass));
    c.magic = CRED_MAGIC;
    cred_save(&c);              /* Ghi lại: ssid/pass trắng, Azure còn nguyên */
    sta_clear_config();
    ESP_LOGI(TAG, "Da xoa ALL WiFi trong NVS");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t scans_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "log")) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req,
                                  "<!DOCTYPE html><html><head><meta charset=utf-8></head><body>"
                                  "<p>Can nhap PIN muc Nhat ky tren trang chu.</p><p><a href=/ >Ve portal</a></p></body></html>");
    }
    return scan_log_send_html_page(req);
}

#define WIFI_SCAN_CACHE_MAX   32
#define WIFI_SCAN_CACHE_TTL_US (30LL * 1000000)

#define WIFI_SCAN_TIMEOUT_US (15LL * 1000000)

typedef struct {
    bool valid;
    bool scanning;
    int64_t cache_us;
    int64_t scan_start_us;
    uint16_t count;
    wifi_ap_record_t aps[WIFI_SCAN_CACHE_MAX];
} wifi_scan_cache_t;

static wifi_scan_cache_t s_wifi_scan;

static void wifi_scan_cache_fill(void)
{
    uint16_t ap_count = 0;
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        s_wifi_scan.valid = false;
        s_wifi_scan.scanning = false;
        return;
    }
    if (ap_count > WIFI_SCAN_CACHE_MAX) {
        ap_count = WIFI_SCAN_CACHE_MAX;
    }
    s_wifi_scan.count = ap_count;
    if (ap_count > 0) {
        (void)esp_wifi_scan_get_ap_records(&ap_count, s_wifi_scan.aps);
        s_wifi_scan.count = ap_count;
    }
    s_wifi_scan.valid = true;
    s_wifi_scan.scanning = false;
    s_wifi_scan.cache_us = esp_timer_get_time();
}

static esp_err_t wifi_scan_send_cached(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send_chunk(req, "[", 1);
    if (e != ESP_OK) {
        return e;
    }
    char buf[128];
    for (int i = 0; i < (int)s_wifi_scan.count; i++) {
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d}%s",
                 (char *)s_wifi_scan.aps[i].ssid, s_wifi_scan.aps[i].rssi,
                 (i < (int)s_wifi_scan.count - 1) ? "," : "");
        e = httpd_resp_send_chunk(req, buf, strlen(buf));
        if (e != ESP_OK) {
            return e;
        }
    }
    e = httpd_resp_send_chunk(req, "]", 1);
    if (e != ESP_OK) {
        return e;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "wifi")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "[]");
    }

    bool force_fresh = false;
    char qry[16];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        char v[4];
        if (httpd_query_key_value(qry, "fresh", v, sizeof(v)) == ESP_OK && v[0] == '1') {
            force_fresh = true;
        }
    }

    const int64_t now_us = esp_timer_get_time();
    if (!force_fresh && s_wifi_scan.valid && !s_wifi_scan.scanning &&
        (now_us - s_wifi_scan.cache_us) < WIFI_SCAN_CACHE_TTL_US) {
        return wifi_scan_send_cached(req);
    }

    if (s_wifi_scan.scanning) {
        if ((now_us - s_wifi_scan.scan_start_us) > WIFI_SCAN_TIMEOUT_US) {
            ESP_LOGW(TAG, "WiFi scan timeout — huy va tra cache");
            (void)esp_wifi_scan_stop();
            wifi_scan_cache_fill();
        } else {
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"scanning\":true}");
        }
    }

    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };
    esp_err_t scan_err = esp_wifi_scan_start(&scan_config, false);
    if (scan_err == ESP_ERR_WIFI_STATE) {
        s_wifi_scan.scanning = true;
        s_wifi_scan.scan_start_us = now_us;
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"scanning\":true}");
    }
    if (scan_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    s_wifi_scan.scanning = true;
    s_wifi_scan.valid = false;
    s_wifi_scan.scan_start_us = now_us;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"scanning\":true}");
}

static esp_err_t ping_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, "ok");
}

static bool portal_http_probe(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        static const char req[] =
            "GET /api/ping HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        if (send(sock, req, sizeof(req) - 1, 0) > 0) {
            char buf[32];
            int n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                ok = (strstr(buf, "200") != NULL);
            }
        }
    }
    close(sock);
    return ok;
}

static esp_err_t stop_httpd(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    esp_err_t e = httpd_stop(s_server);
    s_server = NULL;
    return e;
}

static esp_err_t restart_httpd(void);

static void portal_health_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS((uint32_t)PORTAL_HEALTH_INITIAL_DELAY_SEC * 1000U));

    int fail_streak = 0;
    for (;;) {
        if (portal_http_probe()) {
            if (fail_streak > 0) {
                ESP_LOGI(TAG, "Portal health OK (truoc do fail %d lan)", fail_streak);
            }
            fail_streak = 0;
        } else {
            fail_streak++;
            ESP_LOGW(TAG, "Portal health FAIL (%d/%d)", fail_streak, PORTAL_HEALTH_FAIL_RESTART);
            if (fail_streak >= PORTAL_HEALTH_FAIL_RESTART) {
                ESP_LOGW(TAG, "Restart httpd — portal khong phan hoi");
                if (restart_httpd() == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    if (portal_http_probe()) {
                        ESP_LOGI(TAG, "Portal phuc hoi sau restart httpd");
                        fail_streak = 0;
                    } else {
                        ESP_LOGE(TAG, "Portal van loi sau restart httpd");
                        fail_streak = 1;
                    }
                } else {
                    ESP_LOGE(TAG, "restart httpd that bai");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS((uint32_t)PORTAL_HEALTH_INTERVAL_SEC * 1000U));
    }
}

static bool s_portal_health_started;

static void portal_health_start(void)
{
    if (s_portal_health_started) {
        return;
    }
    if (xTaskCreate(portal_health_task, "portal_hlth", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Khong tao duoc portal health task");
        return;
    }
    s_portal_health_started = true;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Gioi han URI/header: ESP-IDF 5.3 chi co CONFIG_HTTPD_MAX_* trong sdkconfig (khong co field trong httpd_config_t). */
    /* /scans dung nhieu buffer tren stack; mac dinh 4096 de tran stack overflow -> reset. */
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = PORTAL_HTTP_MAX_SOCKETS;
    cfg.keep_alive_enable = false;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start that bai: %s (max_open_sockets=%u)", esp_err_to_name(err),
                 (unsigned)cfg.max_open_sockets);
        return err;
    }

    httpd_uri_t u_ping = { .uri = "/api/ping", .method = HTTP_GET, .handler = ping_get_handler, .user_ctx = NULL };
    httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = portal_root_get_handler, .user_ctx = NULL };
    httpd_uri_t u_save_w = { .uri = "/save_wifi", .method = HTTP_POST, .handler = save_wifi_post_handler, .user_ctx = NULL };
    httpd_uri_t u_save_a = { .uri = "/save_azure", .method = HTTP_POST, .handler = save_azure_post_handler, .user_ctx = NULL };
    httpd_uri_t u_clear_a = { .uri = "/clear_azure", .method = HTTP_POST, .handler = clear_azure_post_handler, .user_ctx = NULL };
    httpd_uri_t u_clear = { .uri = "/clear", .method = HTTP_POST, .handler = clear_post_handler, .user_ctx = NULL };
    httpd_uri_t u_scans = { .uri = "/scans", .method = HTTP_GET, .handler = scans_get_handler, .user_ctx = NULL };
    httpd_uri_t u_scanwifi = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
    httpd_uri_t u_saved_wifis = { .uri = "/saved_wifis", .method = HTTP_GET, .handler = saved_wifis_get_handler, .user_ctx = NULL };
    httpd_uri_t u_del_wifi = { .uri = "/del_wifi", .method = HTTP_POST, .handler = del_wifi_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &u_ping);
    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_save_w);
    httpd_register_uri_handler(s_server, &u_save_a);
    httpd_register_uri_handler(s_server, &u_clear_a);
    httpd_register_uri_handler(s_server, &u_clear);
    httpd_register_uri_handler(s_server, &u_scans);
    httpd_register_uri_handler(s_server, &u_scanwifi);
    httpd_register_uri_handler(s_server, &u_saved_wifis);
    httpd_register_uri_handler(s_server, &u_del_wifi);
    portal_web_register_handlers(s_server);
    ESP_LOGI(TAG, "Web: http://192.168.4.1/ — menu trai, PIN theo muc (AP: %s)", AP_SSID);
    return ESP_OK;
}

static esp_err_t restart_httpd(void)
{
    (void)stop_httpd();
    vTaskDelay(pdMS_TO_TICKS(300));
    return start_httpd();
}

static void time_synced_cb(struct timeval *tv);
static void sntp_adopt_wall_time_if_valid(void);

static bool wall_time_valid(time_t utc)
{
    struct tm t;
    scan_log_wall_tm(utc, &t);
    return t.tm_year >= (2020 - 1900);
}

/* Sau mat dien: gio khoi phuc tu NVS >= 2020 nhung KHONG phai gio NTP that.
 * Dung de chan cham cong "sai gio" cho den khi SNTP that su cap nhat. */
static bool    s_wall_from_nvs = false;
static int64_t s_wall_nvs_epoch = 0;      /* UTC khoi phuc tu NVS */
static int64_t s_wall_nvs_uptime_us = 0;  /* esp_timer luc khoi phuc */
/* Nguong (giay) coi la "dong ho da nhay" -> SNTP that da hieu chinh. */
#define WALL_NTP_JUMP_MIN_SEC 5

/** Luu UTC lan NTP cuoi — sau reset khoi phuc tam cho den khi SNTP cap nhat lai. */
static void wall_time_nvs_save(time_t utc)
{
    if (!wall_time_valid(utc)) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i64(h, NVS_KEY_WALL_UTC, (int64_t)utc);
    nvs_commit(h);
    nvs_close(h);
}

/** Sau mat nguon: dat time() tu NVS (co the lech bang thoi gian tat may); SNTP se hieu chinh. */
static void wall_time_nvs_restore(void)
{
    if (wall_time_valid(time(NULL))) {
        return;
    }
    nvs_handle_t h;
    int64_t saved = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    esp_err_t e = nvs_get_i64(h, NVS_KEY_WALL_UTC, &saved);
    nvs_close(h);
    if (e != ESP_OK || !wall_time_valid((time_t)saved)) {
        return;
    }
    struct timeval tv = {.tv_sec = (time_t)saved, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "Khoi phuc gio NVS that bai");
        return;
    }
    /* Danh dau: gio nay tu NVS (tam, hien thi) — chua duoc phep cham cong. */
    s_wall_from_nvs = true;
    s_wall_nvs_epoch = saved;
    s_wall_nvs_uptime_us = esp_timer_get_time();
    struct tm t;
    scan_log_wall_tm((time_t)saved, &t);
    ESP_LOGI(TAG, "Khoi phuc gio NVS (tam, cho NTP): %04d-%02d-%02d %02d:%02d:%02d",
             (int)(t.tm_year + 1900), (int)(t.tm_mon + 1), (int)t.tm_mday, (int)t.tm_hour, (int)t.tm_min,
             (int)t.tm_sec);
}

bool wifi_portal_time_is_valid(void)
{
    if (!s_time_synced) {
        sntp_adopt_wall_time_if_valid();
    }
    return s_time_synced;
}

/** UTC cho backend — chi sau NTP that (khong dung gio NVS tam). */
time_t wifi_portal_get_utc_sec(void)
{
    if (!s_time_synced) {
        return 0;
    }
    time_t now = time(NULL);
    if (!wall_time_valid(now)) {
        return 0;
    }
    return now;
}

static esp_err_t sntp_pick_server_api(void *arg)
{
    (void)arg;
    const char *srv = s_ntp_servers[s_ntp_server_idx % (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]))];
    esp_sntp_setservername(0, srv);
    return ESP_OK;
}

static void sntp_rotate_server(void)
{
    s_ntp_server_idx = (uint8_t)((s_ntp_server_idx + 1u) % (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0])));
    (void)esp_netif_tcpip_exec(sntp_pick_server_api, NULL);
}

/** NTP da cap nhat time() nhung callback cham — danh dau synced de UI/Azure khong treo. */
static void sntp_adopt_wall_time_if_valid(void)
{
    if (s_time_synced) {
        return;
    }
    time_t now = time(NULL);
    if (!wall_time_valid(now)) {
        return;
    }
    /* Sau mat dien, time() la gio khoi phuc tu NVS (chua chinh xac).
     * Chi chap nhan khi phat hien SNTP that su da hieu chinh: time() nhay
     * khoi quy dao du doan (NVS_epoch + uptime). Neu chua nhay -> cho callback NTP,
     * KHONG cham cong bang gio NVS de tranh sai gio. */
    if (s_wall_from_nvs) {
        int64_t elapsed_s = (esp_timer_get_time() - s_wall_nvs_uptime_us) / 1000000;
        int64_t predicted = s_wall_nvs_epoch + elapsed_s;
        int64_t diff = (int64_t)now - predicted;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff < WALL_NTP_JUMP_MIN_SEC) {
            return;
        }
        ESP_LOGI(TAG, "SNTP hieu chinh gio NVS (nhay %lld s)", (long long)diff);
    }
    s_time_synced = true;
    struct tm t;
    scan_log_wall_tm(now, &t);
    wall_time_nvs_save(now);
    ESP_LOGI(TAG, "NTP ok (poll), Real time: %04d-%02d-%02d %02d:%02d:%02d",
             (int)(t.tm_year + 1900), (int)(t.tm_mon + 1), (int)t.tm_mday,
             (int)t.tm_hour, (int)t.tm_min, (int)t.tm_sec);
    app_azure_notify_sntp_synced();
}

static void sntp_start_or_restart(void)
{
    if (!s_sntp_started) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS >= 3
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3, ESP_SNTP_SERVER_LIST(
            "time.google.com", "pool.ntp.org", "vn.pool.ntp.org"));
#else
        const char *srv = s_ntp_servers[s_ntp_server_idx % (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]))];
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(srv);
#endif
        cfg.server_from_dhcp = true;
        cfg.renew_servers_after_new_IP = true;
        cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
        cfg.sync_cb = time_synced_cb;
        cfg.start = true;
        esp_err_t e = esp_netif_sntp_init(&cfg);
        if (e == ESP_OK) {
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP init (DHCP+NTP, %u may chu tinh)",
                     (unsigned)(sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0])));
        } else {
            ESP_LOGW(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(e));
        }
        return;
    }
#if CONFIG_LWIP_SNTP_MAX_SERVERS < 3
    sntp_rotate_server();
#endif
    esp_err_t e = esp_netif_sntp_start();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_start: %s", esp_err_to_name(e));
    } else {
#if CONFIG_LWIP_SNTP_MAX_SERVERS < 3
        ESP_LOGI(TAG, "SNTP retry: %s",
                 s_ntp_servers[s_ntp_server_idx % (sizeof(s_ntp_servers) / sizeof(s_ntp_servers[0]))]);
#else
        ESP_LOGI(TAG, "SNTP retry (restart)");
#endif
    }
}

/**
 * Co WiFi + IP thi thu SNTP lien tuc (khong gioi han 20 lan) cho den khi dong bo.
 * Thoat khi mat WiFi; GOT_IP se tao task moi.
 */
static void sntp_retry_task(void *arg)
{
    (void)arg;
    int attempt = 0;

    /* Cho DNS/router on dinh sau GOT_IP */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (!s_time_synced && s_wifi_conn_status == WIFI_STATUS_CONNECTED) {
        sntp_adopt_wall_time_if_valid();
        if (s_time_synced) {
            break;
        }

        if (attempt > 0) {
            const TickType_t dly = (attempt <= 4) ? pdMS_TO_TICKS(5000) : pdMS_TO_TICKS(10000);
            vTaskDelay(dly);
            if (s_time_synced || s_wifi_conn_status != WIFI_STATUS_CONNECTED) {
                break;
            }
            sntp_adopt_wall_time_if_valid();
            if (s_time_synced) {
                break;
            }
        }

        ESP_LOGW(TAG, "SNTP chua dong bo, thu lai (lan %d)...", attempt + 1);
        sntp_start_or_restart();
        attempt++;
    }

    s_sntp_retry_task_live = false;
    vTaskDelete(NULL);
}

static void schedule_sntp_retry(void)
{
    if (s_time_synced || s_sntp_retry_task_live) {
        return;
    }
    s_sntp_retry_task_live = true;
    if (xTaskCreate(sntp_retry_task, "sntp_retry", 3072, NULL, 3, NULL) != pdPASS) {
        s_sntp_retry_task_live = false;
        ESP_LOGW(TAG, "Khong tao duoc task sntp_retry");
    }
}

/** Sau khi NTP cap nhat — dung scan_log_wall_tm, khong localtime_r (tranh getenv tren task SNTP). */
static void time_synced_cb(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    if (!wall_time_valid(now)) {
        ESP_LOGW(TAG, "SNTP callback nhung time() chua hop le (%lld)", (long long)now);
        return;
    }
    s_time_synced = true;
    struct tm t;
    scan_log_wall_tm(now, &t);
    wall_time_nvs_save(now);
    ESP_LOGI(TAG, "NTP ok, Real time: %04d-%02d-%02d %02d:%02d:%02d",
             (int)(t.tm_year + 1900), (int)(t.tm_mon + 1), (int)t.tm_mday,
             (int)t.tm_hour, (int)t.tm_min, (int)t.tm_sec);
    app_azure_notify_sntp_synced();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *ev)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        wifi_scan_cache_fill();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)ev;
        int reason = d ? (int)d->reason : -1;
        ESP_LOGW(TAG, "STA mat ket noi, reason=%d", reason);
        s_wifi_conn_status = WIFI_STATUS_FAIL;
        schedule_sta_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_reconnect_count = 0;
        sta_current_idx_save();
        s_wifi_conn_status = WIFI_STATUS_CONNECTED;
        if (s_pending_ssid[0]) {
            wifi_list_add(s_pending_ssid, s_pending_pass);
            s_pending_ssid[0] = '\0';
        }
        ESP_LOGI(TAG, "STA da co IP");
        /* AP+STA: route NTP/Internet qua STA, khong qua softAP 192.168.4.x */
        if (s_sta_netif) {
            esp_netif_set_default_netif(s_sta_netif);
        }
        if (!s_time_synced) {
            sntp_start_or_restart();
            schedule_sntp_retry();
        }
    }
}

esp_err_t wifi_portal_start(void)
{
    app_login_pin_init();
    ESP_ERROR_CHECK(esp_netif_init());
    /*
     * Viet Nam UTC+7: tren newlib ESP dung "UTC-7" (giong test IDF: "UTC-8" = UTC+8).
     * "CST-7" khong tuong thich, de gio sai.
     */
    setenv("TZ", "UTC-7", 1);
    tzset();
    wall_time_nvs_restore();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&icfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t ap = {0};
    copy_field((char *)ap.ap.ssid, sizeof(ap.ap.ssid), AP_SSID);
    ap.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    copy_field((char *)ap.ap.password, sizeof(ap.ap.password), AP_PASS);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    wifi_list_load();
    if (s_wifi_list.count == 0) {
        wifi_cred_t cred;
        memset(&cred, 0, sizeof(cred));
        if (cred_load(&cred) == ESP_OK && cred.ssid[0] != '\0') {
            wifi_list_add(cred.ssid, cred.pass);
        }
    }

    s_sta_auto_reconnect = false;
    s_sta_reconnect_count = 0;

    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_wifi_list.count > 0) {
        sta_current_idx_load();
        ESP_LOGI(TAG, "STA nhanh: bo scan luc boot, AP %u/%u", (unsigned)(s_current_wifi_idx + 1u),
                 (unsigned)s_wifi_list.count);
        sta_apply_current_wifi();
    }

    esp_err_t ret = start_httpd();
    if (ret == ESP_OK) {
        portal_health_start();
    }
    return ret;
}

/* ----------------------------------------------------------------
 * Brand text (chữ thương hiệu trên màn idle) — NVS key "brand_txt"
 * ---------------------------------------------------------------- */
#define BRAND_NVS_KEY "brand_txt"
#define BRAND_DEFAULT "MEBISOFT"
#define BRAND_MAX_LEN 15  /* không kể '\0' */

void wifi_portal_get_brand_text(char *out, size_t sz)
{
    if (!out || sz == 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t bl = sz;
        esp_err_t e = nvs_get_str(h, BRAND_NVS_KEY, out, &bl);
        nvs_close(h);
        if (e == ESP_OK && out[0] != '\0') return;
    }
    /* Fallback */
    snprintf(out, sz, "%s", BRAND_DEFAULT);
}

esp_err_t wifi_portal_set_brand_text(const char *txt)
{
    if (!txt) return ESP_ERR_INVALID_ARG;
    /* Cắt tối đa BRAND_MAX_LEN ký tự */
    char tmp[BRAND_MAX_LEN + 1];
    snprintf(tmp, sizeof(tmp), "%s", txt);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, BRAND_NVS_KEY, tmp);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
