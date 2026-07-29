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
#include "attendance_day.h"
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
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <stdarg.h>


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

#define WEB_LOG_BUF_SIZE (64 * 1024)
static char *s_web_log_buf = NULL;
static size_t s_web_log_head = 0;
static size_t s_web_log_tail = 0;
static size_t s_web_log_len = 0;
static volatile bool s_web_log_busy = false;
static vprintf_like_t s_old_vprintf = NULL;

static int web_log_vprintf(const char *fmt, va_list args)
{
    if (s_web_log_busy) {
        return 0;
    }
    s_web_log_busy = true;

    int ret = 0;
    if (s_old_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_old_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    if (s_web_log_buf) {
        char temp_buf[128];
        va_list args_copy2;
        va_copy(args_copy2, args);
        int formatted_len = vsnprintf(temp_buf, sizeof(temp_buf), fmt, args_copy2);
        va_end(args_copy2);

        if (formatted_len > 0) {
            if (formatted_len >= sizeof(temp_buf)) {
                formatted_len = sizeof(temp_buf) - 1;
            }
            for (int i = 0; i < formatted_len; i++) {
                s_web_log_buf[s_web_log_head] = temp_buf[i];
                s_web_log_head = (s_web_log_head + 1) % WEB_LOG_BUF_SIZE;
                if (s_web_log_len < WEB_LOG_BUF_SIZE) {
                    s_web_log_len++;
                } else {
                    s_web_log_tail = (s_web_log_tail + 1) % WEB_LOG_BUF_SIZE;
                }
            }
        }
    }

    s_web_log_busy = false;
    return ret;
}

static esp_err_t api_terminal_log_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau dang nhap\"}");
    }
    if (!s_web_log_buf) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Log buffer not initialized");
    }
    
    s_web_log_busy = true;
    size_t len = s_web_log_len;
    char *temp = malloc(len + 1);
    if (!temp) {
        s_web_log_busy = false;
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Out of memory");
    }
    
    size_t idx = 0;
    size_t tail = s_web_log_tail;
    size_t l = len;
    while (l > 0) {
        temp[idx++] = s_web_log_buf[tail];
        tail = (tail + 1) % WEB_LOG_BUF_SIZE;
        l--;
    }
    temp[idx] = '\0';
    s_web_log_busy = false;
    
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    esp_err_t err = httpd_resp_send(req, temp, idx);
    free(temp);
    return err;
}

