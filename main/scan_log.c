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
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sd_card.h"
#include "esp_task_wdt.h"
#include "lv_port.h"
#include "portal_web.h"
#include "app_rfid.h"
#include <sys/stat.h>
#include <sys/socket.h>

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
/** Doc dong CSV — PSRAM (tail window / linebuf). */
/** 1 dong JSON ~640B; trang toi da 30 dong + hdr — 1 chunk HTTP (khong cap Internal moi lan send). */
#define SCAN_LOG_JSON_ROW_MAX 640
#define SCAN_LOG_JSON_BUF_SZ (384 + 30 * SCAN_LOG_JSON_ROW_MAX + 8)
EXT_RAM_BSS_ATTR static char s_log_json_chunk[SCAN_LOG_JSON_ROW_MAX];
EXT_RAM_BSS_ATTR static char s_log_json_buf[SCAN_LOG_JSON_BUF_SZ];
EXT_RAM_BSS_ATTR static char s_log_file_buf[512];
EXT_RAM_BSS_ATTR static char s_log_esc_ts[80];
EXT_RAM_BSS_ATTR static char s_log_esc_uid[48];
EXT_RAM_BSS_ATTR static char s_log_esc_nm[128];
EXT_RAM_BSS_ATTR static char s_log_esc_id[128];
EXT_RAM_BSS_ATTR static char s_log_esc_ad[24];

static volatile uint32_t s_log_api_gen;

static bool scan_log_api_aborted(uint32_t my_gen)
{
    return s_log_api_gen != my_gen;
}

