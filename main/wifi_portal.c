#include "wifi_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "mbedtls/base64.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "scan_log.h"
#include "app_azure.h"

static const char *TAG = "wifi_portal";

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

void wifi_list_remove(const char *ssid)
{
    int found = -1;
    for (int i = 0; i < s_wifi_list.count; i++) {
        if (strcmp(s_wifi_list.wifis[i].ssid, ssid) == 0) {
            found = i; break;
        }
    }
    if (found >= 0) {
        for (int i = found; i < s_wifi_list.count - 1; i++) s_wifi_list.wifis[i] = s_wifi_list.wifis[i+1];
        s_wifi_list.count--;
        wifi_list_save();
    }
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
#define AP_SSID "ESP32-Config"
#define AP_PASS "12345678"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4

/** HTTP Basic Auth cho trang cấu hình (khác mật khẩu WiFi AP; cùng giá trị nếu muốn). */
#define PORTAL_WEB_USER "admin"
#define PORTAL_WEB_PASS "12345678"

static httpd_handle_t s_server;
static bool s_sntp_started;

/** true: co NVS WiFi — cho phep tu ket noi lai sau khi mat STA */
static bool s_sta_auto_reconnect;
/** So lan da tu goi lai connect sau disconnect (reset khi co IP) */
static uint8_t s_sta_reconnect_count;
static volatile bool s_sta_reconnect_task_live;

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

static esp_err_t send_401_basic(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-Config\"");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "Unauthorized");
}

/** Authorization: Basic base64(user:password) */
static bool portal_auth_ok(httpd_req_t *req)
{
    char buf[192];
    if (httpd_req_get_hdr_value_len(req, "Authorization") == 0) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(buf, "Basic ", 6) != 0) {
        return false;
    }
    const char *b64 = buf + 6;
    while (*b64 == ' ' || *b64 == '\t') {
        b64++;
    }
    unsigned char dec[96];
    size_t olen = 0;
    int ret = mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen, (const unsigned char *)b64, strlen(b64));
    if (ret != 0) {
        return false;
    }
    dec[olen] = '\0';

    char expect[80];
    snprintf(expect, sizeof(expect), "%s:%s", PORTAL_WEB_USER, PORTAL_WEB_PASS);
    return strcmp((char *)dec, expect) == 0;
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
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    esp_wifi_disconnect();
    esp_wifi_connect();
}



static void sta_clear_config(void)
{
    s_sta_auto_reconnect = false;
    wifi_config_t w;
    memset(&w, 0, sizeof(w));
    esp_wifi_set_config(WIFI_IF_STA, &w);
    esp_wifi_disconnect();
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
    char prefix[40];
    snprintf(prefix, sizeof(prefix), "%s=", key);
    const char *p = strstr(body, prefix);
    if (!p) {
        return -1;
    }
    p += strlen(prefix);
    size_t i = 0;
    while (i < out_len - 1 && *p && *p != '&') {
        out[i++] = *p++;
    }
    out[i] = '\0';
    url_decode_inplace(out);
    return 0;
}

