#include "ui_log_model.h"

#include "board_pins.h"
#include "sd_card.h"
#include "lv_port.h"

#include <stdio.h>
#include <string.h>

static ui_log_row_t s_rows[UI_LOG_ROWS_PER_PAGE];
static int          s_count        = 0;
static int          s_cached_page = -99;

static void log_parse_ts_field(const char *f0, ui_log_row_t *r)
{
    int Y, M, D, h, mi, s2;
    if (f0 && sscanf(f0, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &s2) == 6) {
        snprintf(r->date, sizeof(r->date), "%02d/%02d/%04d", D, M, Y);
        snprintf(r->time, sizeof(r->time), "%02d:%02d:%02d", h, mi, s2);
    } else {
        strncpy(r->date, "--", sizeof(r->date) - 1);
        r->date[sizeof(r->date) - 1] = '\0';
        strncpy(r->time, (f0 && f0[0]) ? f0 : "?", sizeof(r->time) - 1);
        r->time[sizeof(r->time) - 1] = '\0';
    }
}

void ui_log_model_invalidate(void)
{
    s_cached_page = -99;
}

int ui_log_model_load_page(int page, ui_log_row_t *rows_out, int max_rows)
{
    if (!rows_out || max_rows <= 0) {
        return 0;
    }

    if (page == s_cached_page && max_rows >= UI_LOG_ROWS_PER_PAGE) {
        int n = s_count < max_rows ? s_count : max_rows;
        for (int i = 0; i < n; i++) {
            rows_out[i] = s_rows[i];
        }
        return n;
    }

    s_count = 0;
    if (!sd_card_is_mounted()) {
        s_cached_page = page;
        return 0;
    }

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    if (!fp) {
        sd_card_unlock();
        s_cached_page = page;
        return 0;
    }

    // Đếm tổng số dòng
    int total_lines = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        lv_port_feed_wdt();
        total_lines++;
    }

    int skip_from_end = page * UI_LOG_ROWS_PER_PAGE;
    int target_line_end = total_lines - skip_from_end;
    int target_line_start = target_line_end - UI_LOG_ROWS_PER_PAGE + 1;
    if (target_line_start < 1) target_line_start = 1;

    if (target_line_end < 1) {
        fclose(fp);
        sd_card_unlock();
        s_cached_page = page;
        return 0;
    }

    // Quay lại đầu file để đọc các dòng mục tiêu
    fseek(fp, 0, SEEK_SET);
    int cur = 0;
    s_count = 0;
    
    // Lưu các dòng vào mảng tạm để đảo ngược lại sau
    ui_log_row_t temp_rows[UI_LOG_ROWS_PER_PAGE];
    int temp_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        lv_port_feed_wdt();
        cur++;
        if (cur < target_line_start) continue;
        if (cur > target_line_end) break;

        char buf[256];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *nl = strchr(buf, '\r'); if (nl) *nl = '\0';
        nl = strchr(buf, '\n'); if (nl) *nl = '\0';

        // Format nhat ky: Date | UID | Name | ID | Type
        char *f0 = strtok(buf, "|"); // Time
        strtok(NULL, "|");           // UID (bo qua)
        char *f2 = strtok(NULL, "|"); // Name
        char *f3 = strtok(NULL, "|"); // ID
        
        ui_log_row_t *r = &temp_rows[temp_count];
        log_parse_ts_field(f0 ? f0 : "", r);
        strncpy(r->name, f2 ? f2 : "The La", sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
        strncpy(r->id, f3 ? f3 : "---", sizeof(r->id) - 1);
        r->id[sizeof(r->id) - 1] = '\0';
        
        temp_count++;
    }
    fclose(fp);
    sd_card_unlock();

    // Đảo ngược mảng temp_rows vào rows_out để dòng mới nhất (dòng cuối file) lên đầu
    for (int i = 0; i < temp_count; i++) {
        rows_out[i] = temp_rows[temp_count - 1 - i];
        s_rows[i] = rows_out[i]; // Cập nhật cache
    }
    s_count = temp_count;

    s_cached_page = page;
    return s_count;
}
