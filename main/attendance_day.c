#include "attendance_day.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "board_pins.h"
#include "card_profile.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "portal_web.h"
#include "app_rfid.h"
#include "app_azure.h"
#include "scan_log.h"
#include "sd_card.h"
#include "wifi_portal.h"

static const char *TAG = "attend_day";

#define ATT_WORK_START_MIN (8 * 60 + 30)  /* 08:30 — muộn nếu LẦN ĐẦU vào sau mốc này */
#define ATT_WORK_END_MIN   (18 * 60)      /* 18:00 — về đúng giờ nếu LẦN RA >= mốc này */
/** Sau giờ này mới kết luận quên quẹt (đã quẹt vào nhưng chưa có lần ra thật). */
#define ATT_FORGOT_AFTER_MIN (18 * 60 + 30) /* 18:30 */
/** Hai lần quẹt cách nhau dưới ngưỡng này coi như cùng lần / nhầm — chưa tính là ra. */
#define ATT_CHECKOUT_GAP_MIN 20
#define ATT_MAX_EMP        128
#define ATT_LIST_CAP       64

#define ATT_LINE_SZ 384
EXT_RAM_BSS_ATTR static char s_att_line[ATT_LINE_SZ];
EXT_RAM_BSS_ATTR static char s_att_json_item[256];
EXT_RAM_BSS_ATTR static char s_att_json_hdr[400];

typedef struct {
    char uid[20];
    char name[48];
    char id[48];
    int y_swipes;
    int y_first_min;
    int y_last_min;
    int t_swipes;
    int t_first_min;
    int t_last_min;
} att_emp_t;

/**
 * Có lần ra thật: last sau first ít nhất ATT_CHECKOUT_GAP_MIN phút.
 * Lần đầu = vào; quẹt liên tiếp gần nhau (~20 phút) không tính về sớm.
 */
static bool att_has_checkout(const att_emp_t *e, int today)
{
    if (!e) {
        return false;
    }
    int first;
    int last;
    int swipes;
    if (today) {
        first = e->t_first_min;
        last = e->t_last_min;
        swipes = e->t_swipes;
    } else {
        first = e->y_first_min;
        last = e->y_last_min;
        swipes = e->y_swipes;
    }
    if (swipes < 2 || first < 0 || last < 0 || last <= first) {
        return false;
    }
    return (last - first) >= ATT_CHECKOUT_GAP_MIN;
}

/** Chỉ quẹt từ giờ về trở đi (≥end_min) — quên quẹt vào, không phải đi muộn. */
static bool att_only_evening_swipe(const att_emp_t *e, int today, int end_min)
{
    if (!e) {
        return false;
    }
    int first = today ? e->t_first_min : e->y_first_min;
    int swipes = today ? e->t_swipes : e->y_swipes;
    return swipes >= 1 && first >= end_min;
}

/**
 * Quên quẹt: trong ngày đã quẹt (≥1) nhưng chưa có lần ra đủ khoảng cách.
 * Hôm nay chỉ lọc vào Đi về sau forgot_after_min; hôm qua (ngày đã đóng) luôn áp dụng.
 * now_min: phút trong ngày hiện tại; với hôm qua truyền -1.
 */
static bool att_is_forgot(const att_emp_t *e, int today, int now_min, int forgot_after_min)
{
    if (!e) {
        return false;
    }
    int swipes = today ? e->t_swipes : e->y_swipes;
    if (swipes < 1 || att_has_checkout(e, today)) {
        return false;
    }
    if (today && now_min >= 0 && now_min < forgot_after_min) {
        return false;
    }
    return true;
}

