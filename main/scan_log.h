#pragma once

#include <time.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** Gio theo offset dia phuong (board_pins) — khong dung localtime_r/getenv. */
void scan_log_wall_tm(time_t utc, struct tm *out);

/**
 * Ghi mot lan quet (can SD da mount). Dinh dang dong: ts|uid|name|id|reg
 * reg: 1 = da dang ky, 0 = chua, -1 = loi SD / khong doc profile.
 */
void scan_log_append(const char *uid_hex, const char *name, const char *id, int registered);

/**
 * Ghi thao tac quan tri (luu/xoa the tu man hinh cam ung).
 * action: "SAVE" hoac "DEL"
 */
void scan_log_append_admin(const char *uid_hex, const char *name, const char *id, const char *action);

/** Ghi xuong SD cac ban ghi tam tu NVS khi SD mount. */
void scan_log_flush_pending(void);

/**
 * Cat rfid_log.csv, chi giu khoang 60 ngay gan nhat (goi sau mount SD / flush).
 * May lau khong quet the van xoa ban ghi cu.
 */
void scan_log_trim_at_boot(void);

/** Trang HTML bang nhat ky (chunked). Can SD + file co the trong. */
esp_err_t scan_log_send_html_page(httpd_req_t *req);
