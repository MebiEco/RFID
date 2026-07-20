#pragma once

#include <stddef.h>

#define UI_LOG_ROWS_PER_PAGE 4

typedef struct {
    char date[12];
    char time[10];
    char name[24];
    char id[20];
} ui_log_row_t;

void ui_log_model_invalidate(void);

/** Đọc tối đa max_rows dòng cho trang page (0-based). Trả về số dòng đọc được. */
int ui_log_model_load_page(int page, ui_log_row_t *rows_out, int max_rows);
