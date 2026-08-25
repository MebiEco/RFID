#pragma once

#include "esp_err.h"

/** Cần SD đã mount (sd_card_mount). Đọc file JPEG, giải mã, vẽ vào khung tại (x,y) kích thước w×h. */
esp_err_t sd_png_show_image_at(const char *path, int tgt_x, int tgt_y, int tgt_w, int tgt_h);