static const char HTML_PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Cấu hình Thiết Bị</title>"
    "<style>"
    "body{font-family:'Segoe UI',Tahoma,sans-serif;background:linear-gradient(135deg,#f5f7fa 0%,#c3cfe2 100%);"
    "min-height:100vh;margin:0;display:flex;align-items:center;justify-content:center;padding:20px;box-sizing:border-box}"
    ".card{background:#fff;padding:25px;border-radius:12px;box-shadow:0 8px 24px rgba(0,0,0,0.1);width:100%;max-width:400px}"
    "h1{text-align:center;color:#333;margin-top:0;font-size:24px;border-bottom:1px solid #eee;padding-bottom:15px}"
    ".form-group{margin-bottom:15px}"
    "label{display:block;margin-bottom:5px;font-weight:600;color:#555;font-size:14px}"
    "input{width:100%;padding:10px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:14px;transition:border 0.3s}"
    "input:focus{border-color:#007bff;outline:none}"
    ".scan-btn{width:100%;background:#e2e6ea;color:#333;border:1px solid #dae0e5;padding:8px;border-radius:6px;margin-bottom:10px;cursor:pointer;margin-top:5px;font-weight:600}"
    ".scan-btn:hover{background:#d3d9df}"
    ".btn{width:100%;padding:12px;background:#007bff;color:#fff;border:none;border-radius:6px;font-size:16px;cursor:pointer;font-weight:bold;margin-top:5px;flex:1}"
    ".btn:hover{background:#0056b3}"
    ".footer{margin-top:20px;text-align:center;font-size:13px;color:#777}"
    ".msg{margin-top:5px;font-size:13px;font-weight:bold;text-align:center;min-height:18px}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h1> Máy Công Chứng Đi Làm </h1>"
    
    "<form id=\"frmWifi\">"
    "<h3 style=\"color:#007bff;margin-bottom:10px\">1. Mạng WiFi</h3>"
    "<div class=\"form-group\">"
    "<label>WiFi SSID</label>"
    "<input list=\"ssid-list\" id=\"ssid\" name=\"ssid\" maxlength=\"31\" required>"
    "<datalist id=\"ssid-list\"></datalist>"
    "<button type=\"button\" class=\"scan-btn\" id=\"scanBtn\" onclick=\"scanWifi()\">🔄 Quét WiFi Lân Cận</button>"
    "</div>"
    "<div class=\"form-group\"><label>Mật khẩu WiFi</label><input type=\"password\" name=\"pass\" maxlength=\"63\"></div>"
    "<div id=\"resWifi\" class=\"msg\"></div>"
    "<div style=\"display:flex;gap:10px;margin-top:5px\">"
    "<button type=\"button\" class=\"btn\" onclick=\"sendForm('frmWifi','/save_wifi','resWifi')\">💾 Lưu</button>"
    "<button type=\"button\" class=\"btn\" style=\"background:#dc3545\" onclick=\"sendForm('frmWifi','/clear','resWifi')\">❌ Clear</button>"
    "</div>"
    "</form>"
    
    "<hr style=\"border:0;border-top:1px solid #eee;margin:25px 0\">"
    
    "<form id=\"frmAzure\">"
    "<h3 style=\"color:#28a745;margin-bottom:10px\">2. Đám Mây Azure IoT</h3>"
    "<div class=\"form-group\"><label>HostName</label><input name=\"azure_host\" placeholder=\"vd: abc.azure-devices.net\" maxlength=\"63\"></div>"
    "<div class=\"form-group\"><label>Device ID</label><input name=\"azure_devid\" placeholder=\"my-esp32\" maxlength=\"31\"></div>"
    "<div class=\"form-group\"><label>Shared Access Key</label><input type=\"password\" name=\"azure_sas_key\" placeholder=\"Nhập Key\" maxlength=\"127\"></div>"
    "<div id=\"resAzure\" class=\"msg\"></div>"
    "<div style=\"display:flex;gap:10px;margin-top:5px\">"
    "<button type=\"button\" class=\"btn\" style=\"background:#28a745\" onclick=\"sendForm('frmAzure','/save_azure','resAzure')\">☁️ Tích hợp Azure</button>"
    "<button type=\"button\" class=\"btn\" style=\"background:#dc3545\" onclick=\"sendForm('frmAzure','/clear_azure','resAzure')\">❌ Xóa Azure</button>"
    "</div>"
    "</form>"
    
    "<div class=\"footer\"><a href=\"/scans\" style=\"color:#6c757d;text-decoration:none\">📋 Nhật ký Thẻ (SD Card)</a></div>"
    "</div>"
    "<script>"
    "function scanWifi(){"
    "let b=document.getElementById('scanBtn');let l=document.getElementById('ssid-list');"
    "b.innerText='⏳ Đang quét...';b.disabled=true;"
    "fetch('/scan').then(r=>r.json()).then(d=>{"
    "l.innerHTML='';d.forEach(i=>{"
    "let o=document.createElement('option');o.value=i.ssid;l.appendChild(o);"
    "});b.innerText='✅ Chọn WiFi từ danh sách sổ xuống';b.disabled=false;"
    "}).catch(e=>{b.innerText='❌ Lỗi quét mạng';b.disabled=false;});"
    "}"
    "function sendForm(fid, u, rid){"
    "let f=document.getElementById(fid); let r=document.getElementById(rid); let btns=f.querySelectorAll('button');"
    "btns.forEach(b=>b.disabled=true); r.innerText='⏳ Đang xử lý...'; r.style.color='#007bff';"
    "let params=new URLSearchParams(new FormData(f));"
    "if(u=='/clear') params=new URLSearchParams();"
    "fetch(u, {method:'POST',body:params})"
    ".then(x=>{ if(x.ok){r.innerText='✅ Cập nhật thành công';r.style.color='#28a745';}else{r.innerText='❌ Lỗi hệ thống';r.style.color='#dc3545'} btns.forEach(b=>b.disabled=false); })"
    ".catch(e=>{ r.innerText='❌ Mất kết nối'; r.style.color='#dc3545'; btns.forEach(b=>b.disabled=false); });"
    "}"
    "</script>"
    "</body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) {
        return send_401_basic(req);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_wifi_post_handler(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return send_401_basic(req);

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
    if (!portal_auth_ok(req)) return send_401_basic(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    char buf[128];
    for (int i = 0; i < s_wifi_list.count; i++) {
        snprintf(buf, sizeof(buf), "\"%s\"%s", s_wifi_list.wifis[i].ssid, (i < s_wifi_list.count - 1) ? "," : "");
        httpd_resp_send_chunk(req, buf, strlen(buf));
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t del_wifi_post_handler(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return send_401_basic(req);
    char buf[128];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) return ESP_FAIL;
    buf[rlen] = '\0';
    
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
    if (!portal_auth_ok(req)) return send_401_basic(req);

    char buf[384];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) return ESP_FAIL;
    buf[rlen] = '\0';

    char host[64] = {0}, devid[32] = {0}, sas[128] = {0};
    (void)form_get(buf, "azure_host", host, sizeof(host));
    (void)form_get(buf, "azure_devid", devid, sizeof(devid));
    (void)form_get(buf, "azure_sas_key", sas, sizeof(sas));

    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    cred_load(&c); // Giữ info WiFi cũ
    c.magic = CRED_MAGIC;
    copy_field(c.azure_host, sizeof(c.azure_host), host);
    copy_field(c.azure_dev, sizeof(c.azure_dev), devid);
    copy_field(c.azure_sas, sizeof(c.azure_sas), sas);
    cred_save(&c);

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t clear_azure_post_handler(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) return send_401_basic(req);
    
    wifi_cred_t c;
    memset(&c, 0, sizeof(c));
    cred_load(&c);
    c.magic = CRED_MAGIC;
    memset(c.azure_host, 0, sizeof(c.azure_host));
    memset(c.azure_dev, 0, sizeof(c.azure_dev));
    memset(c.azure_sas, 0, sizeof(c.azure_sas));
    cred_save(&c);
    
    ESP_LOGI(TAG, "Da xoa cau hinh Azure khoi NVS");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t clear_post_handler(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) {
        return send_401_basic(req);
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
    if (!portal_auth_ok(req)) {
        return send_401_basic(req);
    }
    return scan_log_send_html_page(req);
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    if (!portal_auth_ok(req)) {
        return send_401_basic(req);
    }
    wifi_scan_config_t scan_config = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false
    };
    esp_err_t scan_err = esp_wifi_scan_start(&scan_config, true);
    if(scan_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }
    
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_info == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_info);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    char buf[128];
    for (int i = 0; i < ap_count; i++) {
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d}%s", (char *)ap_info[i].ssid, ap_info[i].rssi, (i < ap_count - 1) ? "," : "");
        httpd_resp_send_chunk(req, buf, strlen(buf));
    }
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    
    free(ap_info);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Gioi han URI/header: ESP-IDF 5.3 chi co CONFIG_HTTPD_MAX_* trong sdkconfig (khong co field trong httpd_config_t). */
    /* /scans dung nhieu buffer tren stack; mac dinh 4096 de tran stack overflow -> reset. */
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start");
        return ESP_FAIL;
    }

    httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
    httpd_uri_t u_save_w = { .uri = "/save_wifi", .method = HTTP_POST, .handler = save_wifi_post_handler, .user_ctx = NULL };
    httpd_uri_t u_save_a = { .uri = "/save_azure", .method = HTTP_POST, .handler = save_azure_post_handler, .user_ctx = NULL };
    httpd_uri_t u_clear_a = { .uri = "/clear_azure", .method = HTTP_POST, .handler = clear_azure_post_handler, .user_ctx = NULL };
    httpd_uri_t u_clear = { .uri = "/clear", .method = HTTP_POST, .handler = clear_post_handler, .user_ctx = NULL };
    httpd_uri_t u_scans = { .uri = "/scans", .method = HTTP_GET, .handler = scans_get_handler, .user_ctx = NULL };
    httpd_uri_t u_scanwifi = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
    httpd_uri_t u_saved_wifis = { .uri = "/saved_wifis", .method = HTTP_GET, .handler = saved_wifis_get_handler, .user_ctx = NULL };
    httpd_uri_t u_del_wifi = { .uri = "/del_wifi", .method = HTTP_POST, .handler = del_wifi_post_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_server, &u_root);
    httpd_register_uri_handler(s_server, &u_save_w);
    httpd_register_uri_handler(s_server, &u_save_a);
    httpd_register_uri_handler(s_server, &u_clear_a);
    httpd_register_uri_handler(s_server, &u_clear);
    httpd_register_uri_handler(s_server, &u_scans);
    httpd_register_uri_handler(s_server, &u_scanwifi);
    httpd_register_uri_handler(s_server, &u_saved_wifis);
    httpd_register_uri_handler(s_server, &u_del_wifi);
    ESP_LOGI(TAG, "Web: http://192.168.4.1/ va /scans (Basic auth user=%s)", PORTAL_WEB_USER);
    return ESP_OK;
}

