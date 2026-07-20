/**
 * Web quan tri: menu trai, nhap PIN theo tung muc (dung app_login_verify_pin).
 */
#include "portal_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"

#include "app_rfid.h"
#include "board_pins.h"
#include "card_profile.h"
#include "lcd_ui.h"
#include "app_azure.h"
#include "scan_log.h"
#include "sd_card.h"
#include "wifi_portal.h"
#include "lcd_panel_config.h"

#include "esp_system.h"
#include "app_audio.h"
#include "lv_port.h"

/* 1x1 PNG trong (tranh 404 favicon tren trinh duyet) */
static const unsigned char s_favicon_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82,
};
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "portal_web";

#define PORTAL_SESS_MAX 6
#define PORTAL_TOKEN_LEN 33
#define PORTAL_SESS_TTL_SEC 1800

typedef struct {
    bool active;
    char token[PORTAL_TOKEN_LEN];
    char section[16];
    int64_t expiry;
} portal_sess_t;

static portal_sess_t s_sess[PORTAL_SESS_MAX];

/* Bien giam sat khoa bao mat */
static int s_failed_login_attempts = 0;
static int64_t s_login_lockout_until_sec = 0;

static int s_failed_pin_attempts = 0;
static int64_t s_pin_lockout_until_sec = 0;

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

/** Nhan body POST ngan; tranh ESP_FAIL khi client dong socket (recv errno 104). */
int portal_recv_small_body(httpd_req_t *req, char *buf, size_t bufsz)
{
    if (bufsz == 0) {
        return -1;
    }
    size_t total = 0;
    int want = (int)((req->content_len > 0 && (size_t)req->content_len < bufsz - 1) ? req->content_len
                                                                                    : bufsz - 1);
    while (total < (size_t)want) {
        int r = httpd_req_recv(req, buf + total, want - (int)total);
        if (r > 0) {
            total += (size_t)r;
            if (req->content_len > 0 && total >= (size_t)req->content_len) {
                break;
            }
            continue;
        }
        if (r == 0) {
            break;
        }
        /* r < 0: timeout hoac ECONNRESET — neu da co du lieu thi dung */
        if (total > 0) {
            break;
        }
        return -1;
    }
    buf[total] = '\0';
    return (int)total;
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
    char tmp[320];
    size_t i = 0;
    while (i < sizeof(tmp) - 1 && *p && *p != '&') {
        tmp[i++] = *p++;
    }
    tmp[i] = '\0';
    url_decode_inplace(tmp);
    snprintf(out, out_len, "%s", tmp);
    return 0;
}

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in) {
        in = "";
    }
    for (size_t i = 0; in[i] && j + 2 < outsz; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if ((unsigned char)c >= 0x20) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static void gen_token(char *tok, size_t sz)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i + 1 < sz; i++) {
        tok[i] = hex[esp_random() % 16];
    }
    tok[sz - 1] = '\0';
}

static void sess_purge_expired(void)
{
    int64_t now = esp_timer_get_time() / 1000000;
    for (int i = 0; i < PORTAL_SESS_MAX; i++) {
        if (s_sess[i].active && s_sess[i].expiry <= now) {
            s_sess[i].active = false;
        }
    }
}

static const char *get_req_token(httpd_req_t *req, char *buf, size_t bufsz)
{
    if (httpd_req_get_hdr_value_str(req, "X-Portal-Token", buf, bufsz) == ESP_OK && buf[0]) {
        return buf;
    }
    char qry[96];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        if (httpd_query_key_value(qry, "token", buf, bufsz) == ESP_OK && buf[0]) {
            return buf;
        }
    }
    return NULL;
}

bool portal_auth_section(httpd_req_t *req, const char *section)
{
    if (!section || !section[0]) {
        return false;
    }
    char tokbuf[PORTAL_TOKEN_LEN];
    const char *tok = get_req_token(req, tokbuf, sizeof(tokbuf));
    if (!tok) {
        return false;
    }
    sess_purge_expired();
    int64_t now = esp_timer_get_time() / 1000000;
    for (int i = 0; i < PORTAL_SESS_MAX; i++) {
        if (s_sess[i].active && strcmp(s_sess[i].token, tok) == 0 && s_sess[i].expiry > now) {
            if (strcmp(s_sess[i].section, section) == 0) {
                s_sess[i].expiry = now + PORTAL_SESS_TTL_SEC;
                return true;
            }
        }
    }
    return false;
}

static bool sess_create(const char *section, char *tok_out, size_t tok_sz)
{
    sess_purge_expired();
    int slot = -1;
    for (int i = 0; i < PORTAL_SESS_MAX; i++) {
        if (!s_sess[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    gen_token(tok_out, tok_sz);
    s_sess[slot].active = true;
    strncpy(s_sess[slot].token, tok_out, PORTAL_TOKEN_LEN - 1);
    strncpy(s_sess[slot].section, section, sizeof(s_sess[slot].section) - 1);
    s_sess[slot].expiry = (esp_timer_get_time() / 1000000) + PORTAL_SESS_TTL_SEC;
    return true;
}esp_err_t portal_admin_login_post_handler(httpd_req_t *req)
{
    int64_t now_sec = esp_timer_get_time() / 1000000;
    if (s_login_lockout_until_sec > 0 && now_sec < s_login_lockout_until_sec) {
        int64_t remain = s_login_lockout_until_sec - now_sec;
        char resp[160];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"Khoa dang nhap. Thu lai sau %lld giay.\"}", (long long)remain);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, resp);
    }

    char buf[256];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (rlen <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Khong nhan du lieu dang nhap\"}");
    }

    char user[64] = {0}, pass[64] = {0};
    if (form_get(buf, "user", user, sizeof(user)) != 0 || form_get(buf, "pass", pass, sizeof(pass)) != 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Thieu tai khoan hoac mat khau\"}");
    }
    //ESP_LOGI(TAG, "Login attempt: user='%s', pass='%s'", user, pass);
    if (strcmp(user, "mebieco") != 0 || strcmp(pass, "68686868@") != 0) {
        s_failed_login_attempts++;
        int64_t current_now = esp_timer_get_time() / 1000000;
        char error_msg[160];
        if (s_failed_login_attempts >= 5) {
            s_login_lockout_until_sec = current_now + 300;
            snprintf(error_msg, sizeof(error_msg), "Thu sai %d lan. Khoa dang nhap 5 phut.", s_failed_login_attempts);
        } else {
            snprintf(error_msg, sizeof(error_msg), "Tai khoan hoac mat khau sai (Con lai %d lan thu)", 5 - s_failed_login_attempts);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        char resp[200];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", error_msg);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, resp);
    }

    s_failed_login_attempts = 0;
    s_login_lockout_until_sec = 0;

    char tok[PORTAL_TOKEN_LEN];
    if (!sess_create("admin", tok, sizeof(tok))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Session");
        return ESP_OK;
    }
    char resp[120];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"token\":\"%s\"}", tok);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

esp_err_t portal_unlock_post_handler(httpd_req_t *req)
{
    int64_t now_sec = esp_timer_get_time() / 1000000;
    if (s_pin_lockout_until_sec > 0 && now_sec < s_pin_lockout_until_sec) {
        int64_t remain = s_pin_lockout_until_sec - now_sec;
        char resp[160];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"Ma PIN bi khoa. Thu lai sau %lld giay.\"}", (long long)remain);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, resp);
    }

    char buf[256];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    buf[rlen] = '\0';

    char section[20] = {0};
    char pin[20] = {0};
    if (form_get(buf, "section", section, sizeof(section)) != 0 || section[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Thieu section");
        return ESP_OK;
    }
    if (form_get(buf, "pin", pin, sizeof(pin)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Thieu pin");
        return ESP_OK;
    }

    if (!app_login_verify_pin(pin)) {
        s_failed_pin_attempts++;
        int64_t current_now = esp_timer_get_time() / 1000000;
        char error_msg[160];
        if (s_failed_pin_attempts >= 5) {
            s_pin_lockout_until_sec = current_now + 300;
            snprintf(error_msg, sizeof(error_msg), "Nhap sai PIN %d lan. Khoa PIN 5 phut.", s_failed_pin_attempts);
        } else {
            snprintf(error_msg, sizeof(error_msg), "Ma PIN khong dung (Con lai %d lan thu)", 5 - s_failed_pin_attempts);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        char resp[200];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", error_msg);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, resp);
    }

    s_failed_pin_attempts = 0;
    s_pin_lockout_until_sec = 0;

    char tok[PORTAL_TOKEN_LEN];
    if (!sess_create(section, tok, sizeof(tok))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Session");
        return ESP_OK;
    }

    char resp[120];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"token\":\"%s\",\"section\":\"%s\"}", tok, section);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_cards_get_handler(httpd_req_t *req)
{
    const char *sec = "cards";
    int page = 1;
    char qry[64];
    char id_q[48] = {0};
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        char v[48];
        if (httpd_query_key_value(qry, "unreg", v, sizeof(v)) == ESP_OK && v[0] == '1') {
            sec = "register";
        }
        if (httpd_query_key_value(qry, "page", v, sizeof(v)) == ESP_OK) {
            int p = atoi(v);
            if (p > 0) {
                page = p;
            }
        }
        if (httpd_query_key_value(qry, "id", v, sizeof(v)) == ESP_OK && v[0]) {
            snprintf(id_q, sizeof(id_q), "%s", v);
        }
    }
    if (!portal_auth_section(req, sec)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Can PIN muc nay\"}");
    }

    bool only_u = (strcmp(sec, "register") == 0);
    const char *id_filter = (!only_u && id_q[0]) ? id_q : NULL;
    int total_cards = card_profile_count_matched(only_u, id_filter);
    int limit = CARD_PROFILE_LIST_MAX;
    int skip_first = (page - 1) * limit;

    CardProfileEntry_t *entries = malloc(limit * sizeof(CardProfileEntry_t));
    if (!entries) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
    }
    int n = card_profile_list_page(entries, limit, only_u, skip_first, id_filter);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "{\"ok\":true,\"total\":%d,\"page\":%d,\"limit\":%d,\"cards\":[", total_cards, page, limit);
    esp_err_t e = httpd_resp_send_chunk(req, hdr, HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) {
        free(entries);
        return e;
    }
    for (int i = 0; i < n; i++) {
        char eu[48], en[96], ei[96], ed[48];
        json_escape(entries[i].uid, eu, sizeof(eu));
        json_escape(entries[i].name, en, sizeof(en));
        json_escape(entries[i].id, ei, sizeof(ei));
        json_escape(entries[i].date, ed, sizeof(ed));
        char row[380];
        int row_len = snprintf(row, sizeof(row), "%s{\"uid\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"date\":\"%s\",\"registered\":%s}",
                 i ? "," : "", eu, en, ei, ed, entries[i].registered ? "true" : "false");
        if (row_len < 0 || (size_t)row_len >= sizeof(row)) {
            continue;
        }
        e = httpd_resp_send_chunk(req, row, (size_t)row_len);
        if (e != ESP_OK) {
            free(entries);
            return e;
        }
    }
    free(entries);
    e = httpd_resp_send_chunk(req, "]}", 2);
    if (e != ESP_OK) {
        return e;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_cards_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "cards") && !portal_auth_section(req, "register")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char buf[320];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) {
        return ESP_FAIL;
    }
    buf[rlen] = '\0';
    char uid[24] = {0}, name[48] = {0}, id[48] = {0};
    if (form_get(buf, "uid", uid, sizeof(uid)) != 0 || uid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Thieu uid");
        return ESP_OK;
    }
    (void)form_get(buf, "name", name, sizeof(name));
    (void)form_get(buf, "id", id, sizeof(id));

    int n_len = 0;
    for (int i = 0; name[i]; i++) {
        if (name[i] != ' ' && name[i] != '\t' && name[i] != '\r' && name[i] != '\n') {
            n_len++;
        }
    }
    int i_len = 0;
    for (int i = 0; id[i]; i++) {
        if (id[i] != ' ' && id[i] != '\t' && id[i] != '\r' && id[i] != '\n') {
            i_len++;
        }
    }
    if (n_len == 0 || i_len == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Vui long nhap day du ten va ma nhan vien\"}");
    }

    esp_err_t err = card_profile_save(uid, name, id);
    if (err == ESP_OK) {
        int32_t msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
#if BOARD_ENABLE_AZURE
        app_azure_send_card_event(uid, name, id, 603, msg_idx);
#endif
        lcd_ui_invalidate_card_cache();
        scan_log_append_admin(uid, name, id, "SAVE", msg_idx);
        app_audio_play_confirm();
    }
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Luu that bai\"}");
}