static esp_err_t api_terminal_log_clear_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau dang nhap\"}");
    }
    if (s_web_log_buf) {
        s_web_log_busy = true;
        s_web_log_head = 0;
        s_web_log_tail = 0;
        s_web_log_len = 0;
        s_web_log_busy = false;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_hardware_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau dang nhap\"}");
    }

    int64_t uptime_sec = esp_timer_get_time() / 1000000;
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    uint32_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    uint32_t flash_size = 0;
    (void)esp_flash_get_size(NULL, &flash_size);

    const esp_partition_t *run_part = esp_ota_get_running_partition();
    const char *part_label = run_part ? run_part->label : "N/A";
    uint32_t part_size = run_part ? run_part->size : 0;
    uint32_t part_address = run_part ? run_part->address : 0;

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const char *chip_model_str = "ESP32-S3";
    if (chip_info.model == CHIP_ESP32) chip_model_str = "ESP32";
    else if (chip_info.model == CHIP_ESP32S2) chip_model_str = "ESP32-S2";
    else if (chip_info.model == CHIP_ESP32S3) chip_model_str = "ESP32-S3";
    else if (chip_info.model == CHIP_ESP32C3) chip_model_str = "ESP32-C3";

    esp_reset_reason_t reason = esp_reset_reason();
    const char *reason_str = "UNKNOWN";
    switch (reason) {
        case ESP_RST_POWERON:  reason_str = "POWERON_RESET"; break;
        case ESP_RST_EXT:      reason_str = "EXTERNAL_RESET"; break;
        case ESP_RST_SW:       reason_str = "SOFTWARE_RESET"; break;
        case ESP_RST_PANIC:    reason_str = "PANIC_RESET"; break;
        case ESP_RST_INT_WDT:  reason_str = "INT_WDT_RESET"; break;
        case ESP_RST_TASK_WDT: reason_str = "TASK_WDT_RESET"; break;
        case ESP_RST_WDT:      reason_str = "WDT_RESET"; break;
        case ESP_RST_DEEPSLEEP:reason_str = "DEEPSLEEP_RESET"; break;
        case ESP_RST_BROWNOUT: reason_str = "BROWNOUT_RESET"; break;
        case ESP_RST_SDIO:     reason_str = "SDIO_RESET"; break;
        default: break;
    }

    char resp[768];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"uptime_sec\":%lld,\"free_heap\":%u,\"min_free_heap\":%u,\"heap_total\":%u,"
             "\"internal_free\":%u,\"internal_min\":%u,\"internal_total\":%u,"
             "\"spiram_free\":%u,\"spiram_min\":%u,\"spiram_total\":%u,\"dma_largest\":%u,"
             "\"flash_size\":%u,\"partition_label\":\"%s\",\"partition_size\":%u,\"partition_address\":%u,"
             "\"chip_model\":\"%s\",\"cores\":%d,\"revision\":%d,\"reset_reason\":\"%s\",\"idf_ver\":\"%s\"}",
             (long long)uptime_sec, (unsigned)free_heap, (unsigned)min_free_heap, (unsigned)heap_total,
             (unsigned)int_free, (unsigned)int_min, (unsigned)int_total,
             (unsigned)psram_free, (unsigned)psram_min, (unsigned)psram_total, (unsigned)dma_largest,
             (unsigned)flash_size, part_label, (unsigned)part_size, (unsigned)part_address,
             chip_model_str, (int)chip_info.cores, (int)chip_info.revision, reason_str, esp_get_idf_version());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_sendstr(req, resp);
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

    CardProfileEntry_t *entries = heap_caps_malloc(limit * sizeof(CardProfileEntry_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) {
        entries = malloc(limit * sizeof(CardProfileEntry_t));
    }
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

static esp_err_t api_attendance_overview_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau dang nhap\"}");
    }
    return attendance_day_send_overview_json(req);
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    if (!portal_auth_section(req, "admin")) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Yeu cau dang nhap\"}");
    }
    char resp[480];
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
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"wifi_saved\":%d,\"wifi_status\":\"%s\",\"sd\":%s,\"time_ok\":%s,\"time\":\"%s\","
             "\"lcd_panel\":%u,\"lcd_panel_name\":\"%s\",\"azure_connected\":%s,"
             "\"fw_ver\":\"%s\",\"fw_date\":\"%s %s\",\"fw_part\":\"%s\"}",
             cnt, stt, sd_card_is_mounted() ? "true" : "false", time_ok ? "true" : "false", time_str,
             (unsigned)lcd_panel_get_id(), lcd_p ? lcd_p->name : "",
             az_conn ? "true" : "false",
             app && app->version[0] ? app->version : "?",
             app ? app->date : "?", app ? app->time : "?",
             run ? run->label : "?");
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

/* UI: portal.html gzip trong flash. Cap nhat: sua portal.html roi chay: python tools/pack_portal.py */
extern const uint8_t portal_html_gz_start[] asm("_binary_portal_html_gz_start");
extern const uint8_t portal_html_gz_end[] asm("_binary_portal_html_gz_end");