static void att_feed_wdt(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

static void att_hm_from_ts(const char *ts, int *out_min, char *hm, size_t hmsz)
{
    *out_min = -1;
    if (hm && hmsz) {
        hm[0] = '\0';
    }
    char wall[40];
    scan_log_ts_field_to_wall_iso(ts, wall, sizeof(wall));
    const char *use = (wall[0] != '\0') ? wall : ts;
    /* YYYY-MM-DDThh:mm:ss (gio VN) */
    if (!use || strlen(use) < 16 || use[10] != 'T') {
        return;
    }
    int hh = (use[11] - '0') * 10 + (use[12] - '0');
    int mm = (use[14] - '0') * 10 + (use[15] - '0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return;
    }
    *out_min = hh * 60 + mm;
    if (hm && hmsz >= 6) {
        snprintf(hm, hmsz, "%02d:%02d", hh, mm);
    }
}

static void att_min_to_hm(int mins, char *hm, size_t hmsz)
{
    if (!hm || hmsz < 6 || mins < 0) {
        if (hm && hmsz) {
            hm[0] = '\0';
        }
        return;
    }
    snprintf(hm, hmsz, "%02d:%02d", mins / 60, mins % 60);
}

static void json_esc(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    if (!in) {
        in = "";
    }
    for (size_t i = 0; in[i] && j + 2 < outsz; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
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

static int att_find_uid(att_emp_t *emps, int n, const char *uid)
{
    if (!uid || !uid[0]) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (strcasecmp(emps[i].uid, uid) == 0) {
            return i;
        }
    }
    return -1;
}

static void att_apply_swipe(att_emp_t *e, int day_is_today, int mins)
{
    if (mins < 0) {
        return;
    }
    if (day_is_today) {
        if (e->t_swipes == 0) {
            e->t_first_min = mins;
            e->t_last_min = mins;
        } else {
            if (mins < e->t_first_min) {
                e->t_first_min = mins;
            }
            if (mins > e->t_last_min) {
                e->t_last_min = mins;
            }
        }
        e->t_swipes++;
    } else {
        if (e->y_swipes == 0) {
            e->y_first_min = mins;
            e->y_last_min = mins;
        } else {
            if (mins < e->y_first_min) {
                e->y_first_min = mins;
            }
            if (mins > e->y_last_min) {
                e->y_last_min = mins;
            }
        }
        e->y_swipes++;
    }
}

static esp_err_t send_chunk(httpd_req_t *req, const char *s)
{
    return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_list_item(httpd_req_t *req, bool *first, const char *name, const char *id,
                                const char *time_key, const char *time_val)
{
    char en[96], ei[96];
    json_esc(name ? name : "", en, sizeof(en));
    json_esc(id ? id : "", ei, sizeof(ei));
    if (time_key && time_val && time_val[0]) {
        snprintf(s_att_json_item, sizeof(s_att_json_item), "%s{\"name\":\"%s\",\"id\":\"%s\",\"%s\":\"%s\"}",
                 *first ? "" : ",", en, ei, time_key, time_val);
    } else {
        snprintf(s_att_json_item, sizeof(s_att_json_item), "%s{\"name\":\"%s\",\"id\":\"%s\"}", *first ? "" : ",",
                 en, ei);
    }
    *first = false;
    return send_chunk(req, s_att_json_item);
}

static void att_format_ymd(const struct tm *t, char *out, size_t outsz)
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

/** YYYY-MM-DD hợp lệ (chỉ kiểm tra format + khoảng tháng/ngày thô). */
static bool att_ymd_ok(const char *s)
{
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') {
        return false;
    }
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int m = (s[5] - '0') * 10 + (s[6] - '0');
    int d = (s[8] - '0') * 10 + (s[9] - '0');
    return y >= 2000 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

/**
 * Doc cua so tu EOF (append-only). Xu ly dong moi→cu; gap ngay < hist_ymd thi dung.
 * Mo rong toi ATT_TAIL_WIN_MAX; van chua gap ngay cu → fallback quet xuoi (nad).
 */
#define ATT_TAIL_WIN_START (256 * 1024)
#define ATT_TAIL_WIN_MAX   (1024 * 1024)

static void att_apply_line(att_emp_t *emps, int nemp, char *line, const char *today_ymd, const char *hist_ymd,
                           int *day_out, bool *older_out)
{
    if (day_out) {
        *day_out = -1;
    }
    if (older_out) {
        *older_out = false;
    }
    char *p = line;
    char *ts = strsep(&p, "|");
    char *uid = strsep(&p, "|");
    (void)strsep(&p, "|");
    (void)strsep(&p, "|");
    char *reg_s = strsep(&p, "|");
    if (!ts || !uid || ts[0] == '\0') {
        return;
    }
    if (reg_s && strcmp(reg_s, "99") == 0) {
        return;
    }
    int reg = reg_s ? atoi(reg_s) : -1;
    if (reg != 1) {
        return;
    }
    char row_ymd[12];
    scan_log_ts_field_ymd_local(ts, row_ymd, sizeof(row_ymd));
    if (row_ymd[0] == '\0') {
        return;
    }
    if (strcmp(row_ymd, hist_ymd) < 0) {
        if (older_out) {
            *older_out = true;
        }
        return;
    }
    int day = -1;
    if (strcmp(row_ymd, today_ymd) == 0) {
        day = 1;
    } else if (strcmp(row_ymd, hist_ymd) == 0) {
        day = 0;
    } else {
        return; /* ngay giua / lech — bo */
    }
    if (day_out) {
        *day_out = day;
    }
    int idx = att_find_uid(emps, nemp, uid);
    if (idx < 0) {
        return;
    }
    int mins = -1;
    att_hm_from_ts(ts, &mins, NULL, 0);
    att_apply_swipe(&emps[idx], day, mins);
}

/**
 * Doc tu EOF… Tra ve: true=ok, false=loi/abort (xem *aborted).
 */
static bool att_scan_log_for_overview(FILE *fp, att_emp_t *emps, int nemp, const char *today_ymd,
                                      const char *hist_ymd, bool *aborted)
{
    if (aborted) {
        *aborted = false;
    }
    if (!fp || !emps || nemp <= 0) {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        return false;
    }
    long fsz = ftell(fp);
    if (fsz <= 0) {
        return true;
    }

    size_t win = (size_t)ATT_TAIL_WIN_START;
    if ((long)win > fsz) {
        win = (size_t)fsz;
    }
    bool saw_older = false;
    bool covered_all = false;

    while (1) {
        att_feed_wdt();
        if (app_rfid_swipe_busy() || sd_card_service_waiting() || app_azure_tx_busy()) {
            ESP_LOGW(TAG, "overview abort — uu tien quet/Azure");
            if (aborted) {
                *aborted = true;
            }
            return false;
        }
        long start = fsz - (long)win;
        if (start < 0) {
            start = 0;
        }
        covered_all = (start == 0);
        size_t n = (size_t)(fsz - start);
        char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            return false;
        }
        if (fseek(fp, start, SEEK_SET) != 0 || fread(buf, 1, n, fp) != n) {
            free(buf);
            return false;
        }
        buf[n] = '\0';

        char *body = buf;
        if (start > 0) {
            char *nl = strchr(buf, '\n');
            if (!nl) {
                free(buf);
                if (covered_all) {
                    break;
                }
                goto expand;
            }
            body = nl + 1;
        }

        /* Thu thap con tro dong; duyet moi → cu. */
        int max_lines = (int)(n / 40) + 8;
        if (max_lines < 32) {
            max_lines = 32;
        }
        char **lines = (char **)heap_caps_malloc((size_t)max_lines * sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!lines) {
            free(buf);
            return false;
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

        saw_older = false;
        for (int i = nlines - 1; i >= 0; i--) {
            bool older = false;
            att_apply_line(emps, nemp, lines[i], today_ymd, hist_ymd, NULL, &older);
            if (older) {
                saw_older = true;
                break;
            }
        }
        free(lines);
        free(buf);

        if (saw_older || covered_all) {
            ESP_LOGI(TAG, "overview tail-win=%u saw_older=%d covered=%d", (unsigned)win, (int)saw_older,
                     (int)covered_all);
            return true;
        }
expand:
        if (win >= (size_t)ATT_TAIL_WIN_MAX || (long)win >= fsz) {
            break;
        }
        win *= 2;
        if (win > (size_t)ATT_TAIL_WIN_MAX) {
            win = (size_t)ATT_TAIL_WIN_MAX;
        }
        if ((long)win > fsz) {
            win = (size_t)fsz;
        }
        /* Reset swipe stats truoc khi quet lai cua so lon hon. */
        for (int i = 0; i < nemp; i++) {
            emps[i].y_swipes = 0;
            emps[i].y_first_min = -1;
            emps[i].y_last_min = -1;
            emps[i].t_swipes = 0;
            emps[i].t_first_min = -1;
            emps[i].t_last_min = -1;
        }
    }

    /* Fallback: quet xuoi, bo ngay cu (van feed WDT). */
    ESP_LOGW(TAG, "overview tail miss — fallback full forward");
    for (int i = 0; i < nemp; i++) {
        emps[i].y_swipes = 0;
        emps[i].y_first_min = -1;
        emps[i].y_last_min = -1;
        emps[i].t_swipes = 0;
        emps[i].t_first_min = -1;
        emps[i].t_last_min = -1;
    }
    rewind(fp);
    int line_n = 0;
    while (fgets(s_att_line, ATT_LINE_SZ, fp)) {
        if ((++line_n & 31) == 0) {
            att_feed_wdt();
        }
        att_apply_line(emps, nemp, s_att_line, today_ymd, hist_ymd, NULL, NULL);
    }
    return true;
}

esp_err_t attendance_day_send_overview_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (portal_reject_heavy_if_busy(req)) {
        return ESP_OK;
    }

    if (!wifi_portal_time_is_valid()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Chua dong bo gio NTP\"}");
    }
    if (!sd_card_is_mounted()) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SD chua mount\"}");
    }

    time_t now = time(NULL);
    struct tm today_tm;
    scan_log_wall_tm(now, &today_tm);
    char today_ymd[16];
    att_format_ymd(&today_tm, today_ymd, sizeof(today_ymd));
    const int now_min = today_tm.tm_hour * 60 + today_tm.tm_min;

    time_t yest = now - 24 * 3600;
    struct tm yest_tm;
    scan_log_wall_tm(yest, &yest_tm);
    char yest_ymd[16];
    att_format_ymd(&yest_tm, yest_ymd, sizeof(yest_ymd));

    char hist_ymd[16];
    snprintf(hist_ymd, sizeof(hist_ymd), "%s", yest_ymd);
    char qry[96];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
        char date_q[16];
        if (httpd_query_key_value(qry, "date", date_q, sizeof(date_q)) == ESP_OK && date_q[0]) {
            if (!att_ymd_ok(date_q)) {
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Ngay khong hop le (YYYY-MM-DD)\"}");
            }
            if (strcmp(date_q, today_ymd) >= 0) {
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Chi xem ngay truoc hom nay\"}");
            }
            snprintf(hist_ymd, sizeof(hist_ymd), "%s", date_q);
        }
    }

    att_emp_t *emps = (att_emp_t *)heap_caps_calloc(ATT_MAX_EMP, sizeof(att_emp_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!emps) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Het PSRAM (tong quan)\"}");
    }

    CardProfileEntry_t *profiles =
        (CardProfileEntry_t *)heap_caps_malloc(ATT_MAX_EMP * sizeof(CardProfileEntry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!profiles) {
        free(emps);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Het PSRAM (profiles)\"}");
    }

    int nprof = card_profile_list_page(profiles, ATT_MAX_EMP, false, 0, NULL);
    int nemp = 0;
    for (int i = 0; i < nprof && nemp < ATT_MAX_EMP; i++) {
        if (!profiles[i].registered) {
            continue;
        }
        snprintf(emps[nemp].uid, sizeof(emps[nemp].uid), "%s", profiles[i].uid);
        snprintf(emps[nemp].name, sizeof(emps[nemp].name), "%s",
                 profiles[i].name[0] ? profiles[i].name : profiles[i].uid);
        snprintf(emps[nemp].id, sizeof(emps[nemp].id), "%s", profiles[i].id);
        emps[nemp].y_swipes = 0;
        emps[nemp].y_first_min = -1;
        emps[nemp].y_last_min = -1;
        emps[nemp].t_swipes = 0;
        emps[nemp].t_first_min = -1;
        emps[nemp].t_last_min = -1;
        nemp++;
    }
    free(profiles);

    sd_card_lock();
    FILE *fp = fopen(BOARD_SD_RFID_LOG_PATH, "r");
    bool aborted = false;
    if (fp) {
        (void)att_scan_log_for_overview(fp, emps, nemp, today_ymd, hist_ymd, &aborted);
        fclose(fp);
    }
    sd_card_unlock();
    if (aborted) {
        free(emps);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Dang quet the / gui Azure — thu lai sau\"}");
    }
    int t_abs = 0, t_late = 0, t_ontime = 0;
    int t_leave_abs = 0, t_forgot = 0, t_early = 0, t_out_ok = 0;
    int y_abs = 0, y_late = 0, y_early = 0, y_forgot = 0;

    int att_work_start_min = 8 * 60 + 30;
    int att_work_end_min = 18 * 60;
    wifi_portal_get_work_hours_min(&att_work_start_min, &att_work_end_min);
    int att_forgot_after_min = att_work_end_min + 30;
    char work_start_str[8] = {0}, work_end_str[8] = {0};
    wifi_portal_get_work_hours(work_start_str, sizeof(work_start_str), work_end_str, sizeof(work_end_str));
    char work_hours_buf[32];
    snprintf(work_hours_buf, sizeof(work_hours_buf), "%s-%s", work_start_str, work_end_str);

    for (int i = 0; i < nemp; i++) {
        att_emp_t *e = &emps[i];
        if (e->t_swipes == 0) {
            t_abs++;
        } else if (att_only_evening_swipe(e, 1, att_work_end_min)) {
        } else if (e->t_first_min > att_work_start_min) {
            t_late++;
        } else {
            t_ontime++;
        }
        if (e->t_swipes == 0) {
            t_leave_abs++;
        } else if (att_is_forgot(e, 1, now_min, att_forgot_after_min)) {
            t_forgot++;
        } else if (att_has_checkout(e, 1) && e->t_last_min < att_work_end_min) {
            t_early++;
        } else if (att_has_checkout(e, 1)) {
            t_out_ok++;
        }
        if (e->y_swipes == 0) {
            y_abs++;
        } else if (att_is_forgot(e, 0, -1, att_forgot_after_min)) {
            y_forgot++;
        }
        if (e->y_swipes >= 1 && !att_only_evening_swipe(e, 0, att_work_end_min) && e->y_first_min > att_work_start_min) {
            y_late++;
        }
        if (att_has_checkout(e, 0) && e->y_last_min < att_work_end_min) {
            y_early++;
        }
    }

    ESP_LOGI(TAG, "overview chunk free_int=%u dma=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    snprintf(s_att_json_hdr, sizeof(s_att_json_hdr),
             "{\"ok\":true,\"work_hours\":\"%s\",\"work_start\":\"%s\",\"work_end\":\"%s\",\"registered\":%d,"
             "\"today\":{\"date\":\"%s\","
             "\"arrive\":{\"absent\":%d,\"late\":%d,\"ontime\":%d,\"absent_list\":[",
             work_hours_buf, work_start_str, work_end_str, nemp, today_ymd, t_abs, t_late, t_ontime);

    esp_err_t err = send_chunk(req, s_att_json_hdr);
    if (err != ESP_OK) {
        free(emps);
        return err;
    }

    bool first = true;
    int listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        if (emps[i].t_swipes != 0) {
            continue;
        }
        err = send_list_item(req, &first, emps[i].name, emps[i].id, NULL, NULL);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"late_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (e->t_swipes == 0 || att_only_evening_swipe(e, 1, att_work_end_min) || e->t_first_min <= att_work_start_min) {
            continue;
        }
        char hm[8];
        att_min_to_hm(e->t_first_min, hm, sizeof(hm));
        err = send_list_item(req, &first, e->name, e->id, "in", hm);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"ontime_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (e->t_swipes == 0 || att_only_evening_swipe(e, 1, att_work_end_min) || e->t_first_min > att_work_start_min) {
            continue;
        }
        char hm[8];
        att_min_to_hm(e->t_first_min, hm, sizeof(hm));
        err = send_list_item(req, &first, e->name, e->id, "in", hm);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    char leave_hdr[200];
    snprintf(leave_hdr, sizeof(leave_hdr),
             "]},\"leave\":{\"absent\":%d,\"forgot\":%d,\"early\":%d,\"on_time_out\":%d,\"absent_list\":[", t_leave_abs,
             t_forgot, t_early, t_out_ok);
    err = send_chunk(req, leave_hdr);
    if (err != ESP_OK) {
        free(emps);
        return err;
    }

    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        if (emps[i].t_swipes != 0) {
            continue;
        }
        err = send_list_item(req, &first, emps[i].name, emps[i].id, NULL, NULL);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"forgot_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (!att_is_forgot(e, 1, now_min, att_forgot_after_min)) {
            continue;
        }
        char hm[8];
        if (att_only_evening_swipe(e, 1, att_work_end_min)) {
            att_min_to_hm(e->t_last_min >= 0 ? e->t_last_min : e->t_first_min, hm, sizeof(hm));
            err = send_list_item(req, &first, e->name, e->id, "out", hm);
        } else {
            att_min_to_hm(e->t_first_min, hm, sizeof(hm));
            err = send_list_item(req, &first, e->name, e->id, "in", hm);
        }
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"early_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (!att_has_checkout(e, 1) || e->t_last_min >= att_work_end_min) {
            continue;
        }
        char hm[8];
        att_min_to_hm(e->t_last_min, hm, sizeof(hm));
        err = send_list_item(req, &first, e->name, e->id, "out", hm);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"on_time_out_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (!att_has_checkout(e, 1) || e->t_last_min < att_work_end_min) {
            continue;
        }
        char hm[8];
        att_min_to_hm(e->t_last_min, hm, sizeof(hm));
        err = send_list_item(req, &first, e->name, e->id, "out", hm);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    char yhdr[320];
    snprintf(yhdr, sizeof(yhdr),
             "]}},\"hist_default\":\"%s\",\"yesterday\":{\"date\":\"%s\",\"absent\":%d,\"late\":%d,\"early\":%d,\"forgot\":%d,\"absent_list\":[",
             yest_ymd, hist_ymd, y_abs, y_late, y_early, y_forgot);
    err = send_chunk(req, yhdr);
    if (err != ESP_OK) {
        free(emps);
        return err;
    }

    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        if (emps[i].y_swipes != 0) {
            continue;
        }
        err = send_list_item(req, &first, emps[i].name, emps[i].id, NULL, NULL);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"late_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (e->y_swipes < 1 || att_only_evening_swipe(e, 0, att_work_end_min) || e->y_first_min <= att_work_start_min) {
            continue;
        }
        char hm[8];
        att_min_to_hm(e->y_first_min, hm, sizeof(hm));
        err = send_list_item(req, &first, e->name, e->id, "in", hm);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"forgot_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (!att_is_forgot(e, 0, -1, att_forgot_after_min)) {
            continue;
        }
        char hm[8];
        if (att_only_evening_swipe(e, 0, att_work_end_min)) {
            att_min_to_hm(e->y_last_min >= 0 ? e->y_last_min : e->y_first_min, hm, sizeof(hm));
            err = send_list_item(req, &first, e->name, e->id, "out", hm);
        } else {
            att_min_to_hm(e->y_first_min, hm, sizeof(hm));
            err = send_list_item(req, &first, e->name, e->id, "in", hm);
        }
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    err = send_chunk(req, "],\"early_list\":[");
    if (err != ESP_OK) {
        free(emps);
        return err;
    }
    first = true;
    listed = 0;
    for (int i = 0; i < nemp && listed < ATT_LIST_CAP; i++) {
        att_emp_t *e = &emps[i];
        if (!att_has_checkout(e, 0) || e->y_last_min >= att_work_end_min) {
            continue;
        }
        char hm[8];
        att_min_to_hm(e->y_last_min, hm, sizeof(hm));
        err = send_list_item(req, &first, e->name, e->id, "out", hm);
        if (err != ESP_OK) {
            free(emps);
            return err;
        }
        listed++;
    }

    free(emps);
    err = send_chunk(req, "]}}");
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG,
             "overview chunked | today %s in abs=%d late=%d on=%d | out abs=%d forgot=%d early=%d ok=%d | hist %s abs=%d late=%d forgot=%d early=%d (n=%d)",
             today_ymd, t_abs, t_late, t_ontime, t_leave_abs, t_forgot, t_early, t_out_ok, hist_ymd, y_abs, y_late,
             y_forgot, y_early, nemp);
    return httpd_resp_send_chunk(req, NULL, 0);
}
