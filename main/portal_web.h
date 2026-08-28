#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Trang chinh: menu trai + noi dung (khong Basic Auth). */
esp_err_t portal_root_get_handler(httpd_req_t *req);

/** POST /api/unlock  body: section=wifi&pin=1234  -> JSON {ok,token,section} */
esp_err_t portal_unlock_post_handler(httpd_req_t *req);

/** Kiem tra header X-Portal-Token hop le cho section (vd "wifi", "log", "cards"). */
bool portal_auth_section(httpd_req_t *req, const char *section);

/** Log Terminal / OTA — can PIN van nang (ADMIN/1411). */
bool portal_auth_master_section(httpd_req_t *req, const char *section);

/** Nhan body POST ngan (vd form urlencoded); tra ve so byte hoac -1. */
int portal_recv_small_body(httpd_req_t *req, char *buf, size_t bufsz);

/** Bat hook log som (main) — ring 1KB chay lien tuc, khong can PSRAM. */
void portal_web_log_init(void);

/** Dang ky them URI (/api/cards, /api/log, /api/pin_change, /api/azure, ...). */
void portal_web_register_handlers(httpd_handle_t server);

/**
 * Chan API tong quan/cham cong — du lieu tren PSRAM; chi can Internal toi thieu cho httpd/TCP.
 * true = da gui JSON loi, caller return ngay.
 */
bool portal_reject_heavy_if_busy(httpd_req_t *req);

/**
 * Chan /api/log — buffer log tren PSRAM; chi can Internal toi thieu cho httpd chunk.
 */
bool portal_reject_log_if_busy(httpd_req_t *req);
