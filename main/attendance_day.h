/**
 * Tổng quan chấm công:
 * - Hôm nay: Đi làm + Đi về (cùng 1 ngày, đủ chi tiết)
 * - Hôm qua: tổng quát không đi / đi muộn / về sớm
 * Giờ công cố định 08:30 – 18:00 (giờ địa phương board).
 * Lần quẹt đầu = vào. Hai lần cách nhau < 20 phút không tính là ra.
 * Về sớm: có lần ra thật và giờ ra < 18:00.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

/** Gửi JSON attendance overview (chunked). Caller đã auth. */
esp_err_t attendance_day_send_overview_json(httpd_req_t *req);