static esp_err_t api_cards_del_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "cards") && !portal_auth_section(req, "register")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char buf[128];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (rlen <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Khong nhan du lieu\"}");
    }
    char uid[24] = {0};
    if (form_get(buf, "uid", uid, sizeof(uid)) != 0 || uid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Thieu uid\"}");
    }
    char name[48] = {0}, id[48] = {0};
    bool registered = false;
    (void)card_profile_lookup(uid, name, sizeof(name), id, sizeof(id), &registered, NULL);

    esp_err_t err = card_profile_delete(uid);
    if (err == ESP_OK) {
        int32_t msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
#if BOARD_ENABLE_AZURE
        app_azure_send_card_event(uid, name, id, 604, msg_idx);
#endif
        lcd_ui_invalidate_card_cache();
        scan_log_append_admin(uid, name, id, "DEL", msg_idx);
        app_audio_play_confirm();
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Xoa that bai\"}");
}

static esp_err_t api_log_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "log")) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Can PIN muc nay\"}");
    }
    return scan_log_send_json(req);
}

static esp_err_t api_log_sync_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "log")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char buf[128];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (rlen <= 0) return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No data\"}");
    
    char start_s[16] = {0}, end_s[16] = {0};
    form_get(buf, "start", start_s, sizeof(start_s));
    form_get(buf, "end", end_s, sizeof(end_s));
    
    int32_t s = atol(start_s);
    int32_t e = atol(end_s);
    
    if (s <= 0 || e <= 0 || s > e) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Index khong hop le\"}");
    }
    
#if BOARD_ENABLE_AZURE
    int resent = app_azure_resend_range(602, s, e);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"resent\":%d}", resent);
    return httpd_resp_sendstr(req, resp);
#else
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Azure bi tat\"}");
#endif
}

static esp_err_t api_azure_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "azure")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char host[64], dev[32], mask[24];
    wifi_portal_get_azure(host, sizeof(host), dev, sizeof(dev), mask, sizeof(mask));
    char eh[80], ed[48], em[32];
    json_escape(host, eh, sizeof(eh));
    json_escape(dev, ed, sizeof(ed));
    json_escape(mask, em, sizeof(em));
    char resp[280];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"host\":\"%s\",\"devid\":\"%s\",\"sas_mask\":\"%s\",\"connected\":%s}",
             eh, ed, em, app_azure_is_connected() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

/* -----------------------------------------------------------------------
 * /api/brand  GET  — trả về chữ thương hiệu hiện tại (không cần xác thực)
 * /api/brand  POST — lưu chữ thương hiệu mới (cần PIN section "lcd")
 * ----------------------------------------------------------------------- */
static esp_err_t api_brand_get_handler(httpd_req_t *req)
{
    char brand[24] = {0};
    wifi_portal_get_brand_text(brand, sizeof(brand));
    /* Escape đơn giản: chặn '"' */
    for (int i = 0; brand[i]; i++) {
        if (brand[i] == '"' || brand[i] == '\\') brand[i] = ' ';
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"brand\":\"%s\"}", brand);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_brand_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "lcd")) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Can PIN muc Man hinh\"}");
    }
    char buf[64];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (rlen <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Khong nhan du lieu\"}");
    }
    char brand[20] = {0};
    if (form_get(buf, "brand", brand, sizeof(brand)) != 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Thieu truong brand\"}");
    }
    /* Cho phép rỗng (để xóa chữ), nhưng cắt tối đa 15 ký tự */
    if (strlen(brand) > 15) brand[15] = '\0';
    if (wifi_portal_set_brand_text(brand) != ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Luu NVS that bai\"}");
    }
    /* Cập nhật LCD ngay lập tức */
    lv_port_set_brand_text(brand);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/** GET /api/screen — brand + vol + lcd_panel trong 1 request (giam round-trip tren SoftAP). */
static esp_err_t api_screen_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char brand[24] = {0};
    wifi_portal_get_brand_text(brand, sizeof(brand));
    for (int i = 0; brand[i]; i++) {
        if (brand[i] == '"' || brand[i] == '\\') {
            brand[i] = ' ';
        }
    }
    const lcd_panel_profile_t *cur_prof = lcd_panel_get_active();
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"brand\":\"%s\",\"vol\":%u,\"current\":%u,\"current_name\":\"%s\",\"options\":[",
                     brand, (unsigned)app_audio_get_volume(),
                     (unsigned)lcd_panel_get_id(), cur_prof ? cur_prof->name : "");
    const int cnt = lcd_panel_profile_count();
    for (int i = 0; i < cnt && n > 0 && (size_t)n < sizeof(resp) - 80; i++) {
        const lcd_panel_profile_t *p = lcd_panel_get_by_index(i);
        if (!p) {
            continue;
        }
        n += snprintf(resp + n, sizeof(resp) - (size_t)n, "%s{\"id\":%u,\"name\":\"%s\"}", i ? "," : "",
                      (unsigned)p->id, p->name);
    }
    snprintf(resp + n, sizeof(resp) - (size_t)n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_lcd_panel_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    const lcd_panel_profile_t *cur_prof = lcd_panel_get_active();
    char resp[384];
    int n = snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"current\":%u,\"current_name\":\"%s\",\"options\":[",
                     (unsigned)lcd_panel_get_id(), cur_prof ? cur_prof->name : "");
    const int cnt = lcd_panel_profile_count();
    for (int i = 0; i < cnt && n > 0 && (size_t)n < sizeof(resp) - 80; i++) {
        const lcd_panel_profile_t *p = lcd_panel_get_by_index(i);
        if (!p) {
            continue;
        }
        n += snprintf(resp + n, sizeof(resp) - (size_t)n, "%s{\"id\":%u,\"name\":\"%s\"}", i ? "," : "",
                      (unsigned)p->id, p->name);
    }
    snprintf(resp + n, sizeof(resp) - (size_t)n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_vol_get_handler(httpd_req_t *req)
{
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"vol\":%u}", (unsigned)app_audio_get_volume());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t api_vol_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "lcd")) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau xac thuc\"}");
    }
    char buf[64];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (rlen <= 0) return httpd_resp_sendstr(req, "{\"ok\":false}");
    char vol_s[8] = {0};
    if (form_get(buf, "vol", vol_s, sizeof(vol_s)) == 0 && vol_s[0] != '\0') {
        int v = atoi(vol_s);
        if (v >= 0 && v <= 100) {
            app_audio_set_volume((uint8_t)v);
            app_audio_play_confirm();
            return httpd_resp_sendstr(req, "{\"ok\":true}");
        }
    }
    return httpd_resp_sendstr(req, "{\"ok\":false}");
}

static esp_err_t api_lcd_panel_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char buf[32];
    int rlen = portal_recv_small_body(req, buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    if (rlen <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Khong nhan du lieu\"}");
    }
    char panel_s[8] = {0};
    if (form_get(buf, "panel", panel_s, sizeof(panel_s)) != 0 || panel_s[0] == '\0') {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Thieu panel\"}");
    }
    unsigned long pid = strtoul(panel_s, NULL, 10);
    if (pid == 0 || pid > 255) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Profile khong hop le\"}");
    }
    if (pid == (unsigned long)lcd_panel_get_id()) {
        return httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":false,\"msg\":\"Da chon san\"}");
    }
    if (lcd_panel_set_id((uint8_t)pid) != ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Khong luu duoc NVS\"}");
    }
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true,\"msg\":\"Da luu. Dang khoi dong lai...\"}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau dang nhap\"}");
    }
    char resp[320];
    char time_str[32] = "";
    int cnt = wifi_list_get_count();
    wifi_conn_status_t st = wifi_portal_get_conn_status();
    const char *stt = "idle";
    if (st == WIFI_STATUS_CONNECTING) {
        stt = "connecting";
    } else if (st == WIFI_STATUS_CONNECTED) {
        stt = "connected";
    } else if (st == WIFI_STATUS_FAIL) {
        stt = "fail";
    }
    const bool time_ok = wifi_portal_time_is_valid();
    if (time_ok) {
        time_t now = time(NULL);
        struct tm t;
        scan_log_wall_tm(now, &t);
        int tn = snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
                          (int)(t.tm_year + 1900), (int)(t.tm_mon + 1), (int)t.tm_mday, (int)t.tm_hour,
                          (int)t.tm_min, (int)t.tm_sec);
        if (tn < 0 || (size_t)tn >= sizeof(time_str)) {
            time_str[0] = '\0';
        }
    }
#if BOARD_ENABLE_AZURE
    bool az_conn = app_azure_is_connected();
#else
    bool az_conn = false;
#endif
    const lcd_panel_profile_t *lcd_p = lcd_panel_get_active();
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"wifi_saved\":%d,\"wifi_status\":\"%s\",\"sd\":%s,\"time_ok\":%s,\"time\":\"%s\","
             "\"lcd_panel\":%u,\"lcd_panel_name\":\"%s\",\"azure_connected\":%s}",
             cnt, stt, sd_card_is_mounted() ? "true" : "false", time_ok ? "true" : "false", time_str,
             (unsigned)lcd_panel_get_id(), lcd_p ? lcd_p->name : "",
             az_conn ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t portal_favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char *)s_favicon_png, sizeof(s_favicon_png));
}

