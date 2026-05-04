#include "scan_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "board_pins.h"
#include "lcd_ui.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sd_card.h"
#include <sys/stat.h>

static const char *TAG = "scan_log";

/** Chi giu dong log trong khoang ngay nay (gio UTC + offset hien thi), tranh day SD. */
#define SCAN_LOG_KEEP_DAYS 60
#define SCAN_LOG_KEEP_SEC ((time_t)(SCAN_LOG_KEEP_DAYS * 86400))
/** Khong quet lai toan bo file qua thuong xuyen (ms moi lan quet). */
#define SCAN_LOG_TRIM_INTERVAL_SEC (6 * 3600)
/** Neu file vuot ngay thich — cat bat ky luc nao. */
#define SCAN_LOG_TRIM_FORCE_BYTES ((off_t)(1536 * 1024))

static time_t s_scan_log_last_trim_sec;

/** Hang doi tam khi khong ghi duoc SD — luu tren NVS, flush FIFO khi SD mount. */
#define SCAN_PEND_NVS_NS "scan_pend"
#define SCAN_PEND_NVS_KEY "q"
#define SCAN_PEND_MAGIC 0x51455244u /* 'QERD' v1 queue */
#define SCAN_PEND_MAX 32

typedef struct {
    char uid[24];
    char name[48];
    char id[48];
    int32_t registered;
    int64_t utc_sec;
} scan_pend_rec_t;

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t _pad;
    scan_pend_rec_t rec[SCAN_PEND_MAX];
} scan_pend_blob_t;

/**
 * Blob ~4.2KB — khong dat tren stack (task main chi ~3584 byte mac dinh).
 * Dung buffer tinh + mutex vi rfid_task va main deu goi enqueue/flush.
 */
static scan_pend_blob_t s_pend_blob;
static SemaphoreHandle_t s_pend_mtx;

static void pend_mtx_take(void)
{
    if (s_pend_mtx == NULL) {
        s_pend_mtx = xSemaphoreCreateMutex();
    }
    if (s_pend_mtx) {
        (void)xSemaphoreTake(s_pend_mtx, portMAX_DELAY);
    }
}

static void pend_mtx_give(void)
{
    if (s_pend_mtx) {
        (void)xSemaphoreGive(s_pend_mtx);
    }
}

void scan_log_wall_tm(time_t utc, struct tm *out)
{
    time_t wall = utc + (time_t)BOARD_LOCAL_UTC_OFFSET_SEC;
    gmtime_r(&wall, out);
}