esp_err_t portal_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self' 'unsafe-inline' 'unsafe-eval'; img-src 'self' data:;");

    const size_t total = (size_t)(portal_html_gz_end - portal_html_gz_start);
    /* SoftAP: gui gzip ~11KB theo chunk de tranh EAGAIN */
    const size_t chunk = 1024;
    const char *p = (const char *)portal_html_gz_start;
    for (size_t off = 0; off < total; off += chunk) {
        size_t n = total - off;
        if (n > chunk) {
            n = chunk;
        }
        esp_err_t e = httpd_resp_send_chunk(req, p + off, n);
        if (e != ESP_OK) {
            (void)httpd_resp_send_chunk(req, NULL, 0);
            return e;
        }
        if ((off / chunk) % 4u == 3u) {
            vTaskDelay(1);
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

void portal_web_register_handlers(httpd_handle_t server)
{
    if (!server) {
        return;
    }

    if (!s_web_log_buf) {
        s_web_log_buf = heap_caps_malloc(WEB_LOG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_web_log_buf) {
            s_web_log_buf = malloc(WEB_LOG_BUF_SIZE);
        }
        if (s_web_log_buf) {
            s_web_log_head = 0;
            s_web_log_tail = 0;
            s_web_log_len = 0;
            s_old_vprintf = esp_log_set_vprintf(web_log_vprintf);
        }
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
    httpd_uri_t u_att = { .uri = "/api/attendance_overview", .method = HTTP_GET, .handler = api_attendance_overview_get_handler };
    httpd_uri_t u_pin = { .uri = "/api/pin_change", .method = HTTP_POST, .handler = api_pin_change_post_handler };
    httpd_uri_t u_brand_g = { .uri = "/api/brand", .method = HTTP_GET, .handler = api_brand_get_handler };
    httpd_uri_t u_brand_p = { .uri = "/api/brand", .method = HTTP_POST, .handler = api_brand_post_handler };
    httpd_uri_t u_lcd_g = { .uri = "/api/lcd_panel", .method = HTTP_GET, .handler = api_lcd_panel_get_handler };
    httpd_uri_t u_lcd_p = { .uri = "/api/lcd_panel", .method = HTTP_POST, .handler = api_lcd_panel_post_handler };
    httpd_uri_t u_icon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = portal_favicon_get_handler };
    httpd_uri_t u_vol_g = {.uri = "/api/vol", .method = HTTP_GET, .handler = api_vol_get_handler, .user_ctx = NULL};
    httpd_uri_t u_vol_p = {.uri = "/api/vol", .method = HTTP_POST, .handler = api_vol_post_handler, .user_ctx = NULL};
    httpd_uri_t u_screen = {.uri = "/api/screen", .method = HTTP_GET, .handler = api_screen_get_handler, .user_ctx = NULL};
    httpd_uri_t u_term_log = { .uri = "/api/terminal_log", .method = HTTP_GET, .handler = api_terminal_log_get_handler };
    httpd_uri_t u_term_log_clr = { .uri = "/api/terminal_log/clear", .method = HTTP_POST, .handler = api_terminal_log_clear_handler };
    httpd_uri_t u_hw = { .uri = "/api/hardware", .method = HTTP_GET, .handler = api_hardware_get_handler };

    httpd_register_uri_handler(server, &u_admin_login);
    httpd_register_uri_handler(server, &u_unlock);
    httpd_register_uri_handler(server, &u_cards);
    httpd_register_uri_handler(server, &u_cards_p);
    httpd_register_uri_handler(server, &u_cards_d);
    httpd_register_uri_handler(server, &u_log);
    httpd_register_uri_handler(server, &u_log_sync);
    httpd_register_uri_handler(server, &u_az);
    httpd_register_uri_handler(server, &u_st);
    httpd_register_uri_handler(server, &u_att);
    httpd_register_uri_handler(server, &u_pin);
    httpd_register_uri_handler(server, &u_brand_g);
    httpd_register_uri_handler(server, &u_brand_p);
    httpd_register_uri_handler(server, &u_lcd_g);
    httpd_register_uri_handler(server, &u_lcd_p);
    httpd_register_uri_handler(server, &u_icon);
    httpd_register_uri_handler(server, &u_vol_g);
    httpd_register_uri_handler(server, &u_vol_p);
    httpd_register_uri_handler(server, &u_screen);
    httpd_register_uri_handler(server, &u_term_log);
    httpd_register_uri_handler(server, &u_term_log_clr);
    httpd_register_uri_handler(server, &u_hw);
    ESP_LOGI(TAG, "Portal web: menu + PIN theo muc");
}