static esp_err_t api_pin_change_post_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "pin")) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char buf[128];
    int rlen = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (rlen <= 0) {
        return ESP_FAIL;
    }
    buf[rlen] = '\0';
    char oldp[20] = {0}, newp[20] = {0};
    if (form_get(buf, "old_pin", oldp, sizeof(oldp)) != 0 || form_get(buf, "new_pin", newp, sizeof(newp)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Thieu pin");
        return ESP_OK;
    }
    if (!app_login_verify_pin(oldp)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"PIN cu sai\"}");
    }
    if (newp[0] == '\0' || strlen(newp) >= 16) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"PIN moi khong hop le\"}");
    }
    httpd_resp_set_type(req, "application/json");
    if (app_login_save_new_pin(newp) == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Luu that bai\"}");
}

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Máy chấm công-MEBIECO</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#090d16;color:#f3f4f6;min-height:100vh;line-height:1.5}"
    ".wrap{display:flex;min-height:100vh}"
    ".side{width:240px;background:#111827;border-right:1px solid rgba(255,255,255,0.06);padding:24px 0;flex-shrink:0;display:flex;flex-direction:column}"
    ".side h2{font-size:16px;text-align:center;margin:0 16px 24px;font-weight:700;letter-spacing:1px;text-transform:uppercase;background:linear-gradient(135deg,#6366f1,#06b6d4);-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
    ".nav{display:flex;flex-direction:column;gap:6px;padding:0 12px}"
    ".nav a{display:flex;align-items:center;gap:12px;padding:12px 16px;color:#9ca3af;text-decoration:none;font-size:14px;font-weight:500;border-radius:8px;cursor:pointer;transition:all .2s}"
    ".nav a svg{width:18px;height:18px;fill:currentColor;transition:transform .2s;flex-shrink:0}"
    ".nav a:hover{color:#fff;background:rgba(255,255,255,0.03)}"
    ".nav a:hover svg{transform:translateX(2px)}"
    ".nav a.on{color:#fff;background:linear-gradient(135deg,#4f46e5,#06b6d4);box-shadow:0 4px 12px rgba(79,70,229,0.25)}"
    ".nav a.on svg{fill:#fff}"
    ".main{flex:1;padding:32px;overflow:auto;background:radial-gradient(circle at top right,rgba(99,102,241,0.05),transparent 400px)}"
    ".card{background:#1e293b;border:1px solid rgba(255,255,255,0.06);border-radius:16px;padding:28px;box-shadow:0 8px 32px rgba(0,0,0,0.24);max-width:800px;margin:0 auto}"
    "h1{font-size:22px;font-weight:600;margin-bottom:20px;color:#fff;border-bottom:1px solid rgba(255,255,255,0.08);padding-bottom:12px;display:flex;align-items:center;gap:10px}"
    "label{display:block;margin:16px 0 6px;font-size:13px;font-weight:500;color:#d1d5db}"
    "input:not([type=radio]):not([type=checkbox]),select{width:100%;padding:11px 14px;background:#0f172a;border:1px solid #374151;border-radius:8px;font-size:14px;color:#fff;font-family:inherit;transition:all .2s}"
    "input:not([type=radio]):not([type=checkbox]):focus,select:focus{outline:none;border-color:#6366f1;box-shadow:0 0 0 3px rgba(99,102,241,0.25)}"
    ".btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:11px 20px;border:none;border-radius:8px;background:linear-gradient(135deg,#4f46e5,#6366f1);color:#fff;font-weight:600;font-size:14px;cursor:pointer;transition:all .2s;box-shadow:0 4px 10px rgba(79,70,229,0.2);margin:12px 8px 0 0}"
    ".btn:hover{transform:translateY(-1px);box-shadow:0 6px 14px rgba(79,70,229,0.3);opacity:.95}"
    ".btn:active{transform:translateY(0)}"
    ".btn.red{background:linear-gradient(135deg,#dc2626,#ef4444);box-shadow:0 4px 10px rgba(220,38,38,0.2)}"
    ".btn.red:hover{box-shadow:0 6px 14px rgba(220,38,38,0.3)}"
    ".btn.green{background:linear-gradient(135deg,#059669,#10b981);box-shadow:0 4px 10px rgba(5,150,105,0.2)}"
    ".btn.green:hover{box-shadow:0 6px 14px rgba(5,150,105,0.3)}"
    ".btn:disabled{opacity:.5;cursor:not-allowed;transform:none}"
    ".btn-sm{padding:6px 14px;font-size:12px;font-weight:600;border-radius:6px;border:none;color:#fff;cursor:pointer;transition:all .2s;display:inline-flex;align-items:center;justify-content:center}"
    ".btn-sm.green{background:linear-gradient(135deg,#10b981,#059669);box-shadow:0 2px 6px rgba(16,185,129,0.2)}"
    ".btn-sm.green:hover{transform:translateY(-1px);box-shadow:0 4px 10px rgba(16,185,129,0.35);opacity:.95}"
    ".btn-sm.red{background:linear-gradient(135deg,#ef4444,#dc2626);box-shadow:0 2px 6px rgba(239,68,68,0.2)}"
    ".btn-sm.red:hover{transform:translateY(-1px);box-shadow:0 4px 10px rgba(239,68,68,0.35);opacity:.95}"
    ".btn-sm:active{transform:translateY(0)}"
    ".msg{margin-top:12px;font-size:14px;font-weight:500;min-height:20px}"
    ".msg.success{color:#10b981}"
    ".msg.error{color:#ef4444}"
    ".tbl{width:100%;border-collapse:collapse;font-size:13px;margin-top:16px;border-radius:8px;overflow:hidden}"
    ".tbl th{background:#0f172a;color:#9ca3af;font-weight:600;text-transform:uppercase;font-size:11px;letter-spacing:.5px;padding:12px 16px;text-align:left}"
    ".tbl td{padding:12px 16px;border-bottom:1px solid rgba(255,255,255,0.04);color:#e5e7eb}"
    ".tbl tr:last-child td{border-bottom:none}"
    ".tbl tr:nth-child(even){background:rgba(255,255,255,0.01)}"
    ".tbl tr:hover{background:rgba(255,255,255,0.03)}"
    ".ov{position:fixed;inset:0;background:rgba(3,7,18,0.6);backdrop-filter:blur(8px);display:none;align-items:center;justify-content:center;z-index:99;opacity:0;transition:opacity .3s ease}"
    ".ov.show{display:flex;opacity:1}"
    ".pinbox{background:#1e293b;border:1px solid rgba(255,255,255,0.1);padding:28px;border-radius:16px;width:90%;max-width:360px;box-shadow:0 20px 25px -5px rgba(0,0,0,0.5);transform:scale(0.9);transition:transform .3s ease}"
    ".ov.show .pinbox{transform:scale(1)}"
    ".pinbox h3{margin-bottom:12px;font-size:18px;color:#fff;font-weight:600;display:flex;align-items:center;gap:8px}"
    ".hint{font-size:12px;color:#9ca3af;margin-top:12px;line-height:1.4}"
    ".grid-status{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:16px;margin-bottom:24px}"
    ".status-card{background:#151f32;border:1px solid rgba(255,255,255,0.04);border-radius:12px;padding:20px;display:flex;align-items:center;gap:16px;transition:transform .2s,border-color .2s}"
    ".status-card:hover{transform:translateY(-2px);border-color:rgba(99,102,241,0.2)}"
    ".status-icon{width:48px;height:48px;border-radius:10px;display:flex;align-items:center;justify-content:center;font-size:22px;flex-shrink:0}"
    ".status-icon.wifi{background:rgba(59,130,246,0.15);color:#3b82f6}"
    ".status-icon.sd{background:rgba(16,185,129,0.15);color:#10b981}"
    ".status-icon.clock{background:rgba(245,158,11,0.15);color:#f5980b}"
    ".status-icon.azure{background:rgba(99,102,241,0.15);color:#6366f1}"
    ".status-info{flex:1;min-width:0}"
    ".status-info h3{font-size:13px;font-weight:500;color:#9ca3af;text-transform:uppercase;letter-spacing:.5px;margin-bottom:4px}"
    ".status-val{font-size:15px;font-weight:600;color:#fff;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:6px}"
    ".status-badge{display:inline-flex;align-items:center;font-size:11px;font-weight:600;padding:2px 8px;border-radius:9999px;text-transform:capitalize}"
    ".status-badge.success{background:rgba(16,185,129,0.12);color:#34d399;border:1px solid rgba(16,185,129,0.2)}"
    ".status-badge.warning{background:rgba(245,158,11,0.12);color:#fbbf24;border:1px solid rgba(245,158,11,0.2)}"
    ".status-badge.danger{background:rgba(239,68,68,0.12);color:#f87171;border:1px solid rgba(239,68,68,0.2)}"
    ".home-guide{background:rgba(99,102,241,0.06);border:1px solid rgba(99,102,241,0.15);border-radius:12px;padding:18px;display:flex;gap:14px;align-items:flex-start;margin-top:10px}"
    ".home-guide h4{font-size:14px;font-weight:600;color:#fff;margin-bottom:4px}"
    ".home-guide p{font-size:13px;color:#9ca3af;line-height:1.4}"
    ".badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:4px;font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.5px}"
    ".badge.save{background:rgba(16,185,129,0.15);color:#34d399}"
    ".badge.del{background:rgba(239,68,68,0.15);color:#f87171}"
    ".badge.swipe{background:rgba(59,130,246,0.15);color:#60a5fa}"
    "#wlist{list-style:none;margin-top:12px;display:flex;flex-direction:column;gap:8px}"
    "#wlist li{background:#151f32;border:1px solid rgba(255,255,255,0.04);padding:10px 16px;border-radius:8px;display:flex;align-items:center;justify-content:space-between;font-size:14px;font-weight:500}"
    ".spinner{width:20px;height:20px;border:2px solid rgba(255,255,255,0.1);border-top-color:#fff;border-radius:50%;animation:spin .8s linear infinite;display:inline-block;vertical-align:middle}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".lcd-opt{display:flex;align-items:center;gap:12px;margin:14px 0;padding:16px;cursor:pointer;border-radius:12px;border:1px solid rgba(255,255,255,0.06);background:rgba(255,255,255,0.02);transition:all .2s}"
    ".lcd-opt:hover{border-color:rgba(99,102,241,0.3);background:rgba(255,255,255,0.03)}"
    ".lcd-opt input[type=radio]{width:18px;height:18px;margin:0;cursor:pointer;accent-color:#6366f1;flex-shrink:0}"
    ".lcd-opt.sel{border-color:rgba(99,102,241,0.45);background:rgba(99,102,241,0.08)}"
    ".m-hdr{display:none;background:#111827;border-bottom:1px solid rgba(255,255,255,0.06);height:56px;align-items:center;padding:0 16px;position:sticky;top:0;z-index:90}"
    ".m-hdr h2{font-size:15px;font-weight:700;letter-spacing:1px;text-transform:uppercase;background:linear-gradient(135deg,#6366f1,#06b6d4);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin:0 auto}"
    ".m-hdr button{background:none;border:none;color:#9ca3af;cursor:pointer;padding:8px;display:flex;align-items:center;justify-content:center}"
    ".m-hdr button svg{width:24px;height:24px;fill:currentColor}"
    ".m-ov{display:none;position:fixed;inset:0;background:rgba(3,7,18,0.5);backdrop-filter:blur(4px);z-index:89}"
    "@media(max-width:768px){"
    ".m-hdr{display:flex}"
    ".wrap{flex-direction:row;min-height:calc(100vh - 56px)}"
    ".side{position:fixed;top:56px;left:0;bottom:0;transform:translateX(-100%);z-index:91;width:240px;box-shadow:8px 0 24px rgba(0,0,0,0.5);transition:transform .3s ease}"
    ".side.show{transform:translateX(0)}"
    ".m-ov.show{display:block}"
    ".main{padding:16px}"
    ".card{padding:18px}"
    "}"
    "</style></head><body>"
    "<div class=m-hdr id=mHdr style='display:none'><button id=mBtn type=button><svg viewBox='0 0 24 24'><path d='M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z'/></svg></button><h2>MÁY CHẤM CÔNG</h2></div>"
    "<div class=m-ov id=mOv></div>"
    "<div class=wrap id=appLayout style='display:none'><aside class=side id=sideMenu><h2>MÁY CHẤM CÔNG</h2><nav class=nav>"
    "<a data-sec=home class=on><svg viewBox='0 0 24 24'><path d='M10 20v-6h4v6h5v-8h3L12 3 2 12h3v8z'/></svg><span>Trang chủ</span></a>"
    "<a data-sec=wifi><svg viewBox='0 0 24 24'><path d='M12 3C7.3 3 3.1 5.3 0 9l2 2.5C4.7 8.3 8.2 6.5 12 6.5s7.3 1.8 10 5L24 9c-3.1-3.7-7.3-6-12-6zm0 5c-3.5 0-6.7 1.6-8.9 4.2l1.9 2.4c1.7-2 4.2-3.2 7-3.2s5.3 1.2 7 3.2l1.9-2.4C18.7 14.6 15.5 13 12 13zm0 5c-2.1 0-3.9.9-5.1 2.4L12 24.2l5.1-3.8c-1.2-1.5-3-2.4-5.1-2.4z'/></svg><span>WiFi</span></a>"
    "<a data-sec=azure><svg viewBox='0 0 24 24'><path d='M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM19 18H6c-2.21 0-4-1.79-4-4 0-2.05 1.53-3.76 3.56-3.97l1.07-.11.5-.95C8.08 7.14 9.94 6 12 6c2.62 0 4.88 1.86 5.39 4.43l.3 1.5 1.53.11c1.56.1 2.78 1.41 2.78 2.96 0 1.65-1.35 3-3 3z'/></svg><span>Azure</span></a>"
    "<a data-sec=log><svg viewBox='0 0 24 24'><path d='M19 3h-4.18C14.4 1.84 13.3 1 12 1c-1.3 0-2.4.84-2.82 2H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-7 0c.55 0 1 .45 1 1s-.45 1-1 1-1-.45-1-1 .45-1 1-1zm2 14H7v-2h7v2zm3-4H7v-2h10v2zm0-4H7V7h10v2z'/></svg><span>Nhật ký</span></a>"
    "<a data-sec=cards><svg viewBox='0 0 24 24'><path d='M4 6H2v14c0 1.1.9 2 2 2h14v-2H4V6zm16-4H8c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h12c1.1 0-2-.9-2-2V4c0-1.1-.9-2-2-2zm0 14H8V4h12v12z'/></svg><span>DS thẻ</span></a>"
    "<a data-sec=register><svg viewBox='0 0 24 24'><path d='M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z'/></svg><span>Đăng ký thẻ</span></a>"
    "<a data-sec=pin><svg viewBox='0 0 24 24'><path d='M12.65 10C11.83 7.67 9.61 6 7 6c-3.31 0-6 2.69-6 6s2.69 6 6 6c2.61 0 4.83-1.67 5.65-4H17v4h4v-4h2v-4H12.65zM7 14c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2z'/></svg><span>Đổi PIN</span></a>"
    "<a data-sec=lcd><svg viewBox='0 0 24 24'><path d='M21 3H3c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h7v2H8v2h8v-2h-2v-2h7c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm0 14H3V5h18v12z'/></svg><span>Màn hình</span></a>"
    "<a id=logoutBtn style='color:#ef4444;margin-top:auto'><svg viewBox='0 0 24 24'><path d='M10.09 15.59L11.5 17l5-5-5-5-1.41 1.41L12.67 11H3v2h9.67l-2.58 2.59zM19 3H5c-1.11 0-2 .9-2 2v4h2V5h14v14H5v-4H3v4c0 1.1.89 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2z'/></svg><span>Đăng xuất</span></a>"
    "</nav></aside><main class=main><div class=card id=panel>"
    "<h1 id=title>Trang chủ</h1><div id=body><p id=st>Đang tải...</p></div></div></main></div>"
    "<div id=loginPage style='display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px;background:#090d16'>"
    "<div class=card style='width:100%;max-width:360px;padding:32px'>"
    "<h2 style='text-align:center;margin-bottom:24px;font-size:20px;font-weight:700;background:linear-gradient(135deg,#6366f1,#06b6d4);-webkit-background-clip:text;-webkit-text-fill-color:transparent'>ADMIN LOGIN</h2>"
    "<label>Tài khoản</label><input id=luser placeholder='Taikhoan' autocomplete='username'>"
    "<label>Mật khẩu</label><input type=password id=lpass placeholder='Mat khau ...' autocomplete='current-password'>"
    "<p class=msg id=lmsg style='margin-top:12px'></p>"
    "<button class=btn id=lbtn style='width:100%;margin-top:16px'>Đăng nhập</button>"
    "</div></div>"
    "<div class=ov id=pinOv><div class=pinbox><h3>"
    "<svg viewBox='0 0 24 24' style='width:20px;height:20px;fill:#6366f1;vertical-align:middle;margin-right:6px'><path d='M18 8h-1V6c0-2.76-2.24-5-5-5S7 3.24 7 6v2H6c-1.1 0-2 .9-2 2v10c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V10c0-1.1-.9-2-2-2zm-6 9c-1.1 0-2-.9-2-2s.9-2 2-2 2 .9 2 2-.9 2-2 2zm3.1-9H8.9V6c0-1.71 1.39-3.1 3.1-3.1 1.71 0 3.1 1.39 3.1 3.1v2z'/></svg>"
    "<span id=pinTitle>Nhập PIN</span></h3>"
    "<input type=password id=pinIn maxlength=15 placeholder='Nhập mã PIN xác thực...' autocomplete=off>"
    "<div style='margin-top:16px;display:flex;gap:10px'><button class=btn id=pinOk style='flex:1;margin:0'>Xác nhận</button>"
    "<button class=btn style='background:linear-gradient(135deg,#475569,#64748b);box-shadow:none;flex:1;margin:0' id=pinCancel>Hủy</button></div>"
    "<p class=hint>Nhập Pin Quá 5 lần sẽ bị khóa.</p></div></div>"
    "<div class=ov id=regOv><div class=pinbox style='max-width:400px'><h3>"
    "<svg viewBox='0 0 24 24' style='width:20px;height:20px;fill:#6366f1;vertical-align:middle;margin-right:6px'><path d='M19 3h-4.18C14.4 1.84 13.3 1 12 1c-1.3 0-2.4.84-2.82 2H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-7 0c.55 0 1 .45 1 1s-.45 1-1 1-1-.45-1-1 .45-1 1-1zm2 14H7v-2h7v2zm3-4H7v-2h10v2zm0-4H7V7h10v2z'/></svg>"
    "<span id=regTitle>Đăng ký nhân viên</span></h3>"
    "<label>Tên nhân viên</label><input id=regName placeholder='Nhập tên nhân viên...' autocomplete=off>"
    "<label>Mã nhân viên</label><input id=regId placeholder='Nhập mã nhân viên...' autocomplete=off>"
    "<div style='margin-top:20px;display:flex;gap:10px'><button class=btn id=regOk style='flex:1;margin:0'>Lưu</button>"
    "<button class=btn style='background:linear-gradient(135deg,#475569,#64748b);box-shadow:none;flex:1;margin:0' id=regCancel>Hủy</button></div></div></div>"
    "<script>"
    "const T={home:'Trang chủ',wifi:'Cấu hình WiFi',azure:'Cấu hình Azure IoT',log:'Nhật ký hoạt động',cards:'Danh sách thẻ',register:'Đăng ký thẻ mới',pin:'Đổi mã PIN',lcd:'Cấu hình màn hình'};"
    "let cur='home',tokens=JSON.parse(sessionStorage.getItem('portal_tokens')||'{}'),pendSec=null;"
    "const tabCache={};const TAB_TTL={status:5000,azure:30000,screen:60000};"
    "function tcGet(k){const e=tabCache[k];if(!e)return null;const ttl=TAB_TTL[k]||0;if(ttl&&Date.now()-e.t>ttl){delete tabCache[k];return null;}return e.d;}"
    "function tcSet(k,d){tabCache[k]={t:Date.now(),d};}"
    "function tcClr(k){if(k){delete tabCache[k];}else{for(const x in tabCache)delete tabCache[x];}}"
    "let logFilter={range:'today',from:'',to:'',code:'all',sort:'desc'};"
    "let cardFilter={id:''};"
    "function cardsQueryUrl(unreg,page){page=page||1;const p=new URLSearchParams();p.set('page',page);"
    "if(unreg)p.set('unreg','1');else if(cardFilter.id)p.set('id',cardFilter.id);"
    "return '/api/cards?'+p.toString();}"
    "function logSel(v,cur){return v===cur?' selected':'';}"
    "function logQueryUrl(page){page=page||1;const p=new URLSearchParams();p.set('page',page);"
    "if(logFilter.range==='all')p.set('all','1');"
    "else if(logFilter.range==='7')p.set('days','7');"
    "else if(logFilter.range==='30')p.set('days','30');"
    "else if(logFilter.range==='custom'){if(logFilter.from)p.set('from',logFilter.from);if(logFilter.to)p.set('to',logFilter.to);}"
    "if(logFilter.code&&logFilter.code!=='all')p.set('code',logFilter.code);"
    "p.set('sort',logFilter.sort||'desc');return '/api/log?'+p.toString();}"
    "function $(id){return document.getElementById(id)}"
    "function hdr(sec){const h={'Content-Type':'application/x-www-form-urlencoded'};"
    "const t=sec?tokens[sec]:tokens['admin'];"
    "if(t)h['X-Portal-Token']=t;return h;}"
    "function api(sec,url,opt){return fetch(url,Object.assign({headers:hdr(sec)},opt||{}));}"
    "function showPin(sec,cb){pendSec=sec;const ov=$('pinOv');$('pinTitle').innerText='Mật khẩu PIN - '+T[sec];"
    "$('pinIn').value='';ov.classList.add('show');$('pinIn').focus();"
    "const ok=()=>{const p=$('pinIn').value;fetch('/api/unlock',{method:'POST',"
    "headers:hdr(''),"
    "body:'section='+encodeURIComponent(sec)+'&pin='+encodeURIComponent(p)})"
    ".then(r=>r.json()).then(d=>{if(d.ok){tokens[sec]=d.token;sessionStorage.setItem('portal_tokens',JSON.stringify(tokens));ov.classList.remove('show');cb();}"
    "else alert(d.error||'Mã PIN không đúng');});};"
    "$('pinOk').onclick=ok;$('pinIn').onkeydown=e=>{if(e.key==='Enter')ok();};"
    "$('pinCancel').onclick=()=>ov.classList.remove('show');}"
    "function need(sec,fn){if(tokens[sec]){fn();return;}showPin(sec,fn);}"
    "function renderHomeData(d){"
    "const sttLabels={idle:'Ngoại tuyến (Chờ)',connecting:'Đang kết nối...',connected:'Đã kết nối',fail:'Lỗi kết nối'};"
    "const sttColors={idle:'warning',connecting:'warning',connected:'success',fail:'danger'};"
    "const wifiLbl=sttLabels[d.wifi_status]||d.wifi_status;"
    "const wifiClr=sttColors[d.wifi_status]||'warning';"
    "const sdLbl=d.sd?'Hoạt động':'Trống';"
    "const sdClr=d.sd?'success':'danger';"
    "const sdTxt=d.sd?'Đã gắn thẻ nhớ SD':'Chưa gắn thẻ nhớ SD';"
    "const timeLbl=d.time_ok?'Đồng bộ':'Chưa có giờ';"
    "const timeClr=d.time_ok?'success':'danger';"
    "const timeTxt=d.time_ok?(d.time||'Đã đồng bộ'):'Chờ WiFi + Internet';"
    "const azLbl=d.azure_connected?'Đã kết nối':'Chưa kết nối';"
    "const azClr=d.azure_connected?'success':'danger';"
    "const azTxt=d.azure_connected?'Đang hoạt động':'Mất kết nối';"
    "let h='<div class=grid-status>'"
    "+'<div class=status-card><div class=\"status-icon wifi\">📶</div><div class=status-info><h3>WiFi</h3><p class=status-val>Đã lưu: '+d.wifi_saved+' AP</p><span class=\"status-badge '+wifiClr+'\">'+wifiLbl+'</span></div></div>'"
    "+'<div class=status-card><div class=\"status-icon azure\">☁️</div><div class=status-info><h3>Azure IoT</h3><p class=status-val>'+azTxt+'</p><span class=\"status-badge '+azClr+'\">'+azLbl+'</span></div></div>'"
    "+'<div class=status-card><div class=\"status-icon sd\">💾</div><div class=status-info><h3>Thẻ nhớ SD</h3><p class=status-val>'+sdTxt+'</p><span class=\"status-badge '+sdClr+'\">'+sdLbl+'</span></div></div>'"
    "+'<div class=status-card><div class=\"status-icon clock\">🕒</div><div class=status-info><h3>Giờ hệ thống</h3><p class=status-val>'+timeTxt+'</p><span class=\"status-badge '+timeClr+'\">'+timeLbl+'</span></div></div>'"
    "+'</div>'"
    "+'<div class=home-guide><div style=font-size:20px>🔒</div><div><h4>Hướng dẫn sử dụng</h4><p>Chọn các chức năng ở menu bên trái để cấu hình thiết bị. Một số mục bảo mật yêu cầu bạn nhập PIN của thiết bị để truy cập.</p></div></div>';"
    "$('body').innerHTML=h;}"
    "function renderHome(){"
    "const c=tcGet('status');if(c){renderHomeData(c);return;}"
    "$('body').innerHTML='<p class=msg>Đang tải trạng thái hệ thống...</p>';"
    "const ctrl=new AbortController();const tid=setTimeout(()=>ctrl.abort(),10000);"
    "fetch('/api/status',{headers:hdr(''),signal:ctrl.signal}).then(r=>{clearTimeout(tid);"
    "if(r.status===401||r.status===403){tokens={};sessionStorage.removeItem('portal_tokens');tcClr();checkLogin();return null;}"
    "if(!r.ok)throw new Error('HTTP '+r.status);return r.json();"
    "}).then(d=>{if(!d)return;tcSet('status',d);renderHomeData(d);"
    "}).catch(e=>{clearTimeout(tid);if(!tokens['admin'])return;"
    "$('body').innerHTML='<p class=msg error>'+(e.name==='AbortError'?'Máy chủ không phản hồi (ESP bận hoặc mất WiFi AP).':'Lỗi: '+(e.message||'kết nối'))+'</p>'"
    "+'<button class=btn style=margin-top:12px onclick=\"renderHome()\">Thử lại</button>';});}"
    "function renderWifi(){"
    "let h='<div style=\"max-width:500px\">'"
    "+'<label>Tên WiFi (SSID)</label>'"
    "+'<div style=\"display:flex;gap:8px;margin-bottom:8px\">'"
    "+'<input id=wssid list=wl placeholder=\"Chọn hoặc nhập tên WiFi...\" style=\"flex:1\">'"
    "+'<button class=btn type=button id=wscan style=\"margin:0\">Quét WiFi</button>'"
    "+'</div><datalist id=wl></datalist>'"
    "+'<label>Mật khẩu WiFi</label>'"
    "+'<input type=password id=wpass maxlength=63 placeholder=\"Nhập mật khẩu WiFi...\">'"
    "+'<p class=msg id=wmsg></p>'"
    "+'<div style=\"margin-top:12px\">'"
    "+'<button class=\"btn green\" id=wsave>Lưu & kết nối</button>'"
    "+'<button class=\"btn red\" id=wclr>Xóa tất cả WiFi</button>'"
    "+'</div>'"
    "+'</div>'"
    "+'<h3 style=\"margin-top:24px;font-size:15px;border-bottom:1px solid rgba(255,255,255,0.08);padding-bottom:8px\">Mạng WiFi đã lưu</h3>'"
    "+'<ul id=wlist></ul>';"
    "$('body').innerHTML=h;"
    "function wscanFill(d){const l=$('wl');if(!l)return;l.innerHTML='';(Array.isArray(d)?d:[]).forEach(i=>{const o=document.createElement('option');o.value=i.ssid;l.appendChild(o);});}"
    "function wscanRun(fresh){const btn=$('wscan');if(!btn)return;btn.disabled=true;btn.innerHTML='<span class=spinner></span>';"
    "const poll=()=>{api('wifi','/scan'+(fresh?'?fresh=1':'')).then(r=>r.json()).then(d=>{"
    "if(d&&d.scanning){setTimeout(poll,400);return;}"
    "wscanFill(d);btn.disabled=false;btn.innerText='Quét WiFi';"
    "}).catch(()=>{btn.disabled=false;btn.innerText='Quét WiFi';});};poll();};"
    "$('wscan').onclick=()=>wscanRun(true);"
    "function wlist(){api('wifi','/saved_wifis').then(r=>r.json()).then(a=>{"
    "const u=$('wlist');u.innerHTML='';a.forEach(s=>{"
    "const li=document.createElement('li');"
    "li.innerHTML=s+' <button class=\"btn-sm red\" data-s=\"'+s+'\"><svg viewBox=\"0 0 24 24\" style=\"width:12px;height:12px;fill:currentColor;margin-right:4px;vertical-align:middle\"><path d=\"M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z\"/></svg>Xóa</button>';"
    "li.querySelector('button').onclick=()=>{if(!confirm('Xóa mạng WiFi '+s+'?'))return;"
    "api('wifi','/del_wifi',{method:'POST',body:'ssid='+encodeURIComponent(s)}).then(r=>{"
    "if(!r.ok){alert('Không xóa được WiFi (mã '+r.status+'). Thử nhập lại PIN WiFi.');return;}wlist();});};"
    "u.appendChild(li);});});}"
    "wlist();wscanRun(false);"
    "$('wsave').onclick=()=>{const b=new URLSearchParams();b.set('ssid',$('wssid').value);b.set('pass',$('wpass').value);"
    "api('wifi','/save_wifi',{method:'POST',body:b}).then(r=>{const ok=r.ok;$('wmsg').innerText=ok?'Đã lưu cấu hình và đang kết nối...':'Lỗi khi lưu WiFi';$('wmsg').className='msg '+(ok?'success':'error');if(ok)tcClr('status');wlist();});};"
    "$('wclr').onclick=()=>{if(!confirm('Xóa toàn bộ WiFi đã lưu?'))return;"
    "api('wifi','/clear',{method:'POST'}).then(r=>{if(!r.ok){alert('Không xóa được (mã '+r.status+')');return;}wlist();});};}"
    "function renderAzureData(c){"
    "const statusColor=c.connected?'#10b981':'#ef4444';"
    "const statusText=c.connected?'Đã kết nối':'Chưa kết nối';"
    "let h='<div style=\"background:rgba(255,255,255,0.02);border:1px solid rgba(255,255,255,0.04);border-radius:12px;padding:16px;margin-bottom:20px;font-size:14px\">'"
    "+'<div style=\"font-size:12px;color:#9ca3af;margin-bottom:8px\">Cấu hình đang lưu trên thiết bị</div>'"
    "+'<div>Trạng thái MQTT: <b style=\"color:'+statusColor+'\">'+statusText+'</b></div>'"
    "+'<div style=\"margin-top:6px\">HostName: <b style=\"color:#fff\">'+(c.host||'(Chưa cấu hình)')+'</b></div>'"
    "+'<div style=\"margin-top:6px\">Device ID: <b style=\"color:#fff\">'+(c.devid||'(Chưa cấu hình)')+'</b></div>'"
    "+'<div style=\"margin-top:6px\">SAS Key: <code style=\"color:#06b6d4\">'+(c.sas_mask||'(Chưa cấu hình)')+'</code></div>'"
    "+'</div>'"
    "+'<div style=\"max-width:500px\">'"
    "+'<p style=\"font-size:13px;color:#9ca3af;margin-bottom:12px\">Nhập đầy đủ 3 trường bên dưới mỗi lần lưu (ghi đè cấu hình cũ).</p>'"
    "+'<label>Azure HostName</label><input id=ah maxlength=63 placeholder=\"Ví dụ: myiot.azure-devices.net\">'"
    "+'<label>Device ID</label><input id=ad maxlength=31 placeholder=\"Ví dụ: Device-01\">'"
    "+'<label>SAS Key (Primary key)</label><input type=password id=as maxlength=127 placeholder=\"Dán Primary key từ Azure Portal\">'"
    "+'<p class=msg id=amsg></p>'"
    "+'<div style=\"margin-top:12px\">'"
    "+'<button class=\"btn green\" id=asv>Lưu cấu hình</button>'"
    "+'<button class=\"btn red\" id=acl>Xóa cấu hình</button>'"
    "+'</div>'"
    "+'</div>';"
    "$('body').innerHTML=h;"
    "$('asv').onclick=()=>{const hVal=$('ah').value.trim(),dVal=$('ad').value.trim(),sVal=$('as').value.trim();"
    "if(!hVal||!dVal){alert('Vui lòng nhập HostName và Device ID!');return;}"
    "if(!sVal){alert('Vui lòng nhập SAS Key (Primary key)!');return;}"
    "const b=new URLSearchParams();b.set('azure_host',hVal);"
    "b.set('azure_devid',dVal);b.set('azure_sas_key',sVal);"
    "api('azure','/save_azure',{method:'POST',body:b}).then(r=>{"
    "const ok=r.ok;"
    "if(ok){"
    "tcClr('azure');tcClr('status');"
    "$('amsg').innerText='Đã lưu — thiết bị đang kết nối Azure...';"
    "$('amsg').className='msg success';"
    "setTimeout(renderAzure,1500);"
    "}else{"
    "r.text().then(msg=>{"
    "$('amsg').innerText=msg||'Có lỗi xảy ra';"
    "$('amsg').className='msg error';"
    "}).catch(()=>{"
    "$('amsg').innerText='Có lỗi xảy ra';"
    "$('amsg').className='msg error';"
    "});"
    "}"
    "});};"
    "$('acl').onclick=()=>{if(confirm('Xóa cấu hình Azure IoT?'))api('azure','/clear_azure',{method:'POST'}).then(()=>{tcClr('azure');tcClr('status');renderAzure();});};}"
    "function renderAzure(){"
    "const c=tcGet('azure');if(c){renderAzureData(c);return;}"
    "$('body').innerHTML='<p class=msg>Đang tải cấu hình Azure...</p>';"
    "api('azure','/api/azure').then(r=>r.json()).catch(()=>({})).then(c=>{tcSet('azure',c);renderAzureData(c);});}"
    "function renderLog(page){page=page||1;"
    "$('body').innerHTML='<p class=msg>Đang tải nhật ký...</p>';"
    "api('log',logQueryUrl(page)).then(r=>r.json()).catch(()=>({})).then(d=>{"
    "let h='';if(d.time_ok===false)h+='<div class=home-guide style=\"margin-bottom:16px;background:rgba(245,158,11,0.06);border-color:rgba(245,158,11,0.15)\"><div>🕒</div><div><h4>Thời gian chưa đồng bộ</h4><p>Thiết bị chưa cập nhật được giờ NTP từ internet. Lọc theo ngày có thể không chính xác.</p></div></div>';"
    "h+='<div style=\"margin-bottom:16px;padding:14px;background:rgba(255,255,255,0.02);border-radius:10px;border:1px solid rgba(255,255,255,0.06)\">';"
    "h+='<div style=\"display:flex;flex-wrap:wrap;gap:12px;align-items:flex-end\">';"
    "h+='<div><label style=\"display:block;font-size:12px;color:#9ca3af;margin-bottom:4px\">Thời gian</label><select id=logRange style=\"min-width:130px\">';"
    "h+='<option value=today'+logSel('today',logFilter.range)+'>Hôm nay</option>';"
    "h+='<option value=7'+logSel('7',logFilter.range)+'>7 ngày</option>';"
    "h+='<option value=30'+logSel('30',logFilter.range)+'>30 ngày</option>';"
    "h+='<option value=all'+logSel('all',logFilter.range)+'>Tất cả</option>';"
    "h+='<option value=custom'+logSel('custom',logFilter.range)+'>Tùy chọn</option></select></div>';"
    "h+='<div id=logCustomDates style=\"display:'+(logFilter.range==='custom'?'flex':'none')+';gap:8px;flex-wrap:wrap\">';"
    "h+='<div><label style=\"display:block;font-size:12px;color:#9ca3af;margin-bottom:4px\">Từ ngày</label><input type=date id=logFrom value=\"'+(logFilter.from||'')+'\"></div>';"
    "h+='<div><label style=\"display:block;font-size:12px;color:#9ca3af;margin-bottom:4px\">Đến ngày</label><input type=date id=logTo value=\"'+(logFilter.to||'')+'\"></div></div>';"
    "h+='<div><label style=\"display:block;font-size:12px;color:#9ca3af;margin-bottom:4px\">Loại</label><select id=logCode style=\"min-width:130px\">';"
    "h+='<option value=all'+logSel('all',logFilter.code)+'>Tất cả</option>';"
    "h+='<option value=602'+logSel('602',logFilter.code)+'>Quẹt thẻ</option>';"
    "h+='<option value=601'+logSel('601',logFilter.code)+'>Thẻ lạ</option>';"
    "h+='<option value=603'+logSel('603',logFilter.code)+'>Lưu thẻ</option>';"
    "h+='<option value=604'+logSel('604',logFilter.code)+'>Xóa thẻ</option></select></div>';"
    "h+='<div><label style=\"display:block;font-size:12px;color:#9ca3af;margin-bottom:4px\">Sắp xếp</label><select id=logSort style=\"min-width:170px\">';"
    "h+='<option value=desc'+logSel('desc',logFilter.sort)+'>Thời gian: mới nhất</option>';"
    "h+='<option value=asc'+logSel('asc',logFilter.sort)+'>Thời gian: cũ nhất</option>';"
    "h+='<option value=index_desc'+logSel('index_desc',logFilter.sort)+'>Index: cao → thấp</option>';"
    "h+='<option value=index_asc'+logSel('index_asc',logFilter.sort)+'>Index: thấp → cao</option>';"
    "h+='<option value=id_asc'+logSel('id_asc',logFilter.sort)+'>Mã NV: A → Z</option>';"
    "h+='<option value=id_desc'+logSel('id_desc',logFilter.sort)+'>Mã NV: Z → A</option></select></div>';"
    "h+='<button class=btn id=logApply style=\"margin:0\">Áp dụng</button>';"
    "h+='<button class=btn id=lr style=\"margin:0;background:linear-gradient(135deg,#334155,#475569);box-shadow:none\">Tải lại</button>';"
    "h+='</div></div>';"
    "h+='<div style=\"margin-bottom:16px;background:rgba(255,255,255,0.02);padding:12px;border-radius:8px;border:1px solid rgba(255,255,255,0.04)\">'"
    "+'<h4 style=\"margin-bottom:8px;font-size:14px;color:#fff\">Đẩy lại dữ liệu lên Azure </h4>'"
    "+'<div style=\"display:flex;gap:10px;align-items:center;flex-wrap:wrap\">'"
    "+'<input id=syncStart type=number placeholder=\"Từ Index\" style=\"width:140px;padding:8px\">'"
    "+'<input id=syncEnd type=number placeholder=\"Đến Index\" style=\"width:140px;padding:8px\">'"
    "+'<button class=\"btn green\" id=syncBtn style=\"margin:0\">Đẩy lại</button>'"
    "+'<span id=syncMsg class=msg style=\"margin:0;min-height:0;margin-left:10px\"></span>'"
    "+'</div></div>';"
    "h+='<table class=tbl><thead><tr><th>Thao tác</th><th>Index</th><th>Thời gian</th><th>Tên nhân viên</th><th>Mã nhân viên</th></tr></thead><tbody id=logRows></tbody></table>';"
    "if(d.total>d.limit){"
    "const totP=Math.ceil(d.total/d.limit);"
    "h+='<div style=\"margin-top:16px;display:flex;gap:8px;justify-content:center;align-items:center\">';"
    "if(page>1)h+='<button class=\"btn\" onclick=\"renderLog('+(page-1)+')\">&lt; Trang trước</button>';"
    "h+='<span style=\"color:#9ca3af;font-size:14px\">Trang '+page+' / '+totP+' ('+d.total+' dòng)</span>';"
    "if(page<totP)h+='<button class=\"btn\" onclick=\"renderLog('+(page+1)+')\">Trang sau &gt;</button>';"
    "h+='</div>';}"
    "$('body').innerHTML=h;"
    "if(d.sort_truncated)h+='<p class=msg style=\"margin-bottom:12px;color:#fbbf24\">Có '+(d.total_filtered||0)+' dòng khớp lọc; chỉ sắp xếp 400 dòng đầu — thu hẹp bộ lọc để chính xác hơn.</p>';"
    "renderLogRows(d.rows||[],d.sort||logFilter.sort);"
    "$('logRange').onchange=()=>{const cd=$('logCustomDates');if(cd)cd.style.display=$('logRange').value==='custom'?'flex':'none';};"
    "const logApplyFn=()=>{logFilter.range=$('logRange').value;logFilter.from=$('logFrom')?$('logFrom').value:'';"
    "logFilter.to=$('logTo')?$('logTo').value:'';logFilter.code=$('logCode').value;logFilter.sort=$('logSort').value;renderLog(1);};"
    "$('logApply').onclick=logApplyFn;"
    "$('lr').onclick=()=>renderLog(page);"
    "const sb=$('syncBtn');if(sb)sb.onclick=()=>{"
    "const st=$('syncStart').value,en=$('syncEnd').value;"
    "if(!st||!en||parseInt(st)>parseInt(en)){alert('Khoảng Index không hợp lệ');return;}"
    "sb.disabled=true;$('syncMsg').innerText='Đang đẩy...';$('syncMsg').className='msg';"
    "api('log','/api/log_sync',{method:'POST',body:'start='+encodeURIComponent(st)+'&end='+encodeURIComponent(en)}).then(r=>r.json()).then(x=>{"
    "sb.disabled=false;"
    "$('syncMsg').innerText=x.ok?('Đã đẩy '+x.resent+' bản ghi'):(x.error||'Lỗi');"
    "$('syncMsg').className='msg '+(x.ok?'success':'error');"
    "}).catch(()=>{"
    "sb.disabled=false;$('syncMsg').innerText='Lỗi mạng';$('syncMsg').className='msg error';"
    "});};"
    "});}"
    "function renderLogRows(rows,sort){"
    "let html='';"
    "if(rows.length===0){html='<tr><td colspan=5 style=\"text-align:center;color:#9ca3af;padding:24px\">Không có dữ liệu phù hợp bộ lọc</td></tr>';}"
    "else{"
    "if(sort==='desc')rows.reverse();"
    "rows.forEach(r=>{"
    "const b=r.admin?(r.action==='DEL'?'<span class=\"badge del\">Xóa thẻ</span>':(r.action==='SAVE'?'<span class=\"badge save\">Lưu thẻ</span>':'<span class=\"badge del\">Admin</span>')):(r.code===601?'<span class=\"badge\" style=\"background:#f59e0b;color:#fff\">Thẻ lạ</span>':'<span class=\"badge swipe\">Quẹt thẻ</span>');"
    "const idxVal=(r.index!==undefined&&r.index!==-1)?r.index:'-';"
    "const dispName=r.name||(r.uid?('UID '+r.uid):'')||'<em>Chưa đăng ký</em>';"
    "const dispId=r.id||'';"
    "html+='<tr><td>'+b+'</td><td><code style=\"color:#06b6d4\">'+idxVal+'</code></td><td>'+r.ts+'</td><td>'+dispName+'</td><td><span>'+dispId+'</span></td></tr>';"
    "});}"
    "$('logRows').innerHTML=html;}"
    "function cardDelDone(sec,unreg,d){if(d.ok){renderCards(unreg);if(cur==='log')renderLog();}else alert(d.error||'Xóa thất bại');}"
    "function cardDelReq(sec,unreg,u){api(sec,'/api/cards/del',{method:'POST',body:'uid='+encodeURIComponent(u)}).then(r=>r.json()).then(d=>cardDelDone(sec,unreg,d)).catch(()=>alert('Lỗi kết nối khi xóa thẻ'));}"
    "function renderCards(unreg,page){page=page||1;"
    "const sec=unreg?'register':'cards';"
    "$('body').innerHTML='<p class=msg>Đang tải danh sách thẻ...</p>';"
    "api(sec,cardsQueryUrl(unreg,page)).then(r=>r.json()).catch(()=>({})).then(d=>{"
    "let h='';"
    "if(!unreg){"
    "h+='<div style=\"margin-bottom:16px;padding:14px;background:rgba(255,255,255,0.02);border-radius:10px;border:1px solid rgba(255,255,255,0.06);display:flex;flex-wrap:wrap;gap:10px;align-items:flex-end\">';"
    "h+='<div style=\"flex:1;min-width:200px\"><label style=\"display:block;font-size:12px;color:#9ca3af;margin-bottom:4px\">Lọc theo mã nhân viên</label>';"
    "h+='<input id=cardIdFilter placeholder=\"Nhập mã NV (vd: NV001)...\" value=\"'+(cardFilter.id||'')+'\" style=\"max-width:320px\"></div>';"
    "h+='<button class=btn id=cardFilterBtn style=\"margin:0\">Áp dụng</button>';"
    "h+='<button class=btn id=cardFilterClr style=\"margin:0;background:linear-gradient(135deg,#334155,#475569);box-shadow:none\">Xóa lọc</button>';"
    "h+='</div>';}"
    "h+='<table class=tbl><thead><tr><th>Mã thẻ UID</th>';"
    "if(!unreg)h+='<th>Tên nhân viên</th><th>Mã nhân viên</th>';"
    "else h+='<th>Thời gian quét</th>';"
    "h+='<th style=\"text-align:right\">Thao tác</th></tr></thead><tbody>';"
    "const list=d.cards||[];"
    "if(list.length===0){"
    "h+='<tr><td colspan='+(unreg?3:4)+' style=\"text-align:center;color:#9ca3af;padding:24px\">'+(unreg?'Không có thẻ mới nào đang đợi đăng ký':(cardFilter.id?'Không tìm thấy nhân viên với mã \"'+cardFilter.id+'\"':'Không có thẻ nào trong hệ thống'))+'</td></tr>';"
    "}else{"
    "list.forEach(c=>{"
    "h+='<tr><td><code style=\"color:#06b6d4\">'+c.uid+'</code></td>';"
    "if(!unreg)h+='<td>'+(c.name||'<em>Chưa đặt tên</em>')+'</td><td>'+(c.id||'<em>Chưa có mã</em>')+'</td>';"
    "else h+='<td>'+(c.date||'<em>---</em>')+'</td>';"
    "h+='<td style=\"text-align:right\">';"
    "if(!unreg)h+='<button class=\"btn-sm green\" style=\"margin-right:6px\" data-u=\"'+c.uid+'\" data-n=\"'+(c.name||'')+'\" data-i=\"'+(c.id||'')+'\"><svg viewBox=\"0 0 24 24\" style=\"width:12px;height:12px;fill:currentColor;margin-right:4px;vertical-align:middle\"><path d=\"M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z\"/></svg>Sửa</button>'"
    "+'<button class=\"btn-sm red\" data-u=\"'+c.uid+'\"><svg viewBox=\"0 0 24 24\" style=\"width:12px;height:12px;fill:currentColor;margin-right:4px;vertical-align:middle\"><path d=\"M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z\"/></svg>Xóa</button>';"
    "else h+='<button class=\"btn-sm green\" style=\"margin-right:6px\" data-u=\"'+c.uid+'\" data-n=\"'+(c.name||'')+'\" data-i=\"'+(c.id||'')+'\"><svg viewBox=\"0 0 24 24\" style=\"width:12px;height:12px;fill:currentColor;margin-right:4px;vertical-align:middle\"><path d=\"M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z\"/></svg>Đăng ký</button>'"
    "+'<button class=\"btn-sm red\" data-del-unreg=\"'+c.uid+'\"><svg viewBox=\"0 0 24 24\" style=\"width:12px;height:12px;fill:currentColor;margin-right:4px;vertical-align:middle\"><path d=\"M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z\"/></svg>Xóa</button>';"
    "h+='</td></tr>';"
    "});"
    "}"
    "h+='</tbody></table>';"
    "if(d.total>d.limit){"
    "const totP=Math.ceil(d.total/d.limit);"
    "h+='<div style=\"margin-top:16px;display:flex;gap:8px;justify-content:center;align-items:center\">';"
    "if(page>1)h+='<button class=\"btn\" onclick=\"renderCards('+(unreg?'true':'false')+','+(page-1)+')\">&lt; Trang trước</button>';"
    "h+='<span style=\"color:#9ca3af;font-size:14px\">Trang '+page+' / '+totP+' ('+d.total+' thẻ)</span>';"
    "if(page<totP)h+='<button class=\"btn\" onclick=\"renderCards('+(unreg?'true':'false')+','+(page+1)+')\">Trang sau &gt;</button>';"
    "h+='</div>';}"
    "$('body').innerHTML=h;"
    "if(!unreg){const fb=$('cardFilterBtn');if(fb)fb.onclick=()=>{cardFilter.id=($('cardIdFilter')?$('cardIdFilter').value:'').trim();renderCards(false,1);};"
    "const fc=$('cardFilterClr');if(fc)fc.onclick=()=>{cardFilter.id='';renderCards(false,1);};"
    "const fi=$('cardIdFilter');if(fi)fi.onkeydown=e=>{if(e.key==='Enter'){cardFilter.id=fi.value.trim();renderCards(false,1);}};}"
    "document.querySelectorAll('button[data-u]').forEach(b=>{"
    "b.onclick=()=>{const u=b.getAttribute('data-u');"
    "if(b.classList.contains('red')){"
    "if(!confirm('Xóa thẻ và toàn bộ dữ liệu của thẻ '+u+'?'))return;"
    "cardDelReq(sec,unreg,u);"
    "}else{"
    "const nv=b.getAttribute('data-n')||'',iv=b.getAttribute('data-i')||'';"
    "$('regName').value=nv;$('regId').value=iv;"
    "$('regTitle').innerText=(nv?'Chỉnh sửa':'Đăng ký')+' nhân viên';"
    "const ov=$('regOv');ov.classList.add('show');$('regName').focus();"
    "$('regOk').onclick=()=>{const n=$('regName').value.trim(),i=$('regId').value.trim();"
    "if(!n){alert('Tên nhân viên không được để trống!');return;}"
    "if(!i){alert('Mã số nhân viên không được để trống!');return;}"
    "const p=new URLSearchParams();p.set('uid',u);p.set('name',n);p.set('id',i);"
    "api(sec,'/api/cards',{method:'POST',body:p}).then(r=>r.json()).then(d=>{"
    "if(d.ok){ov.classList.remove('show');renderCards(unreg);}else alert(d.error||'Lưu thất bại');});};"
    "$('regCancel').onclick=()=>ov.classList.remove('show');"
    "}"
    "};});"
    "document.querySelectorAll('button[data-del-unreg]').forEach(b=>{"
    "b.onclick=()=>{const u=b.getAttribute('data-del-unreg');"
    "if(!confirm('Xóa thẻ lạ '+u+'?'))return;"
    "cardDelReq(sec,unreg,u);"
    "};});});}"
    "function renderPin(){"
    "let h='<div style=\"max-width:400px\">'"
    "+'<label>Mật khẩu PIN hiện tại</label><input type=password id=op maxlength=15 placeholder=\"Nhập PIN cũ...\">'"
    "+'<label>Mật khẩu PIN mới</label><input type=password id=np maxlength=15 placeholder=\"Nhập PIN mới (tối đa 15 chữ số)...\">'"
    "+'<p class=msg id=pmsg></p>'"
    "+'<button class=btn id=psv style=\"margin-top:12px\">Lưu PIN mới</button>'"
    "+'</div>';"
    "$('body').innerHTML=h;"
    "$('psv').onclick=()=>{const b=new URLSearchParams();b.set('old_pin',$('op').value);b.set('new_pin',$('np').value);"
    "api('pin','/api/pin_change',{method:'POST',body:b}).then(r=>r.json()).then(d=>{"
    "$('pmsg').innerText=d.ok?'Đổi mã PIN thành công!':' '+(d.error||'Có lỗi xảy ra');$('pmsg').className='msg '+(d.ok?'success':'error');if(d.ok){$('op').value='';$('np').value='';} });};}"
    "function renderLcdData(d){"
    "const curNm=(d.options||[]).find(o=>o.id===d.current);"
    "const curName=curNm?curNm.name:(d.current_name||('ID '+d.current));"
    "const bv=d.brand||'MEBIECO';"
    "let h='<div class=home-guide style=\"margin-bottom:20px\"><div>🖥️</div><div><h4>Chọn loại màn 2.8 inch</h4><p>Sau khi đổi màn hoặc thay module LCD, chọn đúng profile rồi bấm Lưu. Thiết bị sẽ khởi động lại để áp dụng.</p></div></div>';"
    "h+='<div style=\"max-width:520px\">';"
    "(d.options||[]).forEach(o=>{"
    "const ck=o.id===d.current?' checked':'';"
    "const selClass=o.id===d.current?' sel':'';"
    "const now=o.id===d.current?'<span class=\"status-badge success\" style=\"margin-left:8px\">Hiện tại</span>':'';"
    "h+='<label class=\"lcd-opt'+selClass+'\">'"
    "+'<input type=radio name=lcdprof value=\"'+o.id+'\" data-pname=\"'+o.name.replace(/\"/g,'')+'\"'+ck+'>'"
    "+'<span style=\"flex:1;min-width:0\"><b style=\"color:#fff\">'+o.name+'</b>'+now"
    "+'<div class=lcd-swap style=\"display:none;margin-top:8px;font-size:13px;line-height:1.5\"></div></span></label>';});"
    "h+='<p class=msg id=lcdmsg></p>';"
    "h+='<button class=\"btn green\" id=lcdsave style=\"margin-top:12px\">Lưu & khởi động lại</button></div>';"
    "h+='<hr style=\"border-color:rgba(255,255,255,0.08);margin:28px 0\">';"
    "h+='<div style=\"max-width:520px\"><h4 style=\"color:#fff;margin-bottom:12px;font-size:15px\">🏷️ Chữ thương hiệu trên màn chờ</h4>';"
    "h+='<p style=\"font-size:13px;color:#9ca3af;margin-bottom:14px\">Chỉnh sửa dòng chữ hiển thị bên dưới tiêu đề \"MÁY CHẤM CÔNG\" (tối đa 15 ký tự). Yêu cầu nhập PIN khi lưu.</p>';"
    "h+='<input id=brandInp maxlength=15 value=\"'+bv+'\" placeholder=\"VD: MEBIECO...\" style=\"max-width:320px\">';"
    "h+='<p class=msg id=brandmsg></p>';"
    "h+='<button class=btn id=brandsave style=\"margin-top:8px\">Lưu chữ thương hiệu</button></div>';"
    "const vol=(d.vol!=null)?d.vol:100;"
    "h+='<hr style=\"border-color:rgba(255,255,255,0.08);margin:28px 0\">';"
    "h+='<div style=\"max-width:520px\"><h4 style=\"color:#fff;margin-bottom:12px;font-size:15px\">🔊 Âm lượng thiết bị</h4>';"
    "h+='<div style=\"display:flex;align-items:center;gap:16px\">';"
    "h+='<input type=range id=volSlider min=0 max=100 value=\"'+vol+'\" style=\"flex:1;padding:0\">';"
    "h+='<span id=volVal style=\"font-weight:bold;color:#34d399;width:40px\">'+vol+'%</span>';"
    "h+='</div><p class=msg id=volmsg></p></div>';"
    "$('body').innerHTML=h;"
    "function lcdSwapHint(){"
    "const sel=document.querySelector('input[name=lcdprof]:checked');"
    "document.querySelectorAll('.lcd-opt').forEach(l=>{l.classList.remove('sel');});"
    "document.querySelectorAll('.lcd-swap').forEach(el=>{el.style.display='none';el.innerHTML='';});"
    "if(!sel)return;"
    "const row=sel.closest('.lcd-opt');"
    "if(row){row.classList.add('sel');}"
    "const hint=row&&row.querySelector('.lcd-swap');"
    "const newName=sel.getAttribute('data-pname')||'';"
    "if(!hint)return;"
    "if(String(sel.value)===String(d.current)){"
    "hint.innerHTML='<span style=\"color:#34d399\">Đang dùng — không đổi</span>';"
    "}else{"
    "hint.innerHTML='<span style=\"color:#9ca3af\">Màn cũ:</span> <b style=\"color:#fbbf24\">'+curName+'</b> '"
    "+'<span style=\"color:#9ca3af\">→ Màn mới:</span> <b style=\"color:#34d399\">'+newName+'</b>';}"
    "hint.style.display='block';}"
    "document.querySelectorAll('input[name=lcdprof]').forEach(r=>{r.onchange=lcdSwapHint;});"
    "lcdSwapHint();"
    "$('lcdsave').onclick=()=>{const sel=document.querySelector('input[name=lcdprof]:checked');"
    "if(!sel){alert('Chọn một loại màn hình');return;}"
    "const newNm=sel.getAttribute('data-pname')||sel.value;"
    "const cf=String(sel.value)===String(d.current)"
    "?('Giữ màn hiện tại: '+curName+'. Khởi động lại?')"
    ":('Đổi màn:\\n• Cũ: '+curName+'\\n• Mới: '+newNm+'\\n\\nLưu và khởi động lại thiết bị?');"
    "if(!confirm(cf))return;"
    "const b='panel='+encodeURIComponent(sel.value);"
    "$('lcdsave').disabled=true;$('lcdmsg').innerText='Đang lưu...';$('lcdmsg').className='msg';"
    "api('','/api/lcd_panel',{method:'POST',body:b}).then(r=>r.json()).then(x=>{"
    "if(x.ok&&x.reboot){$('lcdmsg').innerText=x.msg||'Đang khởi động lại...';$('lcdmsg').className='msg success';"
    "setTimeout(()=>{location.reload();},8000);return;}"
    "$('lcdsave').disabled=false;"
    "$('lcdmsg').innerText=x.ok?(x.msg||'Đã lưu'):(x.error||'Lỗi');$('lcdmsg').className='msg '+(x.ok?'success':'error');"
    "}).catch(()=>{$('lcdsave').disabled=false;$('lcdmsg').innerText='Lỗi kết nối';$('lcdmsg').className='msg error';});};"
    "$('brandsave').onclick=()=>{"
    "need('lcd',()=>{"
    "const v=$('brandInp').value;"
    "const b=new URLSearchParams();b.set('brand',v);"
    "$('brandsave').disabled=true;$('brandmsg').innerText='Đang lưu...';$('brandmsg').className='msg';"
    "api('lcd','/api/brand',{method:'POST',body:b}).then(r=>r.json()).then(x=>{"
    "$('brandsave').disabled=false;"
    "$('brandmsg').innerText=x.ok?'Đã lưu — màn LCD cập nhật ngay!':(x.error||'Lỗi');"
    "$('brandmsg').className='msg '+(x.ok?'success':'error');if(x.ok)tcClr('screen');"
    "}).catch(()=>{$('brandsave').disabled=false;$('brandmsg').innerText='Lỗi kết nối';$('brandmsg').className='msg error';});});};"
    "const vs=$('volSlider');if(vs){vs.oninput=()=>{$('volVal').innerText=vs.value+'%';};"
    "vs.onchange=()=>{need('lcd',()=>{const v=vs.value;"
    "api('lcd','/api/vol',{method:'POST',body:'vol='+encodeURIComponent(v)}).then(r=>r.json()).then(rx=>{"
    "if(rx.ok){$('volmsg').innerHTML='<span style=\"color:#10b981\">Đã lưu '+v+'%</span>';tcClr('screen');}"
    "else $('volmsg').innerHTML='<span style=\"color:#ef4444\">Lỗi lưu: '+(rx.error||'Cần mã PIN')+'</span>';}).catch(e=>{$('volmsg').innerHTML='<span style=\"color:#ef4444\">Lỗi: '+e.message+'</span>';});});};}"
    "}"
    "function renderLcd(){"
    "const c=tcGet('screen');if(c){renderLcdData(c);return;}"
    "$('body').innerHTML='<p class=msg>Đang tải cấu hình màn hình...</p>';"
    "api('','/api/screen').then(r=>r.json()).catch(()=>({})).then(d=>{"
    "if(!d||!d.ok){$('body').innerHTML='<p class=msg error>Không tải được cấu hình màn hình</p>';return;}"
    "tcSet('screen',d);renderLcdData(d);});}"
    "function go(sec){cur=sec;$('title').innerText=T[sec]||sec;"
    "document.querySelectorAll('.nav a').forEach(a=>a.classList.toggle('on',a.dataset.sec===sec));"
    "$('sideMenu').classList.remove('show');"
    "$('mOv').classList.remove('show');"
    "if(sec==='home'){renderHome();return;}"
    "if(sec==='lcd'){renderLcd();return;}"
    "need(sec,()=>{if(sec==='wifi')renderWifi();else if(sec==='azure')renderAzure();"
    "else if(sec==='log')renderLog();else if(sec==='cards')renderCards(false);"
    "else if(sec==='register')renderCards(true);else if(sec==='pin')renderPin();});}"
    "function checkLogin(){"
    "const li=!!tokens['admin'];"
    "$('appLayout').style.display=li?'flex':'none';"
    "$('loginPage').style.display=li?'none':'flex';"
    "$('mHdr').style.display=li?'':'none';"
    "if(!li){$('mOv').classList.remove('show');$('sideMenu').classList.remove('show');}"
    "if(li)go(cur);"
    "}"
    "$('mBtn').onclick=()=>{"
    "$('sideMenu').classList.toggle('show');"
    "$('mOv').classList.toggle('show');"
    "};"
    "$('mOv').onclick=()=>{"
    "$('sideMenu').classList.remove('show');"
    "$('mOv').classList.remove('show');"
    "};"
    "document.querySelectorAll('.nav a').forEach(a=>a.onclick=e=>{e.preventDefault();go(a.dataset.sec);});"
    "$('logoutBtn').onclick=e=>{e.preventDefault();if(confirm('Đăng xuất khỏi hệ thống?')){tokens={};sessionStorage.removeItem('portal_tokens');tcClr();checkLogin();}};"
    "$('lbtn').onclick=()=>{const u=$('luser').value,p=$('lpass').value;const btn=$('lbtn');"
    "btn.disabled=true;btn.innerHTML='<span class=spinner></span>';"
    "fetch('/api/admin_login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'user='+encodeURIComponent(u)+'&pass='+encodeURIComponent(p)})"
    ".then(r=>r.text().then(t=>{let d;try{d=JSON.parse(t);}catch(e){throw new Error(t||('HTTP '+r.status));}return d;}))"
    ".then(d=>{"
    "btn.disabled=false;btn.innerText='Đăng nhập';"
    "if(d.ok){"
    "tokens['admin']=d.token;"
    "sessionStorage.setItem('portal_tokens',JSON.stringify(tokens));"
    "$('lmsg').innerText='';$('lmsg').className='msg';"
    "checkLogin();"
    "}else{"
    "$('lmsg').innerText=d.error||'Lỗi đăng nhập';$('lmsg').className='msg error';"
    "}"
    "}).catch(e=>{btn.disabled=false;btn.innerText='Đăng nhập';$('lmsg').innerText=e.message||'Không thể kết nối máy chủ';$('lmsg').className='msg error';});};"
    "function doLogin(){if(!$('lbtn').disabled)$('lbtn').click();}"
    "$('lpass').onkeydown=e=>{if(e.key==='Enter')doLogin();};"
    "$('luser').onkeydown=e=>{if(e.key==='Enter')doLogin();};"
    "checkLogin();"
    "</script></body></html>";