/** Tra -3 huy, -2 uu tien quet/Azure, 0 tiep tuc. */
static int scan_log_yield_or_abort(uint32_t my_gen)
{
    static uint32_t s_line_ctr;
    lv_port_feed_wdt();
    if ((++s_line_ctr & 15u) == 0) {
        vTaskDelay(1);
    }
    if (scan_log_api_aborted(my_gen)) {
        return -3;
    }
    if (app_rfid_swipe_busy() || sd_card_service_waiting()) {
        for (int w = 0; (app_rfid_swipe_busy() || sd_card_service_waiting()) && w < 300; w++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (scan_log_api_aborted(my_gen)) {
                return -3;
            }
        }
        if (app_rfid_swipe_busy() || sd_card_service_waiting()) {
            return -2;
        }
    }
    if (app_azure_tx_busy()) {
        vTaskDelay(1);
    }
    return 0;
}

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
    char wall_iso[40] = "";
    scan_log_ts_field_to_wall_iso(ts_field, wall_iso, sizeof(wall_iso));
    if (wall_iso[0] != '\0' && strlen(wall_iso) >= 10) {
        snprintf(ymd_out, ymd_sz, "%.10s", wall_iso);
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
#define SCAN_LOG_WEB_MAX_DAYS 30
/**
 * Web chi doc tail EOF — toi da 10 trang (300 dong).
 * KHONG cho ring/full-scan (nguyen nhan reset Internal/WDT).
 */
#define SCAN_LOG_WEB_MAX_PAGE 100
#define SCAN_LOG_STREAM_MAX_MATCH (SCAN_LOG_WEB_MAX_PAGE * SCAN_LOG_PAGE_LIMIT_MAX)
#define PORTAL_LOG_MIN_INTERNAL (16u * 1024u)
#define SCAN_LOG_TAIL_WIN_START   (8 * 1024)
#define SCAN_LOG_TAIL_WIN_MAX     (128 * 1024)
#define SCAN_LOG_TAIL_MAX_NEED    (SCAN_LOG_WEB_MAX_PAGE * SCAN_LOG_PAGE_LIMIT_MAX)
#define SCAN_LOG_TAIL_LINE_PTR_MAX 4096
#define SCAN_LOG_API_ERR_HARD (-4)

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

enum {
    LOG_JOB_IDLE = 0,
    LOG_JOB_RUNNING,
    LOG_JOB_DONE,
    LOG_JOB_ERR,
};

typedef struct {
    volatile uint8_t state;
    scan_log_json_query_t q;
    bool time_ok;
    char today_ymd[16];
    uint32_t gen;
    int page_count;
    int total_matched;
    int full_filtered;
    bool stream_truncated;
    bool sort_truncated;
    char err[96];
} scan_log_job_meta_t;

/** Ket qua quet — BSS PSRAM; web chi tail (khong ring full-file). */
static EXT_RAM_BSS_ATTR scan_log_stored_row_t s_log_job_rows[SCAN_LOG_PAGE_LIMIT_MAX];
static EXT_RAM_BSS_ATTR scan_log_job_meta_t s_log_job;
static EXT_RAM_BSS_ATTR scan_log_stored_row_t s_log_tail_rows[SCAN_LOG_TAIL_MAX_NEED];
static EXT_RAM_BSS_ATTR char s_log_tail_win[SCAN_LOG_TAIL_WIN_MAX + 1];
static EXT_RAM_BSS_ATTR char *s_log_tail_line_ptrs[SCAN_LOG_TAIL_LINE_PTR_MAX];
static EXT_RAM_BSS_ATTR char s_log_tail_linebuf[SCAN_LOG_LINE_SZ];

void scan_log_api_cancel_inflight(void)
{
    if (s_log_job.state != LOG_JOB_RUNNING) {
        return;
    }
    s_log_api_gen++;
    ESP_LOGD(TAG, "log api cancel — int_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool scan_log_api_is_busy(void)
{
    return s_log_job.state == LOG_JOB_RUNNING;
}

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
        return true;
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

/** Append 1 row JSON vao s_log_json_buf. Tra false neu tran buffer. */
static bool scan_log_append_json_stored(size_t *off, const scan_log_stored_row_t *row, bool *first)
{
    if (!off || !row || !first || *off >= sizeof(s_log_json_buf)) {
        return false;
    }
    char wall_ts[40];
    scan_log_ts_field_to_wall_iso(row->ts, wall_ts, sizeof(wall_ts));
    json_escape(wall_ts[0] ? wall_ts : row->ts, s_log_esc_ts, sizeof(s_log_esc_ts));
    json_escape(row->uid, s_log_esc_uid, sizeof(s_log_esc_uid));
    json_escape(row->name, s_log_esc_nm, sizeof(s_log_esc_nm));
    json_escape(row->id, s_log_esc_id, sizeof(s_log_esc_id));
    json_escape(row->admin_act, s_log_esc_ad, sizeof(s_log_esc_ad));

    const int n = snprintf(s_log_json_chunk, sizeof(s_log_json_chunk),
                           "%s{\"ts\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"admin\":%s,\"action\":\"%s\","
                           "\"code\":%d,\"index\":%ld}",
                           *first ? "" : ",", s_log_esc_ts, s_log_esc_uid, s_log_esc_nm, s_log_esc_id,
                           row->is_admin ? "true" : "false", s_log_esc_ad, row->event_code, row->msg_index);
    if (n <= 0 || (size_t)n >= sizeof(s_log_json_chunk)) {
        return false;
    }
    if (*off + (size_t)n >= sizeof(s_log_json_buf)) {
        return false;
    }
    memcpy(s_log_json_buf + *off, s_log_json_chunk, (size_t)n);
    *off += (size_t)n;
    *first = false;
    return true;
}

static esp_err_t scan_log_json_done(httpd_req_t *req, esp_err_t send_err)
{
    if (req) {
        const int sock = httpd_req_to_sockfd(req);
        if (sock >= 0) {
            (void)httpd_sess_trigger_close(req->handle, sock);
        }
    }
    return send_err;
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
    if (q->from_ymd[0] && !q->to_ymd[0]) {
        strncpy(q->to_ymd, q->from_ymd, sizeof(q->to_ymd) - 1);
        q->to_ymd[sizeof(q->to_ymd) - 1] = '\0';
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

    if (q->days > 0 && time_ok && !q->show_all && (!q->from_ymd[0] || !q->to_ymd[0])) {
        time_t now = time(NULL);
        struct tm tnow;
        scan_log_wall_tm(now, &tnow);
        scan_log_format_ymd(&tnow, q->to_ymd, sizeof(q->to_ymd));
        if (q->days <= 1) {
            scan_log_format_ymd(&tnow, q->from_ymd, sizeof(q->from_ymd));
        } else {
            /* N ngay = hom nay + (N-1) ngay truoc — tranh span=N+1 bi tu choi. */
            time_t from_utc = now - (time_t)(q->days - 1) * 86400;
            struct tm tfrom;
            scan_log_wall_tm(from_utc, &tfrom);
            scan_log_format_ymd(&tfrom, q->from_ymd, sizeof(q->from_ymd));
        }
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

/** Loc nhieu ngay — tail/ring; mot ngay (Tu=Den / hom nay) dung tail tu EOF. */
static bool scan_log_wide_date_filter(const scan_log_json_query_t *jq)
{
    if (!jq) {
        return false;
    }
    if (jq->show_all) {
        return true;
    }
    if (jq->days > 1) {
        return true;
    }
    if (jq->from_ymd[0] && jq->to_ymd[0] && strcmp(jq->from_ymd, jq->to_ymd) != 0) {
        return true;
    }
    return false;
}

/** Gioi han portal: toi da 30 ngay, 10 trang (300 dong) — chi tail EOF. */
static bool scan_log_use_tail_first(const scan_log_json_query_t *jq, const char *today_ymd)
{
    (void)today_ymd;
    if (!jq) {
        return false;
    }
    const int page_cap = jq->limit > 0 ? jq->limit : SCAN_LOG_PAGE_LIMIT_MAX;
    const int need = jq->page * page_cap;
    return need > 0 && need <= SCAN_LOG_TAIL_MAX_NEED;
}

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
    /* Web chi Moi nhat + tail — asc/index/id se full-scan → reset. */
    if (jq->sort_mode != SCAN_LOG_SORT_TIME_DESC) {
        snprintf(err, err_sz, "Web chi sap xep Moi nhat (tranh quet full SD / reset)");
        return false;
    }
    if (jq->page > SCAN_LOG_WEB_MAX_PAGE) {
        snprintf(err, err_sz, "Toi da %d trang (%d dong) tren web (chi doc cuoi file)", SCAN_LOG_WEB_MAX_PAGE,
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

/** /scans — HTML tinh + fetch /api/scans (CSR, khong SSR while(fgets)). */
static const char SCANS_CSR_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>Nhật ký RFID</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,sans-serif;background:#090d16;color:#f3f4f6;padding:24px;line-height:1.5}"
    "h1{font-size:22px;font-weight:600;margin-bottom:16px;color:#fff;border-bottom:1px solid rgba(255,255,255,.08);padding-bottom:12px}"
    ".nav{margin:12px 0;display:flex;flex-wrap:wrap;gap:8px;align-items:center}"
    ".nav a{color:#6366f1;text-decoration:none;font-size:14px;font-weight:500}"
    ".hint{font-size:12px;color:#9ca3af;margin:8px 0 16px}"
    "table{width:100%;border-collapse:collapse;font-size:13px;max-width:900px;border-radius:8px;overflow:hidden;margin-top:10px}"
    "th{background:#0f172a;color:#9ca3af;font-weight:600;text-transform:uppercase;font-size:11px;padding:12px 16px;text-align:left}"
    "td{padding:12px 16px;border-bottom:1px solid rgba(255,255,255,.04);color:#e5e7eb}"
    "tr:nth-child(even){background:rgba(255,255,255,.01)}"
    ".admin-save{background:rgba(16,185,129,.08)!important;font-style:italic}"
    ".admin-del{background:rgba(239,68,68,.08)!important;font-style:italic}"
    "button{background:#6366f1;color:#fff;border:none;border-radius:6px;padding:8px 14px;font-size:13px;cursor:pointer}"
    "button:disabled{opacity:.45;cursor:not-allowed}"
    "</style></head><body>"
    "<h1 id=title>Nhật ký quét thẻ</h1>"
    "<p class=nav><a href=\"/\">&larr; Về portal</a><span id=modeLink></span></p>"
    "<p class=hint id=status>Đang tải...</p>"
    "<table id=logTable><thead><tr>"
    "<th>Code</th><th>Index</th><th>Thời gian</th><th>Tên nhân viên</th><th>Mã / Thao tác</th>"
    "</tr></thead><tbody></tbody></table>"
    "<p class=nav>"
    "<button type=button id=prevBtn>Trang trước</button>"
    "<button type=button id=nextBtn>Trang sau</button>"
    "<button type=button id=reloadBtn>Tải lại</button>"
    "<span id=pageInfo style=\"font-size:13px;color:#9ca3af\"></span>"
    "</p>"
    "<script>"
    "(function(){"
    "var cur=1,loading=false;"
    "function qs(){return new URLSearchParams(location.search);}"
    "function tok(){var p=qs();if(p.get('token'))return p.get('token');"
    "try{var t=JSON.parse(sessionStorage.getItem('portal_tokens')||'{}');return t.log||t.admin||'';}catch(e){return '';}}"
    "function wide(){return qs().get('all')==='1';}"
    "function esc(s){return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
    "function act(r){if(r.admin){var l=r.action==='DEL'?'[XOA THE]':'[LUU THE]';"
    "return esc(l+' '+(r.id||r.uid||'')+' ('+(r.name||r.uid||'')+')');}return esc(r.id||'');}"
    "function row(r){var c=r.admin?(r.action==='DEL'?' class=\"admin-del\"':' class=\"admin-save\"'):'';"
    "return '<tr'+c+'><td>'+esc(r.code)+'</td><td>'+esc(r.index)+'</td><td>'+esc(r.ts)+'</td>"
    "+'<td>'+esc(r.name||r.uid)+'</td><td>'+act(r)+'</td></tr>';}"
    "function apiUrl(p){var u='/api/scans?page='+p+'&limit=30';if(wide())u+='&all=1';return u;}"
    "function loadLogs(p){"
    "if(p<1||loading)return;loading=true;document.getElementById('status').textContent='Đang tải...';"
    "var h={'Accept':'application/json'},t=tok();if(t)h['X-Portal-Token']=t;"
    "fetch(apiUrl(p),{headers:h}).then(function(r){return r.json();}).then(function(d){"
    "loading=false;"
    "if(!d.ok){document.getElementById('status').textContent=d.error||'Lỗi';"
    "document.querySelector('#logTable tbody').innerHTML='';return;}"
    "cur=p;var tb=document.querySelector('#logTable tbody');"
    "if(!d.rows||!d.rows.length)tb.innerHTML='<tr><td colspan=\"5\"><em>Chưa có dữ liệu.</em></td></tr>';"
    "else tb.innerHTML=d.rows.map(row).join('');"
    "var mp=d.max_page||1;document.getElementById('pageInfo').textContent=' Trang '+cur+' / '+mp;"
    "document.getElementById('status').textContent='Tổng '+ (d.total||0) +' dòng — bấm Tải lại để cập nhật';"
    "document.getElementById('prevBtn').disabled=cur<=1;"
    "document.getElementById('nextBtn').disabled=cur>=mp;}).catch(function(){"
    "loading=false;document.getElementById('status').textContent='Lỗi kết nối';});}"
    "function modeLink(){"
    "var p=qs(),t=p.get('token'),q=t?'?token='+encodeURIComponent(t):'';"
    "var el=document.getElementById('modeLink');"
    "if(wide())el.innerHTML=' | <a href=\"/scans'+q+'\">Chỉ hôm nay</a>';"
    "else el.innerHTML=' | <a href=\"/scans?all=1'+(t?'&token='+encodeURIComponent(t):'')+'\">30 ngày gần nhất</a>';"
    "document.getElementById('title').textContent=wide()?'Nhật ký 30 ngày':'Quét thẻ hôm nay';}"
    "document.getElementById('prevBtn').onclick=function(){loadLogs(cur-1);};"
    "document.getElementById('nextBtn').onclick=function(){loadLogs(cur+1);};"
    "document.getElementById('reloadBtn').onclick=function(){loadLogs(cur);};"
    "modeLink();loadLogs(1);"
    "})();"
    "</script></body></html>";

esp_err_t scan_log_send_html_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (!sd_card_is_mounted()) {
        return httpd_resp_sendstr(req,
                                  "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                  "<meta name=\"viewport\" content=\"width=device-width\">"
                                  "<style>body{font-family:sans-serif;background:#090d16;color:#f3f4f6;padding:24px;text-align:center}"
                                  "a{color:#6366f1;text-decoration:none}</style></head><body>"
                                  "<h3>Thẻ SD chưa gắn hoặc chưa mount.</h3>"
                                  "<p style=\"margin-top:12px\"><a href=\"/\">Về portal</a></p></body></html>");
    }
    return httpd_resp_sendstr(req, SCANS_CSR_HTML);
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

/**
 * TIME_DESC: doc 1 cua so co dinh tu EOF (PSRAM).
 * - Du match trong cua so → nhanh, khong quet ca file.
 * - Thieu match ma chua cham dau file → -1 (web: BAO LOI, khong fallback ring).
 * - covered_all chi khi start == 0.
 * page_rows: cu → moi.
 */
/* Cua so nho + mo rong khi thieu — tranh fread 128KB (SPI bounce lau, Internal bi WiFi/TLS can kip). */
/**
 * Doc tu EOF: cua so PSRAM nho, duyet dong moi→cu, dung ngay khi du `need` match.
 * Tra ve so dong trang; -1 = thieu (web khong duoc full-scan).
 */
static int scan_log_fill_page_from_tail(FILE *fp, const scan_log_json_query_t *jq, bool time_ok,
                                        const char *today_ymd, scan_log_stored_row_t *page_rows, int page_cap,
                                        uint32_t api_gen, int *total_out)
{
    if (total_out) {
        *total_out = 0;
    }
    if (!fp || !jq || !page_rows || page_cap <= 0) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        return -1;
    }
    long fsz = ftell(fp);
    if (fsz <= 0) {
        return 0;
    }

    int need = jq->page * page_cap;
    if (need > SCAN_LOG_TAIL_MAX_NEED) {
        need = SCAN_LOG_TAIL_MAX_NEED;
    }

    scan_log_stored_row_t *newest = s_log_tail_rows;
    int got = 0;
    long pos = fsz;
    const size_t max_chunk = SCAN_LOG_TAIL_WIN_MAX;

    int scan_target_max = SCAN_LOG_TAIL_MAX_NEED;

    while (pos > 0 && got < scan_target_max) {
        int y = scan_log_yield_or_abort(api_gen);
        if (y != 0) {
            return y;
        }

        long chunk_start = pos - (long)max_chunk;
        if (chunk_start < 0) {
            chunk_start = 0;
        }
        size_t n = (size_t)(pos - chunk_start);
        pos = chunk_start;

        char *buf = s_log_tail_win;
        if (fseek(fp, chunk_start, SEEK_SET) != 0 || fread(buf, 1, n, fp) != n) {
            break;
        }
        buf[n] = '\0';

        char *body = buf;
        if (chunk_start > 0) {
            char *nl = strchr(buf, '\n');
            if (nl) {
                body = nl + 1;
            }
        }

        int max_lines = (int)(n / 40) + 8;
        if (max_lines < 32) max_lines = 32;
        if (max_lines > SCAN_LOG_TAIL_LINE_PTR_MAX) max_lines = SCAN_LOG_TAIL_LINE_PTR_MAX;

        char **lines = s_log_tail_line_ptrs;
        int nlines = 0;
        char *line = body;
        while (line && *line && nlines < max_lines) {
            char *nl = strchr(line, '\n');
            lines[nlines++] = line;
            if (!nl) break;
            *nl = '\0';
            line = nl + 1;
        }

        for (int i = nlines - 1; i >= 0 && got < scan_target_max; i--) {
            scan_log_parsed_row_t row;
            snprintf(s_log_tail_linebuf, sizeof(s_log_tail_linebuf), "%s", lines[i]);
            if (scan_log_parse_csv_line(s_log_tail_linebuf, &row) &&
                scan_log_row_matches_query(&row, jq, time_ok, today_ymd)) {
                scan_log_store_row(&row, &newest[got++]);
            }
            if ((i & 31) == 0) {
                lv_port_feed_wdt();
            }
        }

        vTaskDelay(1);
    }

    /* Neu loc theo ngay khong ra dong nao (va gio chua sync), fallback lay cac dong moi nhat */
    if (got == 0 && jq->page == 1 && !time_ok && !jq->from_ymd[0] && !jq->to_ymd[0]) {
        pos = fsz;
        while (pos > 0 && got < scan_target_max) {
            long chunk_start = pos - (long)max_chunk;
            if (chunk_start < 0) chunk_start = 0;
            size_t n = (size_t)(pos - chunk_start);
            pos = chunk_start;
            char *buf = s_log_tail_win;
            if (fseek(fp, chunk_start, SEEK_SET) != 0 || fread(buf, 1, n, fp) != n) break;
            buf[n] = '\0';
            char *body = buf;
            if (chunk_start > 0) {
                char *nl = strchr(buf, '\n');
                if (nl) body = nl + 1;
            }
            int max_lines = (int)(n / 40) + 8;
            if (max_lines < 32) max_lines = 32;
            if (max_lines > SCAN_LOG_TAIL_LINE_PTR_MAX) max_lines = SCAN_LOG_TAIL_LINE_PTR_MAX;
            char **lines = s_log_tail_line_ptrs;
            int nlines = 0;
            char *line = body;
            while (line && *line && nlines < max_lines) {
                char *nl = strchr(line, '\n');
                lines[nlines++] = line;
                if (!nl) break;
                *nl = '\0';
                line = nl + 1;
            }
            for (int i = nlines - 1; i >= 0 && got < scan_target_max; i--) {
                scan_log_parsed_row_t row;
                snprintf(s_log_tail_linebuf, sizeof(s_log_tail_linebuf), "%s", lines[i]);
                if (scan_log_parse_csv_line(s_log_tail_linebuf, &row)) {
                    scan_log_store_row(&row, &newest[got++]);
                }
            }
        }
    }

    int start_i = (jq->page - 1) * page_cap;
    int page_count = 0;
    if (start_i < got) {
        int end_i = got < start_i + page_cap ? got : start_i + page_cap;
        for (int i = end_i - 1; i >= start_i; i--) {
            page_rows[page_count++] = newest[i];
        }
    }
    if (total_out) {
        *total_out = got;
    }
    ESP_LOGI(TAG, "log tail need=%d got=%d page=%d/%d", need, got, page_count, page_cap);
    return page_count;
}

static bool scan_log_query_same(const scan_log_json_query_t *a, const scan_log_json_query_t *b)
{
    if (!a || !b) {
        return false;
    }
    return a->show_all == b->show_all && a->sort_mode == b->sort_mode && a->days == b->days &&
           a->type_code == b->type_code && a->limit == b->limit && a->page == b->page &&
           strcmp(a->from_ymd, b->from_ymd) == 0 && strcmp(a->to_ymd, b->to_ymd) == 0;
}

static esp_err_t scan_log_send_json_rows(httpd_req_t *req, const scan_log_json_query_t *jq, bool time_ok,
                                         int page_count, int total_matched, int full_filtered, bool stream_truncated,
                                         bool sort_truncated, const scan_log_stored_row_t *page_rows)
{
    size_t off = 0;
    int n;
    if (sort_truncated) {
        n = snprintf(s_log_json_buf, sizeof(s_log_json_buf),
                     "{\"ok\":true,\"time_ok\":%s,\"total\":%d,\"total_filtered\":%d,\"page\":%d,\"limit\":%d,"
                     "\"max_page\":%d,\"max_days\":%d,\"stream_truncated\":%s,\"sort\":\"%s\",\"sort_truncated\":true,"
                     "\"rows\":[",
                     time_ok ? "true" : "false", total_matched, full_filtered, jq->page, jq->limit, SCAN_LOG_WEB_MAX_PAGE,
                     SCAN_LOG_WEB_MAX_DAYS, stream_truncated ? "true" : "false", scan_log_sort_mode_str(jq->sort_mode));
    } else {
        n = snprintf(s_log_json_buf, sizeof(s_log_json_buf),
                     "{\"ok\":true,\"time_ok\":%s,\"total\":%d,\"page\":%d,\"limit\":%d,\"max_page\":%d,\"max_days\":%d,"
                     "\"stream_truncated\":%s,\"sort\":\"%s\",\"sort_truncated\":false,\"rows\":[",
                     time_ok ? "true" : "false", total_matched, jq->page, jq->limit, SCAN_LOG_WEB_MAX_PAGE,
                     SCAN_LOG_WEB_MAX_DAYS, stream_truncated ? "true" : "false", scan_log_sort_mode_str(jq->sort_mode));
    }
    if (n <= 0 || (size_t)n >= sizeof(s_log_json_buf)) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON buffer overflow\",\"rows\":[]}");
    }
    off = (size_t)n;

    bool first = true;
    for (int i = 0; i < page_count; i++) {
        if (!scan_log_append_json_stored(&off, &page_rows[i], &first)) {
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON buffer overflow\",\"rows\":[]}");
        }
    }
    if (off + 2 >= sizeof(s_log_json_buf)) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON buffer overflow\",\"rows\":[]}");
    }
    s_log_json_buf[off++] = ']';
    s_log_json_buf[off++] = '}';

    /* Gui theo chunk 1KB de LWIP/httpd khong cap phat khoi Internal RAM lon gay reset */
    size_t sent = 0;
    while (sent < off) {
        size_t chunk_sz = (off - sent > 1024) ? 1024 : (off - sent);
        esp_err_t e = httpd_resp_send_chunk(req, s_log_json_buf + sent, chunk_sz);
        if (e != ESP_OK) {
            return e;
        }
        sent += chunk_sz;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/** Quet SD vao s_log_job_rows — web CHI tail EOF (khong ring/full-scan). Tra 0 OK, -3 huy, -2 busy, -4 hard refuse. */
static int scan_log_execute_query_bg(const scan_log_json_query_t *jq, bool time_ok, const char *today_ymd,
                                     uint32_t my_gen)
{
    const bool field_sort = scan_log_sort_by_field(jq->sort_mode);
    const bool time_desc = (!field_sort && jq->sort_mode == SCAN_LOG_SORT_TIME_DESC);
    const int page_cap = jq->limit > 0 ? jq->limit : 30;

    s_log_job.page_count = 0;
    s_log_job.total_matched = 0;
    s_log_job.full_filtered = 0;
    s_log_job.stream_truncated = false;
    s_log_job.sort_truncated = false;
    s_log_job.err[0] = '\0';

    /* Rang cung: khong cho duong full-scan (nguyen nhan reset). */
    if (!time_desc || !scan_log_use_tail_first(jq, today_ymd)) {
        snprintf(s_log_job.err, sizeof(s_log_job.err),
                 "Web chi doc cuoi file (toi da %d trang, Moi nhat) — khong quet full SD",
                 SCAN_LOG_WEB_MAX_PAGE);
        return SCAN_LOG_API_ERR_HARD;
    }

    portal_web_log_suppress(true);
    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        portal_web_log_suppress(false);
        return 0;
    }
    (void)setvbuf(fp, s_log_file_buf, _IOFBF, sizeof(s_log_file_buf));

    int rc = 0;
    int n = scan_log_fill_page_from_tail(fp, jq, time_ok, today_ymd, s_log_job_rows, page_cap, my_gen,
                                         &s_log_job.total_matched);
    if (n == -3 || n == -2) {
        rc = n;
        goto done;
    }
    if (n < 0) {
        n = 0;
    }
    s_log_job.page_count = n;
    rc = 0;

done:
    fclose(fp);
    sd_card_unlock();
    portal_web_log_suppress(false);
    vTaskDelay(1);
    return rc;
}

esp_err_t scan_log_send_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (portal_reject_log_if_busy(req)) {
        return scan_log_json_done(req, ESP_OK);
    }

    if (!sd_card_is_mounted()) {
        return scan_log_json_done(req, httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD chua mount\",\"rows\":[]}"));
    }

    time_t now = time(NULL);
    struct tm tloc;
    scan_log_wall_tm(now, &tloc);
    const bool time_ok = (tloc.tm_year >= (2020 - 1900));
    char today_ymd[16];
    scan_log_format_ymd(&tloc, today_ymd, sizeof(today_ymd));

    scan_log_json_query_t jq;
    scan_log_json_parse_query(req, &jq, time_ok, today_ymd);
    const uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "api/log p=%d days=%d free_int=%u sort=%d (tail-only)", jq.page, jq.days, (unsigned)int_free,
             (int)jq.sort_mode);
    if (int_free < PORTAL_LOG_MIN_INTERNAL) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":false,\"error\":\"Internal RAM thap (%u B) — doi 15s hoac reboot thiet bi\",\"rows\":[]}",
                 (unsigned)int_free);
        return scan_log_json_done(req, httpd_resp_sendstr(req, buf));
    }

    char limit_err[96];
    if (!scan_log_web_limits_ok(&jq, limit_err, sizeof(limit_err))) {
        char buf[192];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\",\"rows\":[]}", limit_err);
        return scan_log_json_done(req, httpd_resp_sendstr(req, buf));
    }

    if (s_log_job.state == LOG_JOB_DONE && scan_log_query_same(&s_log_job.q, &jq)) {
        return scan_log_json_done(req, scan_log_send_json_rows(req, &s_log_job.q, s_log_job.time_ok, s_log_job.page_count,
                                                                s_log_job.total_matched, s_log_job.full_filtered,
                                                                s_log_job.stream_truncated, s_log_job.sort_truncated,
                                                                s_log_job_rows));
    }

    s_log_job.q = jq;
    s_log_job.time_ok = time_ok;
    snprintf(s_log_job.today_ymd, sizeof(s_log_job.today_ymd), "%s", today_ymd);
    s_log_job.gen = ++s_log_api_gen;
    s_log_job.state = LOG_JOB_RUNNING;
    s_log_job.page_count = 0;
    s_log_job.total_matched = 0;
    s_log_job.full_filtered = 0;
    s_log_job.stream_truncated = false;
    s_log_job.sort_truncated = false;
    s_log_job.err[0] = '\0';

    const int rc = scan_log_execute_query_bg(&jq, time_ok, today_ymd, s_log_job.gen);

    if (rc == -3) {
        s_log_job.state = LOG_JOB_IDLE;
        return scan_log_json_done(req, httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Da huy\",\"rows\":[]}"));
    }
    if (rc == -2) {
        s_log_job.state = LOG_JOB_IDLE;
        return scan_log_json_done(req,
                                  httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Dang quet the — thu lai sau\",\"rows\":[]}"));
    }
    if (rc == SCAN_LOG_API_ERR_HARD) {
        s_log_job.state = LOG_JOB_IDLE;
        char buf[256];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\",\"rows\":[]}",
                 s_log_job.err[0] ? s_log_job.err : "Web tu choi quet full SD");
        return scan_log_json_done(req, httpd_resp_sendstr(req, buf));
    }

    s_log_job.state = LOG_JOB_DONE;
    return scan_log_json_done(req, scan_log_send_json_rows(req, &s_log_job.q, s_log_job.time_ok, s_log_job.page_count,
                                                            s_log_job.total_matched, s_log_job.full_filtered,
                                                            s_log_job.stream_truncated, s_log_job.sort_truncated,
                                                            s_log_job_rows));
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
