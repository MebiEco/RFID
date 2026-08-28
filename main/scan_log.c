#include "scan_log.h"

#include <ctype.h>
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
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sd_card.h"
#include "esp_task_wdt.h"
#include "lv_port.h"
#include "portal_web.h"
#include "app_rfid.h"
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
 * Blob ~4.2KB — PSRAM BSS (khong DMA). Giai phong Internal cho LCD/SD.
 * Mutex vi rfid_task va main deu goi enqueue/flush.
 */
static EXT_RAM_BSS_ATTR scan_pend_blob_t s_pend_blob;
static SemaphoreHandle_t s_pend_mtx;

#define SCAN_LOG_LINE_SZ 384
/** Doc dong CSV — PSRAM, dung duoi sd_card_lock (httpd + replay). */
EXT_RAM_BSS_ATTR static char s_scan_log_line[SCAN_LOG_LINE_SZ];
EXT_RAM_BSS_ATTR static char s_log_json_chunk[640];

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

/** Cot 1 moi: Unix UTC thuan so. Log cu: ISO gio VN (YYYY-MM-DDThh:mm:ss). */
static bool scan_log_ts_is_epoch_digits(const char *ts)
{
    if (!ts || ts[0] == '\0' || !isdigit((unsigned char)ts[0])) {
        return false;
    }
    for (const char *p = ts; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

/** ISO gio VN de hien thi / so sanh ngay (tu UTC). */
static void format_wall_iso_from_utc(time_t utc, char *ts, size_t ts_len)
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

/** Ghi CSV: dung TimeStamp Azure (Unix UTC). */
static void format_ts_for_csv(time_t utc, char *ts, size_t ts_len)
{
    if (utc >= (time_t)1577836800LL) { /* 2020-01-01 UTC */
        snprintf(ts, ts_len, "%lld", (long long)utc);
    } else {
        snprintf(ts, ts_len, "no-ntp");
    }
}

time_t scan_log_ts_field_to_utc(const char *ts_str)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (!ts_str || ts_str[0] == '\0' || strncmp(ts_str, "no-ntp", 6) == 0) {
        return 0;
    }
    if (scan_log_ts_is_epoch_digits(ts_str)) {
        long long v = atoll(ts_str);
        if (v < 1577836800LL) {
            return 0;
        }
        return (time_t)v;
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
    /* Log cu: chuoi = gmtime(utc + OFFSET) — parse UTC0 roi tru OFFSET. */
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t wall_as_utc = mktime(&ti);
    setenv("TZ", "UTC-7", 1);
    tzset();
    if (wall_as_utc == (time_t)-1) {
        return 0;
    }
    return wall_as_utc - (time_t)BOARD_LOCAL_UTC_OFFSET_SEC;
}

void scan_log_ts_field_ymd_local(const char *ts_field, char *ymd_out, size_t ymd_sz)
{
    if (!ymd_out || ymd_sz < 11) {
        return;
    }
    ymd_out[0] = '\0';
    if (!ts_field || ts_field[0] == '\0') {
        return;
    }
    time_t utc = scan_log_ts_field_to_utc(ts_field);
    if (utc > 0) {
        struct tm ti;
        scan_log_wall_tm(utc, &ti);
        if (ti.tm_year >= (2020 - 1900)) {
            snprintf(ymd_out, ymd_sz, "%04d-%02d-%02d", (int)(ti.tm_year + 1900), (int)(ti.tm_mon + 1),
                     (int)ti.tm_mday);
        }
        return;
    }
    /* Fallback ISO cu chua parse duoc day du: lay 10 ky tu dau */
    if (strlen(ts_field) >= 10 && ts_field[4] == '-' && ts_field[7] == '-') {
        snprintf(ymd_out, ymd_sz, "%.10s", ts_field);
    }
}

void scan_log_ts_field_to_wall_iso(const char *ts_field, char *out, size_t outsz)
{
    if (!out || outsz == 0) {
        return;
    }
    out[0] = '\0';
    time_t utc = scan_log_ts_field_to_utc(ts_field);
    if (utc > 0) {
        format_wall_iso_from_utc(utc, out, outsz);
        return;
    }
    if (ts_field && ts_field[0]) {
        snprintf(out, outsz, "%s", ts_field);
    }
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

/** Giu dong neu thoi gian (cot 1) >= cutoff UTC; "no-ntp" / khong parse: giu. */
static bool log_line_kept(const char *line, time_t cutoff_utc)
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
    time_t ev = scan_log_ts_field_to_utc(first);
    if (ev == 0) {
        return true;
    }
    return ev >= cutoff_utc;
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
    if (cutoff < (time_t)1577836800LL) {
        return;
    }

    sd_card_lock();
    FILE *in = fopen(path, "r");
    if (!in) {
        sd_card_unlock();
        return;
    }

    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s/log_trim.tmp", BOARD_SD_MOUNT_POINT);
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

        if (log_line_kept(buf, cutoff)) {
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
    format_ts_for_csv(event_utc, ts, sizeof(ts));

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

/** ts: epoch UTC hoac ISO VN cu — loc theo ngay dia phuong */
static int line_matches_today(const char *ts, const char *today_ymd)
{
    char ymd[12];
    scan_log_ts_field_ymd_local(ts, ymd, sizeof(ymd));
    if (ymd[0] == '\0' || !today_ymd) {
        return 0;
    }
    return strcmp(ymd, today_ymd) == 0;
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

/** Gioi han khi sap xep theo index/ma NV (PSRAM). Truoc 1000 ≈ 300KB — de Internal chet. */
#define SCAN_LOG_SORT_BUF_MAX 200
/** Toi da dong/trang portal — bot JSON + RAM httpd. */
#define SCAN_LOG_PAGE_LIMIT_MAX 30
/** Gioi han xem nhat ky tren web portal (bot RAM/SD). */
#define SCAN_LOG_WEB_MAX_DAYS 7
#define SCAN_LOG_WEB_MAX_PAGE 20
/** Dung dem quet file khi loc nhieu ngay — tranh quet ca MB log. */
#define SCAN_LOG_STREAM_MAX_MATCH (SCAN_LOG_WEB_MAX_PAGE * SCAN_LOG_PAGE_LIMIT_MAX)

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
    char row_ymd[12];
    scan_log_ts_field_ymd_local(row->ts, row_ymd, sizeof(row_ymd));
    if (row_ymd[0] == '\0') {
        return false;
    }
    if (!time_ok) {
        return true;
    }
    if (q->from_ymd[0] || q->to_ymd[0]) {
        if (q->from_ymd[0] && strcmp(row_ymd, q->from_ymd) < 0) {
            return false;
        }
        if (q->to_ymd[0] && strcmp(row_ymd, q->to_ymd) > 0) {
            return false;
        }
        return true;
    }
    return strcmp(row_ymd, today_ymd) == 0;
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
    char wall_ts[40];
    scan_log_ts_field_to_wall_iso(row->ts, wall_ts, sizeof(wall_ts));
    char e_ts[80], e_uid[48], e_nm[128], e_id[128], e_ad[24];
    json_escape(wall_ts[0] ? wall_ts : row->ts, e_ts, sizeof(e_ts));
    json_escape(row->uid, e_uid, sizeof(e_uid));
    json_escape(row->name, e_nm, sizeof(e_nm));
    json_escape(row->id, e_id, sizeof(e_id));
    json_escape(row->admin_act, e_ad, sizeof(e_ad));

    snprintf(s_log_json_chunk, sizeof(s_log_json_chunk),
             "%s{\"ts\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"admin\":%s,\"action\":\"%s\","
             "\"code\":%d,\"index\":%ld}",
             *first ? "" : ",", e_ts, e_uid, e_nm, e_id, row->is_admin ? "true" : "false", e_ad, row->event_code,
             row->msg_index);
    *first = false;
    return httpd_resp_send_chunk(req, s_log_json_chunk, strlen(s_log_json_chunk));
}

static void scan_log_json_parse_query(httpd_req_t *req, scan_log_json_query_t *q, bool time_ok,
                                      const char *today_ymd)
{
    memset(q, 0, sizeof(*q));
    q->type_code = -1;
    q->limit = 30;
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
        if (n > 0 && n <= SCAN_LOG_PAGE_LIMIT_MAX) {
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

static bool scan_log_parse_ymd_local(const char *ymd, struct tm *out)
{
    int y = 0, m = 0, d = 0;
    if (!ymd || !out || sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_year = y - 1900;
    out->tm_mon = m - 1;
    out->tm_mday = d;
    return true;
}

static int scan_log_ymd_span_days(const char *from, const char *to)
{
    struct tm tf;
    struct tm tt;
    if (!scan_log_parse_ymd_local(from, &tf) || !scan_log_parse_ymd_local(to, &tt)) {
        return -1;
    }
    time_t tf_t = mktime(&tf);
    time_t tt_t = mktime(&tt);
    if (tf_t == (time_t)-1 || tt_t == (time_t)-1 || tt_t < tf_t) {
        return -1;
    }
    return (int)((tt_t - tf_t) / 86400) + 1;
}

/** Gioi han portal: toi da 7 ngay, 20 trang; all/30 ngay tu dong thu ve 7. */
static bool scan_log_web_limits_ok(scan_log_json_query_t *jq, char *err, size_t err_sz)
{
    if (!jq) {
        return false;
    }
    if (jq->show_all) {
        jq->show_all = false;
        jq->days = SCAN_LOG_WEB_MAX_DAYS;
    }
    if (jq->days > SCAN_LOG_WEB_MAX_DAYS) {
        jq->days = SCAN_LOG_WEB_MAX_DAYS;
    }
    if (jq->page > SCAN_LOG_WEB_MAX_PAGE) {
        snprintf(err, err_sz, "Toi da %d trang (%d dong) tren web — thu loc hep hon", SCAN_LOG_WEB_MAX_PAGE,
                 SCAN_LOG_WEB_MAX_PAGE * jq->limit);
        return false;
    }
    if (jq->from_ymd[0] && jq->to_ymd[0]) {
        int span = scan_log_ymd_span_days(jq->from_ymd, jq->to_ymd);
        if (span < 0) {
            snprintf(err, err_sz, "Khoang ngay khong hop le");
            return false;
        }
        if (span > SCAN_LOG_WEB_MAX_DAYS) {
            snprintf(err, err_sz, "Web chi xem toi da %d ngay — thu hep Tu/Den", SCAN_LOG_WEB_MAX_DAYS);
            return false;
        }
    }
    return true;
}

static esp_err_t scan_log_emit_json_row(httpd_req_t *req, const scan_log_parsed_row_t *row, bool *first)
{
    char wall_ts[40];
    scan_log_ts_field_to_wall_iso(row->ts, wall_ts, sizeof(wall_ts));
    char e_ts[80], e_uid[48], e_nm[128], e_id[128], e_ad[24];
    json_escape(wall_ts[0] ? wall_ts : (row->ts ? row->ts : ""), e_ts, sizeof(e_ts));
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

        char wall_ts[40];
        scan_log_ts_field_to_wall_iso(ts, wall_ts, sizeof(wall_ts));
        char e_ts[80], e_uid[48], e_nm[128], e_id[128], e_act[24];
        html_escape(wall_ts[0] ? wall_ts : ts, e_ts, sizeof(e_ts));
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

/** Loc nhieu ngay / all — tail 48KB khong du; quet xuoi file (PSRAM ~0, 2 pass). */
static bool scan_log_wide_date_filter(const scan_log_json_query_t *jq)
{
    return jq && (jq->show_all || jq->days > 1 || jq->from_ymd[0] != '\0');
}

/**
 * Dem + lay 1 trang (desc/asc) — khong giu toan bo log trong RAM.
 * Tra -2 neu can uu tien quet/Azure.
 */
static int scan_log_fill_page_stream(FILE *fp, const scan_log_json_query_t *jq, bool time_ok,
                                     const char *today_ymd, scan_log_stored_row_t *page_rows, int page_cap,
                                     int *total_out, bool *stream_truncated_out)
{
    if (stream_truncated_out) {
        *stream_truncated_out = false;
    }
    if (total_out) {
        *total_out = 0;
    }
    if (!fp || !jq || !page_rows || page_cap <= 0) {
        return 0;
    }

    int total = 0;
    bool truncated = false;
    if (fseek(fp, 0, SEEK_SET) != 0) {
        return 0;
    }
    while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp)) {
        lv_port_feed_wdt();
        if (app_rfid_swipe_busy() || sd_card_service_waiting() || app_azure_tx_busy()) {
            ESP_LOGW(TAG, "log stream abort — uu tien quet/Azure");
            return -2;
        }
        scan_log_parsed_row_t row;
        if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
            !scan_log_row_matches_query(&row, jq, time_ok, today_ymd)) {
            continue;
        }
        total++;
        if (total >= SCAN_LOG_STREAM_MAX_MATCH) {
            truncated = true;
            break;
        }
    }
    if (stream_truncated_out) {
        *stream_truncated_out = truncated;
    }
    if (total_out) {
        *total_out = total;
    }

    int skip_lo = 0;
    int skip_hi = 0;
    if (jq->sort_mode == SCAN_LOG_SORT_TIME_DESC) {
        skip_lo = total - jq->page * page_cap;
        if (skip_lo < 0) {
            skip_lo = 0;
        }
        skip_hi = total - (jq->page - 1) * page_cap;
        if (skip_hi < 0) {
            return 0;
        }
    } else {
        skip_lo = (jq->page - 1) * page_cap;
        skip_hi = skip_lo + page_cap;
        if (skip_lo >= total) {
            return 0;
        }
        if (skip_hi > total) {
            skip_hi = total;
        }
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        return 0;
    }
    int cur = 0;
    int page_count = 0;
    while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp) && page_count < page_cap) {
        lv_port_feed_wdt();
        if (app_rfid_swipe_busy() || sd_card_service_waiting() || app_azure_tx_busy()) {
            ESP_LOGW(TAG, "log stream pass2 abort");
            return -2;
        }
        scan_log_parsed_row_t row;
        if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
            !scan_log_row_matches_query(&row, jq, time_ok, today_ymd)) {
            continue;
        }
        if (cur >= skip_lo && cur < skip_hi) {
            scan_log_store_row(&row, &page_rows[page_count++]);
        }
        cur++;
        if (cur >= skip_hi) {
            break;
        }
    }
    ESP_LOGI(TAG, "log stream total=%d page=%d rows=%d skip=%d..%d", total, jq->page, page_count, skip_lo,
             skip_hi);
    return page_count;
}


/**
 * TIME_DESC: doc 1 cua so co dinh tu EOF (PSRAM).
 * - Du match trong cua so → nhanh, khong quet ca file.
 * - Thieu match ma chua cham dau file → -1 (fallback full-scan).
 * - covered_all chi khi start == 0.
 * page_rows: cu → moi.
 */
/* Cua so nho + mo rong khi thieu — tranh fread 128KB (SPI bounce lau, Internal bi WiFi/TLS can kip). */
#define SCAN_LOG_TAIL_WIN_START (8 * 1024)
#define SCAN_LOG_TAIL_WIN_MAX   (48 * 1024)
#define SCAN_LOG_TAIL_MAX_NEED  300

/**
 * Doc tu EOF: cua so PSRAM nho, duyet dong moi→cu, dung ngay khi du `need` match.
 * Tra ve so dong trang; -1 = can fallback full-scan.
 */
static int scan_log_fill_page_from_tail(FILE *fp, const scan_log_json_query_t *jq, bool time_ok,
                                        const char *today_ymd, scan_log_stored_row_t *page_rows, int page_cap,
                                        int *total_out)
{
    if (total_out) {
        *total_out = 0;
    }
    if (!fp || !jq || !page_rows || page_cap <= 0) {
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        return 0;
    }
    long fsz = ftell(fp);
    if (fsz <= 0) {
        return 0;
    }

    const int need = jq->page * page_cap;
    if (need <= 0 || need > SCAN_LOG_TAIL_MAX_NEED) {
        return -1;
    }

    scan_log_stored_row_t *newest = (scan_log_stored_row_t *)heap_caps_calloc(
        (size_t)need, sizeof(scan_log_stored_row_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!newest) {
        return -1;
    }

    size_t win = (size_t)SCAN_LOG_TAIL_WIN_START;
    if ((long)win > fsz) {
        win = (size_t)fsz;
    }
    int got = 0;
    bool covered_all = false;
    size_t used_win = win;

    while (1) {
        lv_port_feed_wdt();
        /* Quet the dang cho SD / dang xu ly — nhường ngay, khong mo rong cua so. */
        if (app_rfid_swipe_busy() || sd_card_service_waiting() || app_azure_tx_busy()) {
            free(newest);
            ESP_LOGW(TAG, "log tail abort — uu tien quet/Azure");
            return -2;
        }
        long start = fsz - (long)win;
        if (start < 0) {
            start = 0;
        }
        covered_all = (start == 0);
        size_t n = (size_t)(fsz - start);
        used_win = n;

        char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            free(newest);
            return -1;
        }
        if (fseek(fp, start, SEEK_SET) != 0 || fread(buf, 1, n, fp) != n) {
            free(buf);
            free(newest);
            return -1;
        }
        buf[n] = '\0';

        char *body = buf;
        if (start > 0) {
            char *nl = strchr(buf, '\n');
            if (!nl) {
                free(buf);
                if (covered_all) {
                    got = 0;
                    break;
                }
                goto expand_win;
            }
            body = nl + 1;
        }

        /* Thu thap con tro dong, duyet moi → cu, dung khi du need. */
        int max_lines = (int)(n / 40) + 8;
        if (max_lines < 32) {
            max_lines = 32;
        }
        char **lines = (char **)heap_caps_malloc((size_t)max_lines * sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!lines) {
            free(buf);
            free(newest);
            return -1;
        }
        int nlines = 0;
        char *line = body;
        while (line && *line && nlines < max_lines) {
            char *nl = strchr(line, '\n');
            lines[nlines++] = line;
            if (!nl) {
                break;
            }
            *nl = '\0';
            line = nl + 1;
        }

        got = 0;
        for (int i = nlines - 1; i >= 0 && got < need; i--) {
            scan_log_parsed_row_t row;
            char linebuf[384];
            snprintf(linebuf, sizeof(linebuf), "%s", lines[i]);
            if (scan_log_parse_csv_line(linebuf, &row) &&
                scan_log_row_matches_query(&row, jq, time_ok, today_ymd)) {
                scan_log_store_row(&row, &newest[got++]);
            }
            if ((i & 31) == 0) {
                lv_port_feed_wdt();
            }
        }
        free(lines);
        free(buf);

        if (got >= need || covered_all) {
            break;
        }
expand_win:
        if (win >= (size_t)SCAN_LOG_TAIL_WIN_MAX || (long)win >= fsz) {
            free(newest);
            ESP_LOGI(TAG, "log tail miss need=%d got=%d win=%u — fallback full", need, got, (unsigned)used_win);
            return -1;
        }
        win *= 2;
        if (win > (size_t)SCAN_LOG_TAIL_WIN_MAX) {
            win = (size_t)SCAN_LOG_TAIL_WIN_MAX;
        }
        if ((long)win > fsz) {
            win = (size_t)fsz;
        }
    }

    /* newest[0]=moi nhat … page: cu→moi trong trang (portal reverse khi desc). */
    int start_i = (jq->page - 1) * page_cap;
    int page_count = 0;
    if (start_i < got) {
        int end_i = got < start_i + page_cap ? got : start_i + page_cap;
        for (int i = end_i - 1; i >= start_i; i--) {
            page_rows[page_count++] = newest[i];
        }
    }
    free(newest);

    if (total_out) {
        int base = (jq->page - 1) * page_cap + page_count;
        if (!covered_all && got >= need) {
            *total_out = jq->page * page_cap + 1;
        } else {
            *total_out = base;
        }
    }
    ESP_LOGI(TAG, "log tail-win=%u need=%d got=%d page=%d/%d covered=%d",
             (unsigned)used_win, need, got, page_count, page_cap, (int)covered_all);
    return page_count;
}

esp_err_t scan_log_send_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (portal_reject_log_if_busy(req)) {
        return ESP_OK;
    }

    const uint32_t free_int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t dma0 = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "api/log begin free_int=%u dma=%u", (unsigned)free_int0, (unsigned)dma0);

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

    char limit_err[96];
    if (!scan_log_web_limits_ok(&jq, limit_err, sizeof(limit_err))) {
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\",\"rows\":[]}", limit_err);
        return httpd_resp_sendstr(req, buf);
    }

    const bool field_sort = scan_log_sort_by_field(jq.sort_mode);
    const bool time_desc = (!field_sort && jq.sort_mode == SCAN_LOG_SORT_TIME_DESC);
    const int page_cap = jq.limit > 0 ? jq.limit : 30;

    scan_log_stored_row_t *page_rows = (scan_log_stored_row_t *)heap_caps_calloc(
        (size_t)page_cap, sizeof(scan_log_stored_row_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!page_rows) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Het PSRAM dem trang log\",\"rows\":[]}");
    }

    scan_log_stored_row_t *stored = NULL;
    int stored_count = 0;
    int full_filtered = 0;
    bool sort_truncated = false;
    int total_matched = 0;
    int page_count = 0;
    bool stream_truncated = false;

    if (field_sort) {
        stored = (scan_log_stored_row_t *)heap_caps_calloc(
            SCAN_LOG_SORT_BUF_MAX, sizeof(scan_log_stored_row_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!stored) {
            free(page_rows);
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Het PSRAM sort log\",\"rows\":[]}");
        }
    }

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        free(page_rows);
        free(stored);
        return httpd_resp_sendstr(req, "{\"ok\":true,\"rows\":[],\"total\":0,\"page\":1,\"limit\":30}");
    }

    if (time_desc) {
        int n;
        if (scan_log_wide_date_filter(&jq)) {
            /* 7 ngay / custom: quet file co gioi han dem. */
            n = scan_log_fill_page_stream(fp, &jq, time_ok, today_ymd, page_rows, page_cap, &total_matched,
                                          &stream_truncated);
        } else {
            /* Hom nay: doc cua so tu EOF — nhanh, it IO. */
            n = scan_log_fill_page_from_tail(fp, &jq, time_ok, today_ymd, page_rows, page_cap, &total_matched);
        }
        if (n == -2) {
            fclose(fp);
            sd_card_unlock();
            free(page_rows);
            free(stored);
            return httpd_resp_sendstr(req,
                                      "{\"ok\":false,\"error\":\"Dang quet the / gui Azure — thu lai sau\",\"rows\":[]}");
        }
        if (n >= 0) {
            page_count = n;
        } else {
            /* Fallback: quet xuoi + ring (page*limit qua lon / het PSRAM). */
            int ring_i = 0, ring_n = 0;
            rewind(fp);
            while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp)) {
                lv_port_feed_wdt();
                scan_log_parsed_row_t row;
                if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
                    !scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
                    continue;
                }
                total_matched++;
                scan_log_store_row(&row, &page_rows[ring_i]);
                ring_i = (ring_i + 1) % page_cap;
                if (ring_n < page_cap) {
                    ring_n++;
                }
            }
            page_count = ring_n;
            if (ring_n == page_cap) {
                scan_log_stored_row_t *ordered = (scan_log_stored_row_t *)heap_caps_malloc(
                    (size_t)page_cap * sizeof(scan_log_stored_row_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (ordered) {
                    for (int i = 0; i < page_cap; i++) {
                        ordered[i] = page_rows[(ring_i + i) % page_cap];
                    }
                    memcpy(page_rows, ordered, (size_t)page_cap * sizeof(scan_log_stored_row_t));
                    free(ordered);
                }
            }
            /* total_matched = dem full; trang 1 ring = page_cap dong moi nhat */
            if (jq.page == 1) {
                /* OK */
            } else {
                /* Fallback ring chi dung page 1 — trang >1 can pass 2 */
                int start_idx = 0, end_idx = 0;
                scan_log_page_range(&jq, total_matched, &start_idx, &end_idx);
                page_count = 0;
                rewind(fp);
                int current_match = 0;
                while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp) && page_count < page_cap) {
                    lv_port_feed_wdt();
                    scan_log_parsed_row_t row;
                    if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
                        !scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
                        continue;
                    }
                    if (current_match < start_idx) {
                        current_match++;
                        continue;
                    }
                    if (current_match >= end_idx) {
                        break;
                    }
                    scan_log_store_row(&row, &page_rows[page_count++]);
                    current_match++;
                }
            }
        }
    } else if (field_sort) {
        while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp)) {
            lv_port_feed_wdt();
            scan_log_parsed_row_t row;
            if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
                !scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
                continue;
            }
            full_filtered++;
            if (stored_count < SCAN_LOG_SORT_BUF_MAX) {
                scan_log_store_row(&row, &stored[stored_count++]);
            } else {
                sort_truncated = true;
            }
        }
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
        int start_idx = 0, end_idx = 0;
        scan_log_page_range(&jq, total_matched, &start_idx, &end_idx);
        for (int i = start_idx; i < end_idx && page_count < page_cap; i++) {
            page_rows[page_count++] = stored[i];
        }
        free(stored);
        stored = NULL;
    } else {
        /* TIME_ASC: dem + pass 2 */
        while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp)) {
            lv_port_feed_wdt();
            scan_log_parsed_row_t row;
            if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
                !scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
                continue;
            }
            total_matched++;
        }
        int start_idx = 0, end_idx = 0;
        scan_log_page_range(&jq, total_matched, &start_idx, &end_idx);
        rewind(fp);
        int current_match = 0;
        while (fgets(s_scan_log_line, SCAN_LOG_LINE_SZ, fp) && page_count < page_cap) {
            lv_port_feed_wdt();
            scan_log_parsed_row_t row;
            if (!scan_log_parse_csv_line(s_scan_log_line, &row) ||
                !scan_log_row_matches_query(&row, &jq, time_ok, today_ymd)) {
                continue;
            }
            if (current_match < start_idx) {
                current_match++;
                continue;
            }
            if (current_match >= end_idx) {
                break;
            }
            scan_log_store_row(&row, &page_rows[page_count++]);
            current_match++;
        }
    }

    fclose(fp);
    sd_card_unlock();

    ESP_LOGI(TAG, "api/log after SD free_int=%u dma=%u page=%d total=%d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA), page_count, total_matched);

    char hdr[320];
    if (field_sort && sort_truncated) {
        snprintf(hdr, sizeof(hdr),
                 "{\"ok\":true,\"time_ok\":%s,\"total\":%d,\"total_filtered\":%d,\"page\":%d,\"limit\":%d,"
                 "\"max_page\":%d,\"max_days\":%d,\"stream_truncated\":%s,\"sort\":\"%s\",\"sort_truncated\":true,"
                 "\"rows\":[",
                 time_ok ? "true" : "false", total_matched, full_filtered, jq.page, jq.limit, SCAN_LOG_WEB_MAX_PAGE,
                 SCAN_LOG_WEB_MAX_DAYS, stream_truncated ? "true" : "false",
                 scan_log_sort_mode_str(jq.sort_mode));
    } else {
        snprintf(hdr, sizeof(hdr),
                 "{\"ok\":true,\"time_ok\":%s,\"total\":%d,\"page\":%d,\"limit\":%d,\"max_page\":%d,\"max_days\":%d,"
                 "\"stream_truncated\":%s,\"sort\":\"%s\",\"sort_truncated\":false,\"rows\":[",
                 time_ok ? "true" : "false", total_matched, jq.page, jq.limit, SCAN_LOG_WEB_MAX_PAGE,
                 SCAN_LOG_WEB_MAX_DAYS, stream_truncated ? "true" : "false",
                 scan_log_sort_mode_str(jq.sort_mode));
    }

    esp_err_t e = httpd_resp_send_chunk(req, hdr, strlen(hdr));
    if (e != ESP_OK) {
        free(page_rows);
        return e;
    }

    bool first = true;
    /* TIME_DESC: gui cu→moi trong trang; portal.html se reverse khi sort=desc. */
    for (int i = 0; i < page_count; i++) {
        e = scan_log_emit_json_stored(req, &page_rows[i], &first);
        if (e != ESP_OK) {
            free(page_rows);
            return e;
        }
    }

    free(page_rows);
    e = httpd_resp_send_chunk(req, "]}", 2);
    if (e != ESP_OK) {
        return e;
    }
    e = httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "api/log done free_int=%u dma=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return e;
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
    format_ts_for_csv(ev, ts, sizeof(ts));

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

    /* Cho quet the uu tien: nha khoa SD khi publish/doi, doi swipe xong moi doc tiep. */

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
    int line_n = 0;
    while (fgets(raw, sizeof(raw), fp)) {
        if ((++line_n % 48) == 0) {
            if (esp_task_wdt_status(NULL) == ESP_OK) {
                esp_task_wdt_reset();
            }
            if (app_rfid_swipe_busy() || sd_card_service_waiting()) {
                long pos = ftell(fp);
                fclose(fp);
                fp = NULL;
                sd_card_unlock();
                for (int w = 0; (app_rfid_swipe_busy() || sd_card_service_waiting()) && w < 500; w++) {
                    if (esp_task_wdt_status(NULL) == ESP_OK) {
                        esp_task_wdt_reset();
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                sd_card_lock();
                fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
                if (!fp) {
                    sd_card_unlock();
                    break;
                }
                if (pos >= 0 && fseek(fp, pos, SEEK_SET) != 0) {
                    fclose(fp);
                    fp = NULL;
                    sd_card_unlock();
                    break;
                }
            }
        }

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
        ent.timestamp_utc = (int64_t)scan_log_ts_field_to_utc(fld0);
        strncpy(ent.uid, uid, sizeof(ent.uid) - 1);
        strncpy(ent.name, nm, sizeof(ent.name) - 1);
        strncpy(ent.id, idv, sizeof(ent.id) - 1);

        /* Nha SD truoc publish + delay — quet the duoc ghi log ngay. */
        long pos = ftell(fp);
        fclose(fp);
        fp = NULL;
        sd_card_unlock();

        for (int w = 0; (app_rfid_swipe_busy() || sd_card_service_waiting()) && w < 500; w++) {
            if (esp_task_wdt_status(NULL) == ESP_OK) {
                esp_task_wdt_reset();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

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

        for (int w = 0; (app_rfid_swipe_busy() || sd_card_service_waiting()) && w < 500; w++) {
            if (esp_task_wdt_status(NULL) == ESP_OK) {
                esp_task_wdt_reset();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        sd_card_lock();
        fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
        if (!fp) {
            sd_card_unlock();
            break;
        }
        if (pos >= 0 && fseek(fp, pos, SEEK_SET) != 0) {
            fclose(fp);
            fp = NULL;
            sd_card_unlock();
            break;
        }
    }

    if (fp) {
        fclose(fp);
        sd_card_unlock();
    }

    if (stats_out) {
        *stats_out = st;
    }
    ESP_LOGI(TAG, "Replay tu log: resent=%d (swipe=%d unkn=%d)", st.resent, st.gap_swipe, st.gap_unkn);
    return ESP_OK;
}