esp_err_t portal_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self' 'unsafe-inline' 'unsafe-eval'; img-src 'self' data:;");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

void portal_web_register_handlers(httpd_handle_t server)
{
    if (!server) {
        return;
    }
    httpd_uri_t u_admin_login = { .uri = "/api/admin_login", .method = HTTP_POST, .handler = portal_admin_login_post_handler };
    httpd_uri_t u_unlock = { .uri = "/api/unlock", .method = HTTP_POST, .handler = portal_unlock_post_handler };
    httpd_uri_t u_cards = { .uri = "/api/cards", .method = HTTP_GET, .handler = api_cards_get_handler };
    httpd_uri_t u_cards_p = { .uri = "/api/cards", .method = HTTP_POST, .handler = api_cards_post_handler };
    httpd_uri_t u_cards_d = { .uri = "/api/cards/del", .method = HTTP_POST, .handler = api_cards_del_handler };
    httpd_uri_t u_log = { .uri = "/api/log", .method = HTTP_GET, .handler = api_log_get_handler };
    httpd_uri_t u_log_sync = { .uri = "/api/log_sync", .method = HTTP_POST, .handler = api_log_sync_post_handler };
    httpd_uri_t u_az = { .uri = "/api/azure", .method = HTTP_GET, .handler = api_azure_get_handler };
    httpd_uri_t u_st = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler };
    httpd_uri_t u_pin = { .uri = "/api/pin_change", .method = HTTP_POST, .handler = api_pin_change_post_handler };
    httpd_uri_t u_brand_g = { .uri = "/api/brand", .method = HTTP_GET, .handler = api_brand_get_handler };
    httpd_uri_t u_brand_p = { .uri = "/api/brand", .method = HTTP_POST, .handler = api_brand_post_handler };
    httpd_uri_t u_lcd_g = { .uri = "/api/lcd_panel", .method = HTTP_GET, .handler = api_lcd_panel_get_handler };
    httpd_uri_t u_lcd_p = { .uri = "/api/lcd_panel", .method = HTTP_POST, .handler = api_lcd_panel_post_handler };
    httpd_uri_t u_icon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = portal_favicon_get_handler };
    httpd_uri_t u_vol_g = {.uri = "/api/vol", .method = HTTP_GET, .handler = api_vol_get_handler, .user_ctx = NULL};
    httpd_uri_t u_vol_p = {.uri = "/api/vol", .method = HTTP_POST, .handler = api_vol_post_handler, .user_ctx = NULL};
    httpd_uri_t u_screen = {.uri = "/api/screen", .method = HTTP_GET, .handler = api_screen_get_handler, .user_ctx = NULL};

    httpd_register_uri_handler(server, &u_admin_login);
    httpd_register_uri_handler(server, &u_unlock);
    httpd_register_uri_handler(server, &u_cards);
    httpd_register_uri_handler(server, &u_cards_p);
    httpd_register_uri_handler(server, &u_cards_d);
    httpd_register_uri_handler(server, &u_log);
    httpd_register_uri_handler(server, &u_log_sync);
    httpd_register_uri_handler(server, &u_az);
    httpd_register_uri_handler(server, &u_st);
    httpd_register_uri_handler(server, &u_pin);
    httpd_register_uri_handler(server, &u_brand_g);
    httpd_register_uri_handler(server, &u_brand_p);
    httpd_register_uri_handler(server, &u_lcd_g);
    httpd_register_uri_handler(server, &u_lcd_p);
    httpd_register_uri_handler(server, &u_icon);
    httpd_register_uri_handler(server, &u_vol_g);
    httpd_register_uri_handler(server, &u_vol_p);
    httpd_register_uri_handler(server, &u_screen);
    ESP_LOGI(TAG, "Portal web: menu + PIN theo muc");
}