/** Sau khi NTP cap nhat — dung scan_log_wall_tm, khong localtime_r (tranh getenv tren task SNTP). */
static void time_synced_cb(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    struct tm t;
    scan_log_wall_tm(now, &t);
    ESP_LOGI(TAG, "NTP ok, Real time: %04d-%02d-%02d %02d:%02d:%02d",
             (int)(t.tm_year + 1900), (int)(t.tm_mon + 1), (int)t.tm_mday,
             (int)t.tm_hour, (int)t.tm_min, (int)t.tm_sec);
    app_azure_notify_sntp_synced();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *ev)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)ev;
        int reason = d ? (int)d->reason : -1;
        ESP_LOGW(TAG, "STA mat ket noi, reason=%d", reason);
        schedule_sta_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_reconnect_count = 0;
        sta_current_idx_save();
        ESP_LOGI(TAG, "STA da co IP");
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("vn.pool.ntp.org");
            cfg.sync_cb = time_synced_cb;
            esp_err_t e = esp_netif_sntp_init(&cfg);
            if (e == ESP_OK) {
                s_sntp_started = true;
                ESP_LOGI(TAG, "SNTP: vn.pool.ntp.org + DHCP");
            } else {
                ESP_LOGW(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(e));
            }
        }
    }
}

esp_err_t wifi_portal_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    /*
     * Viet Nam UTC+7: tren newlib ESP dung "UTC-7" (giong test IDF: "UTC-8" = UTC+8).
     * "CST-7" khong tuong thich, de gio sai.
     */
    setenv("TZ", "UTC-7", 1);
    tzset();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

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

    return start_httpd();
}