static void sanitize_field(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in) {
        in = "";
    }
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '|' || c == '\n' || c == '\r' || c == '\t') {
            out[j++] = ' ';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

static void format_ts_from_utc(time_t utc, char *ts, size_t ts_len)
{
    struct tm ti;
    scan_log_wall_tm(utc, &ti);
    if (ti.tm_year >= (2020 - 1900)) {
        snprintf(ts, ts_len, "%04d-%02d-%02dT%02d:%02d:%02d", (int)(ti.tm_year + 1900),
                 (int)(ti.tm_mon + 1), (int)ti.tm_mday, (int)ti.tm_hour, (int)ti.tm_min, (int)ti.tm_sec);
    } else {
        snprintf(ts, ts_len, "no-ntp");
    }
}

/** Giu dong neu thoi gian (cot 1) >= nguong cat; "no-ntp" / sai dinh dang: giu de khong mat du lieu. */
static bool log_line_kept(const char *line, const char *cutoff_ts)
{
    const char *p = strchr(line, '|');
    size_t n = p ? (size_t)(p - line) : strnlen(line, 48);
    if (n == 0) {
        return true;
    }
    if (n > 39) {
        n = 39;
    }
    char first[40];
    memcpy(first, line, n);
    first[n] = '\0';
    if (n > 0 && first[n - 1] == '\r') {
        first[--n] = '\0';
    }
    if (strncmp(first, "no-ntp", 6) == 0) {
        return true;
    }
    if (n < 19) {
        return true;
    }
    return strcmp(first, cutoff_ts) >= 0;
}

static void scan_log_do_trim(bool bypass_rate_limit)
{
    if (!sd_card_is_mounted()) {
        return;
    }

    const char *path = BOARD_SD_RFID_LOG_PATH;
    struct stat st;
    bool force = (stat(path, &st) == 0 && st.st_size >= SCAN_LOG_TRIM_FORCE_BYTES);

    time_t now = time(NULL);
    if (!force && !bypass_rate_limit && s_scan_log_last_trim_sec != 0 &&
        (now - s_scan_log_last_trim_sec) < (time_t)SCAN_LOG_TRIM_INTERVAL_SEC) {
        return;
    }

    struct tm ti_now;
    scan_log_wall_tm(now, &ti_now);
    if (ti_now.tm_year < (2020 - 1900)) {
        return;
    }

    time_t cutoff = now - SCAN_LOG_KEEP_SEC;
    char cutoff_ts[48];
    format_ts_from_utc(cutoff, cutoff_ts, sizeof(cutoff_ts));
    if (strncmp(cutoff_ts, "no-ntp", 6) == 0) {
        return;
    }

    sd_card_lock();
    FILE *in = fopen(path, "r");
    if (!in) {
        sd_card_unlock();
        return;
    }

    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s/rfid_log_trim.tmp", BOARD_SD_MOUNT_POINT);
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        sd_card_unlock();
        ESP_LOGW(TAG, "trim: fopen tmp: %s", strerror(errno));
        return;
    }

    char buf[512];
    size_t dropped = 0;
    size_t kept    = 0;
    while (fgets(buf, sizeof(buf), in)) {
        if (log_line_kept(buf, cutoff_ts)) {
            if (fputs(buf, out) == EOF) {
                ESP_LOGW(TAG, "trim: ghi tmp that bai");
                fclose(in);
                fclose(out);
                (void)remove(tmp_path);
                sd_card_unlock();
                return;
            }
            kept++;
        } else {
            dropped++;
        }
    }
    fclose(in);
    (void)fflush(out);
    {
        int fd = fileno(out);
        if (fd >= 0) {
            (void)fsync(fd);
        }
    }
    fclose(out);

    if (dropped == 0) {
        (void)remove(tmp_path);
        s_scan_log_last_trim_sec = now;
        sd_card_unlock();
        return;
    }

    if (remove(path) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "trim: remove %s: %s", path, strerror(errno));
        (void)remove(tmp_path);
        sd_card_unlock();
        return;
    }
    if (rename(tmp_path, path) != 0) {
        ESP_LOGW(TAG, "trim: rename: %s", strerror(errno));
        (void)remove(tmp_path);
        sd_card_unlock();
        return;
    }
    s_scan_log_last_trim_sec = now;
    sd_card_unlock();
    ESP_LOGI(TAG, "Giu toi da %d ngay: xoa %u dong, giu %u dong", SCAN_LOG_KEEP_DAYS, (unsigned)dropped,
             (unsigned)kept);
    lcd_ui_invalidate_log_cache();
}

/** Ghi mot dong vao CSV; tra ve true neu ghi va fsync ok. */
static bool write_one_csv_line(time_t event_utc, const char *uid_hex, const char *name, const char *id,
                               int registered)
{
    char ts[40];
    format_ts_from_utc(event_utc, ts, sizeof(ts));

    char uid_s[40];
    char ns[80];
    char ids[80];
    sanitize_field(uid_hex, uid_s, sizeof(uid_s));
    sanitize_field(name ? name : "", ns, sizeof(ns));
    sanitize_field(id ? id : "", ids, sizeof(ids));

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "a");
    if (!fp) {
        sd_card_unlock();
        ESP_LOGW(TAG, "fopen append %s: %s", BOARD_SD_RFID_LOG_PATH, strerror(errno));
        return false;
    }

    if (fprintf(fp, "%s|%s|%s|%s|%d\n", ts, uid_s, ns, ids, registered) < 0) {
        ESP_LOGW(TAG, "fprintf log: %s", strerror(errno));
        fclose(fp);
        sd_card_unlock();
        return false;
    }
    fflush(fp);
    {
        int fd = fileno(fp);
        if (fd >= 0) {
            (void)fsync(fd);
        }
    }
    fclose(fp);
    sd_card_unlock();
    ESP_LOGI(TAG, "Ghi nhat ky: %s UID=%s", ts, uid_s);
    scan_log_do_trim(false);
    return true;
}

