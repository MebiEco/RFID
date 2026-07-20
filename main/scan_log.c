#include "scan_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_azure.h"
#include "board_pins.h"
#include "lcd_ui.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sd_card.h"
#include "esp_task_wdt.h"
#include "lv_port.h"
#include <sys/stat.h>

static const char *TAG = "scan_log";

static int scan_log_row_event_code(int reg, bool is_admin, const char *admin_act)
{
    if (is_admin) {
        if (admin_act && strcmp(admin_act, "SAVE") == 0) {
            return 603;
        }
        if (admin_act && strcmp(admin_act, "DEL") == 0) {
            return 604;
        }
        return 0;
    }
    if (reg == 1) {
        return 602;
    }
    if (reg == 0) {
        return 601;
    }
    return 0;
}

/** Chi giữ nhật ký trong vòng 90 ngày, tránh đầy thẻ SD nhưng vẫn đảm bảo lịch sử. */
#define SCAN_LOG_AUTO_TRIM 1
#define SCAN_LOG_KEEP_DAYS 90
#define SCAN_LOG_KEEP_SEC ((time_t)(SCAN_LOG_KEEP_DAYS * 86400))
/** Tăng giới hạn dung lượng lên 10MB để ưu tiên giữ đủ 90 ngày. */
#define SCAN_LOG_TRIM_FORCE_BYTES ((off_t)(10 * 1024 * 1024))
/** Không quét lại toàn bộ file quá thường xuyên (giây mỗi lần quét). */
#define SCAN_LOG_TRIM_INTERVAL_SEC (6 * 3600)

static time_t s_scan_log_last_trim_sec;

/** Hang doi tam khi khong ghi duoc SD — luu tren NVS, flush FIFO khi SD mount. */
#define SCAN_PEND_NVS_NS "scan_pend"
#define SCAN_PEND_NVS_KEY "q"
#define SCAN_PEND_MAGIC 0x51455245u /* 'QERE' v2 queue with index */
#define SCAN_PEND_MAX 32

