#pragma once

#include <time.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** Gio theo offset dia phuong (board_pins) — khong dung localtime_r/getenv. */
void scan_log_wall_tm(time_t utc, struct tm *out);

/**
 * Cot 1 rfid_log.csv: Unix UTC (giong TimeStamp Azure). Log cu ISO VN van doc duoc.
 * Tra ve 0 neu khong parse duoc / no-ntp.
 */
time_t scan_log_ts_field_to_utc(const char *ts_field);

/** YYYY-MM-DD theo gio VN tu cot ts (epoch hoac ISO cu). */
void scan_log_ts_field_ymd_local(const char *ts_field, char *ymd_out, size_t ymd_sz);

/** ISO gio VN de hien thi (tu epoch hoac ISO cu). */
void scan_log_ts_field_to_wall_iso(const char *ts_field, char *out, size_t outsz);

/**
 * Ghi mot lan quet (can SD da mount). Dinh dang dong: utc_epoch|uid|name|id|reg|index
 * reg: 1 = da dang ky, 0 = chua, -1 = loi SD / khong doc profile.
 */
void scan_log_append(const char *uid_hex, const char *name, const char *id, int registered, int32_t msg_idx);

/**
 * Ghi thao tac quan tri (luu/xoa the tu web/LCD/backend DM).
 * action: "SAVE" hoac "DEL"
 * msg_idx: index telemetry 603/604 (idx_admin); <=0 thi tu cap phat.
 * CSV: utc_epoch|uid|name|id|99|SAVE|index
 */
void scan_log_append_admin(const char *uid_hex, const char *name, const char *id, const char *action,
                           int32_t msg_idx);

/** Ghi xuong SD cac ban ghi tam tu NVS khi SD mount. */
void scan_log_flush_pending(void);

/**
 * Cat rfid_log.csv, chi giu khoang 90 ngay gan nhat (goi sau mount SD / flush).
 */
void scan_log_trim_at_boot(void);

/** Trang /scans — HTML tinh + fetch /api/scans (CSR). Can SD mount. */
esp_err_t scan_log_send_html_page(httpd_req_t *req);

/**
 * JSON nhat ky: {"rows":[{"ts","name","id","admin","code","index"},...]}
 * Endpoint: /api/log hoac /api/scans (giong nhau).
 * Query: page, limit (mac dinh 30, toi da 30/trang), days=N (toi da 30), from/to (span toi da 30 ngay),
 *        all=1 (tu thu ve 7 ngay), code=601|602|603|604,
 *        sort=desc (web chi Moi nhat / tail EOF — khong full-scan).
 * Web: toi da 10 trang (300 dong); log day du tren SD / Azure 605.
 */
esp_err_t scan_log_send_json(httpd_req_t *req);

/** Huy quet /api/log dang chay (doi tab portal) — giai phong Internal. */
void scan_log_api_cancel_inflight(void);

/** true khi task nen dang quet SD cho /api/log. */
bool scan_log_api_is_busy(void);

/** Muc tieu resend tu rfid_log.csv (605 sync). */
#define SCAN_LOG_SYNC_MISSING_MAX 64

typedef struct {
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
} scan_log_sync_filter_t;

typedef struct {
    int event_code;
    int32_t index;
    int64_t timestamp_utc;
    char uid[32];
    char name[48];
    char id[48];
} scan_log_replay_entry_t;

typedef int (*scan_log_replay_publish_fn)(const scan_log_replay_entry_t *entry, void *ctx);

typedef struct {
    int resent;
    int gap_swipe;
    int gap_unkn;
} scan_log_replay_stats_t;

/**
 * Doc rfid_log.csv, gui lai ban ghi backend thieu (index > LastIdx* hoac trong Missing[]).
 * publish_fn tra ve 0 neu OK, !=0 de dung som.
 */
esp_err_t scan_log_replay_gaps(const scan_log_sync_filter_t *filter, scan_log_replay_publish_fn publish_fn,
                               void *ctx, scan_log_replay_stats_t *stats_out);