static esp_err_t pend_load(void)
{
    memset(&s_pend_blob, 0, sizeof(s_pend_blob));
    s_pend_blob.magic = SCAN_PEND_MAGIC;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SCAN_PEND_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(s_pend_blob);
    err = nvs_get_blob(h, SCAN_PEND_NVS_KEY, &s_pend_blob, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_pend_blob, 0, sizeof(s_pend_blob));
        s_pend_blob.magic = SCAN_PEND_MAGIC;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (s_pend_blob.magic != SCAN_PEND_MAGIC || s_pend_blob.count > SCAN_PEND_MAX) {
        memset(&s_pend_blob, 0, sizeof(s_pend_blob));
        s_pend_blob.magic = SCAN_PEND_MAGIC;
    }
    return ESP_OK;
}

static esp_err_t pend_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SCAN_PEND_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    s_pend_blob.magic = SCAN_PEND_MAGIC;
    err = nvs_set_blob(h, SCAN_PEND_NVS_KEY, &s_pend_blob, sizeof(s_pend_blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void pending_enqueue(const char *uid_hex, const char *name, const char *id, int registered)
{
    if (!uid_hex || !uid_hex[0]) {
        return;
    }

    pend_mtx_take();
    esp_err_t lr = pend_load();
    if (lr != ESP_OK) {
        ESP_LOGW(TAG, "pending load: %s", esp_err_to_name(lr));
        pend_mtx_give();
        return;
    }

    if (s_pend_blob.count >= SCAN_PEND_MAX) {
        memmove(&s_pend_blob.rec[0], &s_pend_blob.rec[1],
                (size_t)(s_pend_blob.count - 1U) * sizeof(s_pend_blob.rec[0]));
        s_pend_blob.count--;
        ESP_LOGW(TAG, "Hang doi NVS day — bo ban ghi cu nhat");
    }

    scan_pend_rec_t *r = &s_pend_blob.rec[s_pend_blob.count];
    memset(r, 0, sizeof(*r));
    strncpy(r->uid, uid_hex, sizeof(r->uid) - 1);
    if (name) {
        strncpy(r->name, name, sizeof(r->name) - 1);
    }
    if (id) {
        strncpy(r->id, id, sizeof(r->id) - 1);
    }
    r->registered = registered;
    r->utc_sec = (int64_t)time(NULL);
    s_pend_blob.count++;

    esp_err_t err = pend_save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pending save: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Da luu tam queue NVS (%u ban ghi)", (unsigned)s_pend_blob.count);
    }
    pend_mtx_give();
}

void scan_log_flush_pending(void)
{
    if (!sd_card_is_mounted()) {
        return;
    }

    pend_mtx_take();
    for (;;) {
        esp_err_t lr = pend_load();
        if (lr != ESP_OK) {
            ESP_LOGW(TAG, "flush load: %s", esp_err_to_name(lr));
            break;
        }
        if (s_pend_blob.count == 0) {
            break;
        }

        scan_pend_rec_t *head = &s_pend_blob.rec[0];
        time_t ev = (time_t)head->utc_sec;
        if (!write_one_csv_line(ev, head->uid, head->name, head->id, (int)head->registered)) {
            break;
        }

        memmove(&s_pend_blob.rec[0], &s_pend_blob.rec[1],
                (size_t)(s_pend_blob.count - 1U) * sizeof(s_pend_blob.rec[0]));
        s_pend_blob.count--;
        memset(&s_pend_blob.rec[s_pend_blob.count], 0, sizeof(s_pend_blob.rec[s_pend_blob.count]));

        esp_err_t err = pend_save();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush save: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "Flush pending con %u", (unsigned)s_pend_blob.count);
    }
    pend_mtx_give();
}

void scan_log_append(const char *uid_hex, const char *name, const char *id, int registered)
{
    if (!uid_hex || !uid_hex[0]) {
        return;
    }

    time_t ev = time(NULL);

    if (sd_card_is_mounted()) {
        if (write_one_csv_line(ev, uid_hex, name, id, registered)) {
            return;
        }
    }

    pending_enqueue(uid_hex, name, id, registered);
}