typedef struct {
    char uid[24];
    char name[48];
    char id[48];
    int32_t registered;
    int64_t utc_sec;
    int32_t index;
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

static void csv_strip_line_end(char *buf)
{
    if (!buf) {
        return;
    }
    char *nl = strchr(buf, '\n');
    if (nl) {
        *nl = '\0';
    }
    char *cr = strchr(buf, '\r');
    if (cr) {
        *cr = '\0';
    }
}

static void csv_strip_field_cr(char *fld)
{
    if (!fld) {
        return;
    }
    char *cr = strchr(fld, '\r');
    if (cr) {
        *cr = '\0';
    }
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
    if (!SCAN_LOG_AUTO_TRIM) {
        (void)bypass_rate_limit;
        return;
    }
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
        /* Feed watchdog mỗi khi đọc/ghi một dòng để tránh trigger TWDT khi file log lớn (>10MB) */
        lv_port_feed_wdt();
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }

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
                               int registered, int32_t index)
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

    if (fprintf(fp, "%s|%s|%s|%s|%d|%ld\n", ts, uid_s, ns, ids, registered, (long)index) < 0) {
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
    ESP_LOGI(TAG, "Ghi nhat ky: %s UID=%s index=%ld", ts, uid_s, (long)index);
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

static void pending_enqueue(const char *uid_hex, const char *name, const char *id, int registered, int32_t index)
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
    r->index = index;
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
        if (!write_one_csv_line(ev, head->uid, head->name, head->id, (int)head->registered, head->index)) {
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

void scan_log_append(const char *uid_hex, const char *name, const char *id, int registered, int32_t msg_idx)
{
    if (!uid_hex || !uid_hex[0]) {
        return;
    }

    time_t ev = time(NULL);

    if (sd_card_is_mounted()) {
        if (write_one_csv_line(ev, uid_hex, name, id, registered, msg_idx)) {
            return;
        }
    }

    pending_enqueue(uid_hex, name, id, registered, msg_idx);
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

static void json_escape(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in) {
        in = "";
    }
    for (size_t i = 0; in[i] && j + 2 < outsz; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= outsz) {
                break;
            }
            out[j++] = '\\';
            out[j++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            out[j++] = c;
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

/** Ghi YYYY-MM-DD (can toi thieu 11 byte); gioi han y/m/d de tranh -Wformat-truncation. */
static void scan_log_format_ymd(const struct tm *t, char *out, size_t outsz)
{
    if (!out || outsz < 11) {
        return;
    }
    if (!t) {
        out[0] = '\0';
        return;
    }
    int y = (int)(t->tm_year + 1900);
    int m = (int)(t->tm_mon + 1);
    int d = (int)t->tm_mday;
    if (y < 1970) {
        y = 1970;
    } else if (y > 9999) {
        y = 9999;
    }
    if (m < 1) {
        m = 1;
    } else if (m > 12) {
        m = 12;
    }
    if (d < 1) {
        d = 1;
    } else if (d > 31) {
        d = 31;
    }
    snprintf(out, outsz, "%04d-%02d-%02d", y, m, d);
}

typedef enum {
    SCAN_LOG_SORT_TIME_DESC = 0,
    SCAN_LOG_SORT_TIME_ASC,
    SCAN_LOG_SORT_INDEX_DESC,
    SCAN_LOG_SORT_INDEX_ASC,
    SCAN_LOG_SORT_ID_DESC,
    SCAN_LOG_SORT_ID_ASC,
} scan_log_sort_mode_t;

typedef struct {
    bool show_all;
    scan_log_sort_mode_t sort_mode;
    int days;
    int type_code; /* -1 = tat ca */
    int limit;
    int page;
    char from_ymd[11];
    char to_ymd[11];
} scan_log_json_query_t;

/** Gioi han khi sap xep theo index/ma NV (can buffer RAM). */
#define SCAN_LOG_SORT_BUF_MAX 400

typedef struct {
    char ts[80];
    char uid[48];
    char name[96];
    char id[48];
    char admin_act[24];
    int event_code;
    long msg_index;
    bool is_admin;
} scan_log_stored_row_t;

typedef struct {
    const char *ts;
    const char *uid;
    const char *name;
    const char *id;
    const char *admin_act;
    const char *idx_str;
    int event_code;
    long msg_index;
    bool is_admin;
} scan_log_parsed_row_t;

static bool scan_log_parse_csv_line(char *buf, scan_log_parsed_row_t *out)
{
    if (!buf || !out) {
        return false;
    }
    csv_strip_line_end(buf);
    char *saveptr = buf;
    char *fld0 = strsep(&saveptr, "|");
    if (!fld0) {
        return false;
    }
    char *fld1 = strsep(&saveptr, "|");
    char *fld2 = strsep(&saveptr, "|");
    char *fld3 = strsep(&saveptr, "|");
    char *fld4 = strsep(&saveptr, "|");
    char *fld5 = strsep(&saveptr, "|");
    char *fld6 = strsep(&saveptr, "|");
    csv_strip_field_cr(fld1);
    csv_strip_field_cr(fld2);
    csv_strip_field_cr(fld3);
    csv_strip_field_cr(fld4);
    csv_strip_field_cr(fld5);
    csv_strip_field_cr(fld6);

    out->ts = fld0;
    out->uid = fld1 ? fld1 : "";
    out->name = fld2 ? fld2 : "";
    out->id = fld3 ? fld3 : "";
    out->is_admin = (fld4 && strcmp(fld4, "99") == 0);
    int reg = (fld4 && !out->is_admin) ? atoi(fld4) : -1;
    out->admin_act = (out->is_admin && fld5 && fld5[0]) ? fld5 : "";
    out->idx_str = out->is_admin ? (fld6 ? fld6 : "-1") : (fld5 ? fld5 : "-1");
    out->msg_index = -1;
    if (out->idx_str && out->idx_str[0] && strcmp(out->idx_str, "-") != 0) {
        out->msg_index = atol(out->idx_str);
    }
    out->event_code = scan_log_row_event_code(reg, out->is_admin, out->admin_act);
    return true;
}

static bool scan_log_row_matches_query(const scan_log_parsed_row_t *row, const scan_log_json_query_t *q,
                                       bool time_ok, const char *today_ymd)
{
    if (!row || !q) {
        return false;
    }
    if (q->type_code >= 0 && row->event_code != q->type_code) {
        return false;
    }
    if (q->show_all) {
        return true;
    }
    if (!row->ts || strlen(row->ts) < 10) {
        return false;
    }
    if (!time_ok) {
        return true;
    }
    if (q->from_ymd[0] || q->to_ymd[0]) {
        if (q->from_ymd[0] && strncmp(row->ts, q->from_ymd, 10) < 0) {
            return false;
        }
        if (q->to_ymd[0] && strncmp(row->ts, q->to_ymd, 10) > 0) {
            return false;
        }
        return true;
    }
    return line_matches_today(row->ts, today_ymd) != 0;
}

static const char *scan_log_sort_mode_str(scan_log_sort_mode_t mode)
{
    switch (mode) {
    case SCAN_LOG_SORT_TIME_ASC:
        return "asc";
    case SCAN_LOG_SORT_INDEX_DESC:
        return "index_desc";
    case SCAN_LOG_SORT_INDEX_ASC:
        return "index_asc";
    case SCAN_LOG_SORT_ID_DESC:
        return "id_desc";
    case SCAN_LOG_SORT_ID_ASC:
        return "id_asc";
    case SCAN_LOG_SORT_TIME_DESC:
    default:
        return "desc";
    }
}

static bool scan_log_sort_by_field(scan_log_sort_mode_t mode)
{
    return mode >= SCAN_LOG_SORT_INDEX_DESC;
}

static void scan_log_store_row(const scan_log_parsed_row_t *src, scan_log_stored_row_t *dst)
{
    snprintf(dst->ts, sizeof(dst->ts), "%s", src->ts ? src->ts : "");
    snprintf(dst->uid, sizeof(dst->uid), "%s", src->uid ? src->uid : "");
    snprintf(dst->name, sizeof(dst->name), "%s", src->name ? src->name : "");
    snprintf(dst->id, sizeof(dst->id), "%s", src->id ? src->id : "");
    snprintf(dst->admin_act, sizeof(dst->admin_act), "%s", src->admin_act ? src->admin_act : "");
    dst->event_code = src->event_code;
    dst->msg_index = src->msg_index;
    dst->is_admin = src->is_admin;
}

static int scan_log_cmp_index_asc(const void *a, const void *b)
{
    const scan_log_stored_row_t *ra = (const scan_log_stored_row_t *)a;
    const scan_log_stored_row_t *rb = (const scan_log_stored_row_t *)b;
    if (ra->msg_index < rb->msg_index) {
        return -1;
    }
    if (ra->msg_index > rb->msg_index) {
        return 1;
    }
    return strcmp(ra->ts, rb->ts);
}

static int scan_log_cmp_index_desc(const void *a, const void *b)
{
    return scan_log_cmp_index_asc(b, a);
}

static int scan_log_cmp_id_asc(const void *a, const void *b)
{
    const scan_log_stored_row_t *ra = (const scan_log_stored_row_t *)a;
    const scan_log_stored_row_t *rb = (const scan_log_stored_row_t *)b;
    const bool ea = (ra->id[0] == '\0');
    const bool eb = (rb->id[0] == '\0');
    if (ea && !eb) {
        return 1;
    }
    if (!ea && eb) {
        return -1;
    }
    const int c = strcmp(ra->id, rb->id);
    if (c != 0) {
        return c;
    }
    return strcmp(ra->ts, rb->ts);
}

static int scan_log_cmp_id_desc(const void *a, const void *b)
{
    return scan_log_cmp_id_asc(b, a);
}

static esp_err_t scan_log_emit_json_stored(httpd_req_t *req, const scan_log_stored_row_t *row, bool *first)
{
    char e_ts[80], e_uid[48], e_nm[128], e_id[128], e_ad[24];
    json_escape(row->ts, e_ts, sizeof(e_ts));
    json_escape(row->uid, e_uid, sizeof(e_uid));
    json_escape(row->name, e_nm, sizeof(e_nm));
    json_escape(row->id, e_id, sizeof(e_id));
    json_escape(row->admin_act, e_ad, sizeof(e_ad));

    char chunk[640];
    snprintf(chunk, sizeof(chunk),
             "%s{\"ts\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"admin\":%s,\"action\":\"%s\","
             "\"code\":%d,\"index\":%ld}",
             *first ? "" : ",", e_ts, e_uid, e_nm, e_id, row->is_admin ? "true" : "false", e_ad, row->event_code,
             row->msg_index);
    *first = false;
    return httpd_resp_send_chunk(req, chunk, strlen(chunk));
}

static void scan_log_json_parse_query(httpd_req_t *req, scan_log_json_query_t *q, bool time_ok,
                                      const char *today_ymd)
{
    memset(q, 0, sizeof(*q));
    q->type_code = -1;
    q->limit = 50;
    q->page = 1;
    q->sort_mode = SCAN_LOG_SORT_TIME_DESC;

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return;
    }
    char v[16];
    if (httpd_query_key_value(query, "all", v, sizeof(v)) == ESP_OK && v[0] == '1') {
        q->show_all = true;
    }
    if (httpd_query_key_value(query, "days", v, sizeof(v)) == ESP_OK) {
        int d = atoi(v);
        if (d > 0) {
            q->days = d;
        }
    }
    if (httpd_query_key_value(query, "from", v, sizeof(v)) == ESP_OK && v[0]) {
        snprintf(q->from_ymd, sizeof(q->from_ymd), "%.10s", v);
    }
    if (httpd_query_key_value(query, "to", v, sizeof(v)) == ESP_OK && v[0]) {
        snprintf(q->to_ymd, sizeof(q->to_ymd), "%.10s", v);
    }
    if (httpd_query_key_value(query, "code", v, sizeof(v)) == ESP_OK && v[0]) {
        int c = atoi(v);
        if (c == 601 || c == 602 || c == 603 || c == 604) {
            q->type_code = c;
        }
    }
    if (httpd_query_key_value(query, "sort", v, sizeof(v)) == ESP_OK && v[0]) {
        if (strcmp(v, "asc") == 0) {
            q->sort_mode = SCAN_LOG_SORT_TIME_ASC;
        } else if (strcmp(v, "index_desc") == 0) {
            q->sort_mode = SCAN_LOG_SORT_INDEX_DESC;
        } else if (strcmp(v, "index_asc") == 0) {
            q->sort_mode = SCAN_LOG_SORT_INDEX_ASC;
        } else if (strcmp(v, "id_desc") == 0) {
            q->sort_mode = SCAN_LOG_SORT_ID_DESC;
        } else if (strcmp(v, "id_asc") == 0) {
            q->sort_mode = SCAN_LOG_SORT_ID_ASC;
        } else {
            q->sort_mode = SCAN_LOG_SORT_TIME_DESC;
        }
    }
    if (httpd_query_key_value(query, "limit", v, sizeof(v)) == ESP_OK) {
        int n = atoi(v);
        if (n > 0 && n <= 200) {
            q->limit = n;
        }
    }
    if (httpd_query_key_value(query, "page", v, sizeof(v)) == ESP_OK) {
        int p = atoi(v);
        if (p > 0) {
            q->page = p;
        }
    }

    if (q->days > 0 && time_ok && !q->show_all) {
        time_t now = time(NULL);
        time_t from_t = now - (time_t)q->days * 86400;
        struct tm tfrom;
        scan_log_wall_tm(from_t, &tfrom);
        scan_log_format_ymd(&tfrom, q->from_ymd, sizeof(q->from_ymd));
        strncpy(q->to_ymd, today_ymd, sizeof(q->to_ymd) - 1);
        q->to_ymd[sizeof(q->to_ymd) - 1] = '\0';
    }
}

static esp_err_t scan_log_emit_json_row(httpd_req_t *req, const scan_log_parsed_row_t *row, bool *first)
{
    char e_ts[80], e_uid[48], e_nm[128], e_id[128], e_ad[24];
    json_escape(row->ts, e_ts, sizeof(e_ts));
    json_escape(row->uid, e_uid, sizeof(e_uid));
    json_escape(row->name, e_nm, sizeof(e_nm));
    json_escape(row->id, e_id, sizeof(e_id));
    json_escape(row->admin_act, e_ad, sizeof(e_ad));

    char chunk[640];
    snprintf(chunk, sizeof(chunk),
             "%s{\"ts\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"admin\":%s,\"action\":\"%s\","
             "\"code\":%d,\"index\":%ld}",
             *first ? "" : ",", e_ts, e_uid, e_nm, e_id, row->is_admin ? "true" : "false", e_ad, row->event_code,
             row->msg_index);
    *first = false;
    return httpd_resp_send_chunk(req, chunk, strlen(chunk));
}

static const char SCANS_CSS[] =
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#090d16;color:#f3f4f6;padding:24px;line-height:1.5}"
    "h1{font-size:22px;font-weight:600;margin-bottom:16px;color:#fff;border-bottom:1px solid rgba(255,255,255,0.08);padding-bottom:12px}"
    ".nav{margin-bottom:16px;display:flex;gap:12px}"
    ".nav a{color:#6366f1;text-decoration:none;font-size:14px;font-weight:500;transition:color .2s}"
    ".nav a:hover{color:#8b5cf6}"
    ".hint{font-size:12px;color:#9ca3af;margin:8px 0 16px}"
    "table{width:100%;border-collapse:collapse;font-size:13px;max-width:800px;border-radius:8px;overflow:hidden;margin-top:10px}"
    "th{background:#0f172a;color:#9ca3af;font-weight:600;text-transform:uppercase;font-size:11px;letter-spacing:.5px;padding:12px 16px;text-align:left}"
    "td{padding:12px 16px;border-bottom:1px solid rgba(255,255,255,0.04);color:#e5e7eb}"
    "tr:last-child td{border-bottom:none}"
    "tr:nth-child(even){background:rgba(255,255,255,0.01)}"
    "tr:hover{background:rgba(255,255,255,0.03)}"
    ".admin-save{background:rgba(16,185,129,0.08)!important;font-style:italic}"
    ".admin-del{background:rgba(239,68,68,0.08)!important;font-style:italic}"
    "code{color:#06b6d4}"
    "</style>";

esp_err_t scan_log_send_html_page(httpd_req_t *req)
{
    if (!sd_card_is_mounted()) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req,
                                  "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                  "<meta name=\"viewport\" content=\"width=device-width\">"
                                  "<style>body{font-family:sans-serif;background:#090d16;color:#f3f4f6;padding:24px;text-align:center}"
                                  "a{color:#6366f1;text-decoration:none}</style></head><body>"
                                  "<h3>Thẻ SD chưa gắn hoặc chưa mount.</h3>"
                                  "<p style=\"margin-top:12px\"><a href=\"/\">Về trang chủ</a></p></body></html>");
    }

    bool show_all = false;
    char qry[128];
    char tok[36] = {0};
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(qry, "all", v, sizeof(v)) == ESP_OK && v[0] == '1') {
            show_all = true;
        }
        httpd_query_key_value(qry, "token", tok, sizeof(tok));
    }

    time_t now = time(NULL);
    struct tm tloc;
    scan_log_wall_tm(now, &tloc);
    const bool time_ok = (tloc.tm_year >= (2020 - 1900));
    char today_ymd[16];
    scan_log_format_ymd(&tloc, today_ymd, sizeof(today_ymd));

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req,
                                  "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                  "<meta name=\"viewport\" content=\"width=device-width\">"
                                  "<style>body{font-family:sans-serif;background:#090d16;color:#f3f4f6;padding:24px;text-align:center}"
                                  "a{color:#6366f1;text-decoration:none}</style></head><body>"
                                  "<h3>Chưa có file nhật ký. Quét thẻ khi SD đã mount.</h3>"
                                  "<p style=\"margin-top:12px\"><a href=\"/\">Về trang chủ</a></p></body></html>");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    esp_err_t e = httpd_resp_send_chunk(req, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><meta http-equiv=\"refresh\" content=\"8\"><title>Nhật ký RFID</title>", HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) {
        fclose(fp);
        sd_card_unlock();
        return e;
    }
    e = httpd_resp_send_chunk(req, SCANS_CSS, HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) {
        fclose(fp);
        sd_card_unlock();
        return e;
    }
    e = httpd_resp_send_chunk(req, "</head><body>", HTTPD_RESP_USE_STRLEN);
    if (e != ESP_OK) {
        fclose(fp);
        sd_card_unlock();
        return e;
    }

    char link_buf[192];
    if (show_all) {
        if (tok[0]) {
            snprintf(link_buf, sizeof(link_buf), " | <a href=\"/scans?token=%s\">Chỉ hôm nay</a>", tok);
        } else {
            strcpy(link_buf, " | <a href=\"/scans\">Chỉ hôm nay</a>");
        }
    } else {
        if (tok[0]) {
            snprintf(link_buf, sizeof(link_buf), " | <a href=\"/scans?all=1&token=%s\">Xem tất cả</a>", tok);
        } else {
            strcpy(link_buf, " | <a href=\"/scans?all=1\">Xem tất cả</a>");
        }
    }

    char head_buf[768];
    snprintf(head_buf, sizeof(head_buf),
             "<h1>%s</h1>"
             "<p class=\"nav\"><a href=\"/\">&larr; Về trang chủ</a>"
             "%s"
             "</p><p class=\"hint\">Tự động tải lại sau mỗi 8 giây</p>"
             "<table><tr><th>Code</th><th>Index</th><th>Thời gian</th><th>Tên nhân viên</th><th>Mã / Thao tác</th></tr>",
             show_all ? "Tất cả nhật ký" : "Quét thẻ trong ngày",
             link_buf);

    e = httpd_resp_send_chunk(req, head_buf, strlen(head_buf));
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
        lv_port_feed_wdt();
        char buf[384];
        snprintf(buf, sizeof(buf), "%s", raw);
        csv_strip_line_end(buf);

        char *fld0 = strtok(buf, "|");
        if (!fld0) {
            continue;
        }
        char *fld1 = strtok(NULL, "|"); /* uid */
        char *fld2 = strtok(NULL, "|"); /* name */
        char *fld3 = strtok(NULL, "|"); /* id */
        char *fld4 = strtok(NULL, "|"); /* registered / "99"=admin */
        char *fld5 = strtok(NULL, "|"); /* admin action label (SAVE/DEL) neu fld4==99, hoac index neu normal */
        char *fld6 = strtok(NULL, "|"); /* index neu admin, NULL neu cu */
        csv_strip_field_cr(fld1);
        csv_strip_field_cr(fld2);
        csv_strip_field_cr(fld3);
        csv_strip_field_cr(fld4);
        csv_strip_field_cr(fld5);
        csv_strip_field_cr(fld6);

        const char *ts = fld0;
        const char *uidv = fld1 ? fld1 : "";
        const char *nm = fld2 ? fld2 : "";
        const char *idv = fld3 ? fld3 : "";
        int is_admin = (fld4 && strcmp(fld4, "99") == 0);
        int reg = (fld4 && !is_admin) ? atoi(fld4) : -1;
        const char *admin_act = (is_admin && fld5 && fld5[0]) ? fld5 : "";
        const char *idx_str = is_admin ? (fld6 ? fld6 : "-") : (fld5 ? fld5 : "-");
        if (!idx_str || idx_str[0] == '\0') {
            idx_str = "-";
        }
        int event_code = scan_log_row_event_code(reg, is_admin, admin_act);

        if (!show_all && time_ok && !line_matches_today(ts, today_ymd)) {
            continue;
        }

        char e_ts[80], e_uid[48], e_nm[128], e_id[128], e_act[24];
        html_escape(ts, e_ts, sizeof(e_ts));
        html_escape(uidv, e_uid, sizeof(e_uid));
        html_escape(nm, e_nm, sizeof(e_nm));
        html_escape(idv, e_id, sizeof(e_id));
        html_escape(admin_act, e_act, sizeof(e_act));

        char row[720];
        if (is_admin) {
            /* Admin row: hien thi ten the va action (SAVE/DEL) voi mau khac */
            const char *css_class = (strcmp(admin_act, "DEL") == 0) ? "admin-del" : "admin-save";
            const char *label = (strcmp(admin_act, "DEL") == 0) ? "[XOA THE]" : "[LUU THE]";
            const char *who = e_nm[0] ? e_nm : e_uid;
            const char *detail = e_id[0] ? e_id : e_uid;
            snprintf(row, sizeof(row),
                     "<tr class=\"%s\"><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s %s (%s)</td></tr>",
                     css_class, event_code, idx_str, e_ts, who, label, detail, e_uid);
        } else {
            snprintf(row, sizeof(row),
                     "<tr><td>%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
                     event_code, idx_str, e_ts, e_nm, e_id);
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
        const char *empty_row = "<tr><td colspan=\"5\"><em>Chua co lan quet nao trong ngay hom nay.</em></td></tr>";
        e = httpd_resp_send_chunk(req, empty_row, strlen(empty_row));
        if (e != ESP_OK) {
            return e;
        }
    }

    char tail[384];
    snprintf(tail, sizeof(tail), "</table><p><small>File: %s</small></p><script>const tb=document.querySelector('table');const rs=Array.from(tb.rows).slice(1);rs.reverse().forEach(r=>tb.appendChild(r));</script></body></html>", BOARD_SD_RFID_LOG_PATH);
    e = httpd_resp_send_chunk(req, tail, strlen(tail));
    if (e != ESP_OK) {
        return e;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void scan_log_page_range(const scan_log_json_query_t *jq, int total_matched, int *start_idx, int *end_idx)
{
    if (jq->sort_mode == SCAN_LOG_SORT_TIME_DESC) {
        *start_idx = total_matched - (jq->page * jq->limit);
        if (*start_idx < 0) {
            *start_idx = 0;
        }
        *end_idx = total_matched - ((jq->page - 1) * jq->limit);
        if (*end_idx < 0) {
            *end_idx = 0;
        }
        return;
    }
    *start_idx = (jq->page - 1) * jq->limit;
    if (*start_idx > total_matched) {
        *start_idx = total_matched;
    }
    *end_idx = *start_idx + jq->limit;
    if (*end_idx > total_matched) {
        *end_idx = total_matched;
    }
}

esp_err_t scan_log_send_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");

    if (!sd_card_is_mounted()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD chua mount\",\"rows\":[]}");
    }

    time_t now = time(NULL);
    struct tm tloc;
    scan_log_wall_tm(now, &tloc);
    const bool time_ok = (tloc.tm_year >= (2020 - 1900));
    char today_ymd[16];
    scan_log_format_ymd(&tloc, today_ymd, sizeof(today_ymd));

    scan_log_json_query_t jq;
    scan_log_json_parse_query(req, &jq, time_ok, today_ymd);

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":true,\"rows\":[]}");
    }

    const bool field_sort = scan_log_sort_by_field(jq.sort_mode);
    scan_log_stored_row_t *stored = NULL;
    int stored_count = 0;
    int full_filtered = 0;
    bool sort_truncated = false;

    if (field_sort) {
        stored = (scan_log_stored_row_t *)calloc(SCAN_LOG_SORT_BUF_MAX, sizeof(scan_log_stored_row_t));
        if (!stored) {
            fclose(fp);
            sd_card_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\",\"rows\":[]}");
        }
    }

    int total_matched = 0;
    char temp_line[384];
    while (fgets(temp_line, sizeof(temp_line), fp)) {
        lv_port_feed_wdt();
        char buf[384];
        snprintf(buf, sizeof(buf), "%s", temp_line);
        scan_log_parsed_row_t row;
        if (!scan_log_parse_csv_line(buf, &row)) {
            continue;
        }
        if (!scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
            continue;
        }
        if (field_sort) {
            full_filtered++;
            if (stored_count < SCAN_LOG_SORT_BUF_MAX) {
                scan_log_store_row(&row, &stored[stored_count++]);
            } else {
                sort_truncated = true;
            }
            continue;
        }
        total_matched++;
    }

    if (field_sort) {
        if (full_filtered > SCAN_LOG_SORT_BUF_MAX) {
            sort_truncated = true;
        }
        total_matched = stored_count;
        int (*cmp_fn)(const void *, const void *) = scan_log_cmp_index_desc;
        if (jq.sort_mode == SCAN_LOG_SORT_INDEX_ASC) {
            cmp_fn = scan_log_cmp_index_asc;
        } else if (jq.sort_mode == SCAN_LOG_SORT_ID_ASC) {
            cmp_fn = scan_log_cmp_id_asc;
        } else if (jq.sort_mode == SCAN_LOG_SORT_ID_DESC) {
            cmp_fn = scan_log_cmp_id_desc;
        }
        if (stored_count > 1) {
            qsort(stored, (size_t)stored_count, sizeof(scan_log_stored_row_t), cmp_fn);
        }
    }

    int start_idx = 0;
    int end_idx = 0;
    scan_log_page_range(&jq, total_matched, &start_idx, &end_idx);
    const int items_to_read = end_idx - start_idx;

    char hdr[224];
    if (field_sort && sort_truncated) {
        snprintf(hdr, sizeof(hdr),
                 "{\"ok\":true,\"time_ok\":%s,\"total\":%d,\"total_filtered\":%d,\"page\":%d,\"limit\":%d,"
                 "\"sort\":\"%s\",\"sort_truncated\":true,\"rows\":[",
                 time_ok ? "true" : "false", total_matched, full_filtered, jq.page, jq.limit,
                 scan_log_sort_mode_str(jq.sort_mode));
    } else {
        snprintf(hdr, sizeof(hdr),
                 "{\"ok\":true,\"time_ok\":%s,\"total\":%d,\"page\":%d,\"limit\":%d,\"sort\":\"%s\","
                 "\"sort_truncated\":false,\"rows\":[",
                 time_ok ? "true" : "false", total_matched, jq.page, jq.limit,
                 scan_log_sort_mode_str(jq.sort_mode));
    }
    esp_err_t e = httpd_resp_send_chunk(req, hdr, strlen(hdr));
    if (e != ESP_OK) {
        fclose(fp);
        free(stored);
        sd_card_unlock();
        return e;
    }

    int rows_out = 0;
    bool first = true;

    if (field_sort) {
        for (int i = start_idx; i < end_idx && rows_out < items_to_read; i++) {
            e = scan_log_emit_json_stored(req, &stored[i], &first);
            if (e != ESP_OK) {
                fclose(fp);
                free(stored);
                sd_card_unlock();
                return e;
            }
            rows_out++;
        }
    } else {
        rewind(fp);
        int current_match = 0;
        char raw[384];
        while (fgets(raw, sizeof(raw), fp) && rows_out < items_to_read) {
            lv_port_feed_wdt();
            char buf[384];
            snprintf(buf, sizeof(buf), "%s", raw);
            scan_log_parsed_row_t row;
            if (!scan_log_parse_csv_line(buf, &row)) {
                continue;
            }
            if (!scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
                continue;
            }
            if (current_match < start_idx) {
                current_match++;
                continue;
            }

            e = scan_log_emit_json_row(req, &row, &first);
            if (e != ESP_OK) {
                fclose(fp);
                sd_card_unlock();
                return e;
            }
            current_match++;
            rows_out++;
        }
    }

    fclose(fp);
    free(stored);
    sd_card_unlock();

    e = httpd_resp_send_chunk(req, "]}", 2);
    if (e != ESP_OK) {
        return e;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

void scan_log_append_admin(const char *uid_hex, const char *name, const char *id, const char *action,
                           int32_t msg_idx)
{
    if (!uid_hex || !uid_hex[0] || !action || !action[0]) {
        return;
    }
    if (msg_idx <= 0) {
        msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
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

    /* registered=99: admin; SAVE/DEL + index (trung idx_admin / telemetry 603-604) */
    fprintf(fp, "%s|%s|%s|%s|99|%s|%ld\n", ts, uid_s, ns, ids, acts, (long)msg_idx);
    fflush(fp);
    {
        int fd = fileno(fp);
        if (fd >= 0) {
            (void)fsync(fd);
        }
    }
    fclose(fp);
    sd_card_unlock();
    ESP_LOGI(TAG, "Admin log: %s UID=%s name=%s index=%ld", acts, uid_s, ns, (long)msg_idx);
    scan_log_do_trim(false);
    lcd_ui_invalidate_log_cache();
}

void scan_log_trim_at_boot(void)
{
    scan_log_do_trim(true);
}

static time_t scan_log_ts_string_to_utc(const char *ts_str)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (!ts_str || ts_str[0] == '\0' || strncmp(ts_str, "no-ntp", 6) == 0) {
        return 0;
    }
    if (sscanf(ts_str, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return 0;
    }
    struct tm ti = {0};
    ti.tm_year = y - 1900;
    ti.tm_mon = mo - 1;
    ti.tm_mday = d;
    ti.tm_hour = h;
    ti.tm_min = mi;
    ti.tm_sec = s;
    ti.tm_isdst = 0;
    /* Chuoi log = gmtime(utc + BOARD_LOCAL_UTC_OFFSET_SEC) */
    time_t wall_epoch = mktime(&ti);
    if (wall_epoch < 0) {
        return 0;
    }
    return wall_epoch - (time_t)BOARD_LOCAL_UTC_OFFSET_SEC;
}

static bool sync_filter_wants_missing(const scan_log_sync_filter_t *filter, int code, int32_t index)
{
    if (!filter || index <= 0) {
        return false;
    }
    for (int i = 0; i < filter->missing_count; i++) {
        if (filter->missing[i].code == code && filter->missing[i].index == index) {
            return true;
        }
    }
    return false;
}

static bool sync_filter_wants_gap(const scan_log_sync_filter_t *filter, int code, int32_t index)
{
    if (!filter || index <= 0) {
        return false;
    }
    if (code == 602 && filter->have_last_swipe && index > filter->last_swipe) {
        return true;
    }
    if (code == 601 && filter->have_last_unkn && index > filter->last_unkn) {
        return true;
    }
    if ((code == 603 || code == 604) && filter->have_last_admin && index > filter->last_admin) {
        return true;
    }
    return false;
}

esp_err_t scan_log_replay_gaps(const scan_log_sync_filter_t *filter, scan_log_replay_publish_fn publish_fn,
                               void *ctx, scan_log_replay_stats_t *stats_out)
{
    if (!filter || !publish_fn) {
        return ESP_ERR_INVALID_ARG;
    }
    scan_log_replay_stats_t st = {0};
    if (!sd_card_is_mounted()) {
        if (stats_out) {
            *stats_out = st;
        }
        return ESP_ERR_INVALID_STATE;
    }

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        if (stats_out) {
            *stats_out = st;
        }
        return ESP_OK;
    }

    char raw[384];
    while (fgets(raw, sizeof(raw), fp)) {
        char buf[384];
        snprintf(buf, sizeof(buf), "%s", raw);
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
        }

        char *fld0 = strtok(buf, "|");
        char *fld1 = strtok(NULL, "|");
        char *fld2 = strtok(NULL, "|");
        char *fld3 = strtok(NULL, "|");
        char *fld4 = strtok(NULL, "|");
        char *fld5 = strtok(NULL, "|");
        char *fld6 = strtok(NULL, "|");
        if (!fld0 || !fld1) {
            continue;
        }

        int event_code = 0;
        int32_t msg_index = 0;
        const char *uid = fld1;
        const char *nm = fld2 ? fld2 : "";
        const char *idv = fld3 ? fld3 : "";
        bool is_admin = (fld4 && strcmp(fld4, "99") == 0);

        if (is_admin) {
            const char *act = fld5 ? fld5 : "";
            const char *idx_str = fld6 ? fld6 : "";
            if (idx_str[0] != '\0') {
                msg_index = (int32_t)atol(idx_str);
            }
            if (strcmp(act, "SAVE") == 0) {
                event_code = 603;
            } else if (strcmp(act, "DEL") == 0) {
                event_code = 604;
            } else {
                continue;
            }
        } else {
            if (!fld4 || !fld5) {
                continue;
            }
            msg_index = (int32_t)atol(fld5);
            int reg = atoi(fld4);
            if (reg == 1) {
                event_code = 602;
            } else if (reg == 0) {
                event_code = 601;
            } else {
                continue;
            }
        }

        if (msg_index <= 0 || event_code <= 0) {
            continue;
        }

        bool want = sync_filter_wants_missing(filter, event_code, msg_index);
        if (!want) {
            want = sync_filter_wants_gap(filter, event_code, msg_index);
        }
        if (!want) {
            continue;
        }

        scan_log_replay_entry_t ent;
        memset(&ent, 0, sizeof(ent));
        ent.event_code = event_code;
        ent.index = msg_index;
        ent.timestamp_utc = (int64_t)scan_log_ts_string_to_utc(fld0);
        strncpy(ent.uid, uid, sizeof(ent.uid) - 1);
        strncpy(ent.name, nm, sizeof(ent.name) - 1);
        strncpy(ent.id, idv, sizeof(ent.id) - 1);

        if (publish_fn(&ent, ctx) != 0) {
            break;
        }
        st.resent++;
        if (event_code == 602) {
            st.gap_swipe++;
        } else if (event_code == 601) {
            st.gap_unkn++;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    fclose(fp);
    sd_card_unlock();

    if (stats_out) {
        *stats_out = st;
    }
    ESP_LOGI(TAG, "Replay tu log: resent=%d (swipe=%d unkn=%d)", st.resent, st.gap_swipe, st.gap_unkn);
    return ESP_OK;
}