static void html_escape(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in) {
        in = "";
    }
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        if (in[i] == '&') {
            if (j + 5 >= outsz) {
                break;
            }
            memcpy(out + j, "&amp;", 5);
            j += 5;
        } else if (in[i] == '<') {
            if (j + 4 >= outsz) {
                break;
            }
            memcpy(out + j, "&lt;", 4);
            j += 4;
        } else if (in[i] == '>') {
            if (j + 4 >= outsz) {
                break;
            }
            memcpy(out + j, "&gt;", 4);
            j += 4;
        } else if (in[i] == '"') {
            if (j + 6 >= outsz) {
                break;
            }
            memcpy(out + j, "&quot;", 6);
            j += 6;
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

/** ts dang YYYY-MM-DDThh:mm:ss — loc theo ngay dia phuong */
static int line_matches_today(const char *ts, const char *today_ymd)
{
    if (!ts || strlen(ts) < 10) {
        return 0;
    }
    return strncmp(ts, today_ymd, 10) == 0;
}

esp_err_t scan_log_send_html_page(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req,
                                  "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>"
                                  "<p>SD chua gan hoac chua mount.</p><p><a href=\"/\">Ve trang chu</a></p></body></html>");
    }

    bool show_all = false;
    char qry[128];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(qry, "all", v, sizeof(v)) == ESP_OK && v[0] == '1') {
            show_all = true;
        }
    }

    time_t now = time(NULL);
    struct tm tloc;
    scan_log_wall_tm(now, &tloc);
    char today_ymd[48];
    (void)snprintf(today_ymd, sizeof(today_ymd), "%04d-%02d-%02d", (int)(tloc.tm_year + 1900),
                   (int)(tloc.tm_mon + 1), (int)tloc.tm_mday);

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req,
                                  "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>"
                                  "<p>Chua co file nhat ky. Quet the khi SD da mount.</p>"
                                  "<p><a href=\"/\">Ve trang chu</a></p></body></html>");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    char head_buf[900];
    snprintf(head_buf, sizeof(head_buf),
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
             "<meta http-equiv=\"refresh\" content=\"8\">"
             "<title>Nhat ky RFID</title><style>body{font-family:sans-serif;margin:12px}"
             "table{border-collapse:collapse;width:100%%;font-size:14px;max-width:900px}"
             "td,th{border:1px solid #ccc;padding:6px;text-align:left}"
             "tr:nth-child(even){background:#f5f5f5}.nav a{margin-right:12px}"
             ".admin-save{background:#d4edda!important;font-style:italic}"
             ".admin-del{background:#f8d7da!important;font-style:italic}</style></head><body>"
             "<h1>%s</h1>"
             "<p class=\"nav\"><a href=\"/\">Cau hinh WiFi</a>"
             "%s"
             "</p><p><small>Tu tai lai moi 8 giay</small></p>"
             "<table><tr><th>Thoi gian</th><th>Ten</th><th>Ma / Thao tac</th></tr>",
             show_all ? "Tat ca nhat ky"
                      : "Quet the trong ngay",
             show_all ? "<a href=\"/scans\">&larr; Chi hom nay</a>"
                      : "<a href=\"/scans?all=1\">Xem tat ca</a>");

    esp_err_t e = httpd_resp_send_chunk(req, head_buf, strlen(head_buf));
    if (e != ESP_OK) {
        fclose(fp);
        sd_card_unlock();
        return e;
    }

    if (!show_all) {
        char sub[192];
        snprintf(sub, sizeof(sub), "<p><strong>Ngay: %02d/%02d/%04d</strong> (Real time)</p>",
                 (int)tloc.tm_mday, (int)(tloc.tm_mon + 1), (int)(tloc.tm_year + 1900));
        e = httpd_resp_send_chunk(req, sub, strlen(sub));
        if (e != ESP_OK) {
            fclose(fp);
            sd_card_unlock();
            return e;
        }
    }

    char raw[384];
    int rows_out = 0;
    while (fgets(raw, sizeof(raw), fp)) {
        char buf[384];
        snprintf(buf, sizeof(buf), "%s", raw);
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
        }

        char *fld0 = strtok(buf, "|");
        if (!fld0) {
            continue;
        }
        (void)strtok(NULL, "|"); /* bo qua uid */
        char *fld2 = strtok(NULL, "|"); /* name */
        char *fld3 = strtok(NULL, "|"); /* id */
        char *fld4 = strtok(NULL, "|"); /* registered / "99"=admin */
        char *fld5 = strtok(NULL, "|"); /* admin action label (SAVE/DEL) neu fld4==99 */

        const char *ts = fld0;
        const char *nm = fld2 ? fld2 : "";
        const char *idv = fld3 ? fld3 : "";
        int is_admin = (fld4 && strcmp(fld4, "99") == 0);
        const char *admin_act = (is_admin && fld5 && fld5[0]) ? fld5 : "";

        if (!show_all && !line_matches_today(ts, today_ymd)) {
            continue;
        }

        char e_ts[80], e_nm[128], e_id[128], e_act[24];
        html_escape(ts, e_ts, sizeof(e_ts));
        html_escape(nm, e_nm, sizeof(e_nm));
        html_escape(idv, e_id, sizeof(e_id));
        html_escape(admin_act, e_act, sizeof(e_act));

        char row[600];
        if (is_admin) {
            /* Admin row: hien thi ten the va action (SAVE/DEL) voi mau khac */
            const char *css_class = (strcmp(admin_act, "DEL") == 0) ? "admin-del" : "admin-save";
            const char *label = (strcmp(admin_act, "DEL") == 0) ? "[XOA THE]" : "[LUU THE]";
            snprintf(row, sizeof(row),
                     "<tr class=\"%s\"><td>%s</td><td>%s</td><td>%s %s</td></tr>",
                     css_class, e_ts, e_nm, label, e_id);
        } else {
            snprintf(row, sizeof(row),
                     "<tr><td>%s</td><td>%s</td><td>%s</td></tr>", e_ts, e_nm, e_id);
        }
        e = httpd_resp_send_chunk(req, row, strlen(row));
        if (e != ESP_OK) {
            fclose(fp);
            sd_card_unlock();
            return e;
        }
        rows_out++;
    }
    fclose(fp);
    sd_card_unlock();

    if (!show_all && rows_out == 0) {
        const char *empty_row = "<tr><td colspan=\"3\"><em>Chua co lan quet nao trong ngay hom nay.</em></td></tr>";
        e = httpd_resp_send_chunk(req, empty_row, strlen(empty_row));
        if (e != ESP_OK) {
            return e;
        }
    }

    char tail[256];
    snprintf(tail, sizeof(tail), "</table><p><small>File: %s</small></p></body></html>", BOARD_SD_RFID_LOG_PATH);
    e = httpd_resp_send_chunk(req, tail, strlen(tail));
    if (e != ESP_OK) {
        return e;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

void scan_log_append_admin(const char *uid_hex, const char *name, const char *id, const char *action)
{
    if (!uid_hex || !uid_hex[0] || !action || !action[0]) {
        return;
    }

    time_t ev = time(NULL);
    char ts[40];
    format_ts_from_utc(ev, ts, sizeof(ts));

    char uid_s[40], ns[80], ids[80], acts[16];
    sanitize_field(uid_hex, uid_s, sizeof(uid_s));
    sanitize_field(name ? name : "", ns, sizeof(ns));
    sanitize_field(id ? id : "", ids, sizeof(ids));
    sanitize_field(action, acts, sizeof(acts));

    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "scan_log_append_admin: SD chua mount, bo qua");
        return;
    }

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "a");
    if (!fp) {
        sd_card_unlock();
        ESP_LOGW(TAG, "fopen admin log: %s", strerror(errno));
        return;
    }

    /* Dung truong registered = 99 de danh dau admin action; action nam o cuoi */
    fprintf(fp, "%s|%s|%s|%s|99|%s\n", ts, uid_s, ns, ids, acts);
    fflush(fp);
    {
        int fd = fileno(fp);
        if (fd >= 0) {
            (void)fsync(fd);
        }
    }
    fclose(fp);
    sd_card_unlock();
    ESP_LOGI(TAG, "Admin log: %s UID=%s name=%s", acts, uid_s, ns);
    scan_log_do_trim(false);
}

void scan_log_trim_at_boot(void)
{
    scan_log_do_trim(true);
}
