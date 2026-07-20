#include "lv_port.h"
#include "ui_layout.h"
#include "lvgl.h"
#include "ui_layout.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "app_rfid.h"
#include "board_pins.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "lcd_ui.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "wifi_portal.h"
#include "nvs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "lv_port";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LV_FONT_DECLARE(lv_font_montserrat_14);

idle_data_t g_idle_data = {0};

static lv_obj_t * idle_scr = NULL;
static lv_obj_t * idle_lbl_title = NULL;
static lv_obj_t * lbl_time = NULL;
static lv_obj_t * idle_lbl_date_line = NULL;
static lv_obj_t * idle_lbl_brand = NULL;   /* chữ thương hiệu — cập nhật qua lv_port_set_brand_text() */
static lv_obj_t * idle_wifi_sig = NULL;
static lv_obj_t * idle_azure_holder = NULL;
static char s_brand_text[16] = "";         /* buffer: đọc từ NVS lúc init, hoặc set từ web */
#if BOARD_ENABLE_AZURE
static lv_obj_t * azure_lbl_off = NULL;
static lv_obj_t * azure_lbl_up = NULL;
static lv_obj_t * azure_lbl_dn = NULL;
#endif
static lv_obj_t * clock_face_cont = NULL;
static lv_obj_t * line_clk_h = NULL;
static lv_obj_t * line_clk_m = NULL;
static lv_obj_t * line_clk_s = NULL;
static lv_point_t clk_pt_h[2];
static lv_point_t clk_pt_m[2];
static lv_point_t clk_pt_s[2];

/** Màn chờ — nền sáng #F1F5F9 (slate-100), khớp theme LVGL. */
#define IDLE_SCR_BG          0xF1F5F9
#define IDLE_SCR_FG          0x000000
#define IDLE_SCR_ACCENT      0x3F51B5
#define IDLE_SCR_MUTED       0x64748B
#define IDLE_STATUS_OK       0x4CAF50
#define IDLE_SCR_CARD_BORDER 0xE2E8F0
#define IDLE_SCR_PAD_X       UI_SCR_PAD_X
#define IDLE_SCR_INNER_W     (BOARD_LCD_H_RES - 2 * (IDLE_SCR_PAD_X))
/** Trái: tiêu đề + thương hiệu + ngày giờ số. Phải: chỉ đồng hồ kim. */
#if BOARD_LCD_V_RES <= 240
#define IDLE_LEFT_COL_W      152
#define IDLE_RIGHT_COL_W     (BOARD_LCD_H_RES - 2 * IDLE_SCR_PAD_X - IDLE_LEFT_COL_W - IDLE_BLOCK_GAP)
#define IDLE_BLOCK_GAP       6
#define IDLE_TITLE_LINE_H    24
#define IDLE_HEADER_BRAND_H  26
#define IDLE_RIGHT_STATUS_H  28
#define IDLE_WIFI_CW         28
#define IDLE_WIFI_CH         22
#define IDLE_CLOCK_FACE_W    (IDLE_RIGHT_COL_W - 6)
#define IDLE_MAIN_ROW_H      (BOARD_LCD_V_RES - 2 * IDLE_BLOCK_GAP)
#else
#define IDLE_BLOCK_GAP       8
#define IDLE_LEFT_COL_W      ((BOARD_LCD_H_RES * 44) / 100)
#define IDLE_RIGHT_COL_W     (BOARD_LCD_H_RES - IDLE_LEFT_COL_W - 8)
#define IDLE_TITLE_LINE_H    32
#define IDLE_HEADER_BAR_H    32
#define IDLE_WIFI_CW         36
#define IDLE_WIFI_CH         24
#define IDLE_CLOCK_MIN_FACE  96
#define IDLE_MAIN_ROW_H      (BOARD_LCD_V_RES - 2 * IDLE_BLOCK_GAP)
#endif
#ifndef IDLE_RIGHT_STATUS_H
#define IDLE_RIGHT_STATUS_H  28
#endif
#define IDLE_AZURE_DN_BLUE   0x2196F3

/* Khai báo các font chữ có dấu tiếng Việt đã được tạo */
LV_FONT_DECLARE(arial_vn_24);
LV_FONT_DECLARE(arial_vn_32);
// arial_vn_48 đã xóa khỏi build (1.1MB, không dùng - dùng arial_vn_32 thay thế)

bool g_lcd_showing_swipe_ui = false; // Define global swipe state here

// --- Thread-safe swipe result buffer ---
// rfid_task chỉ ghi vào đây, lvgl_task đọc và tạo popup
typedef struct {
    volatile bool pending;   // rfid_task set = true khi có kết quả mới
    char display_name[64];
    char emp_id_line[56];
    char time_str[32];
    bool success;
    int check_type;          // 1=Xin chào, 2=Tạm biệt
} swipe_pending_t;

static swipe_pending_t s_swipe_pending = {0};
static void show_swipe_popup_from_lvgl(void);


static void idle_clock_set_hand(lv_point_t *p, int cx, int cy, int len, double ang_rad)
{
    p[0].x = cx;
    p[0].y = cy;
    p[1].x = cx + (int)(len * sin(ang_rad) + 0.5);
    p[1].y = cy - (int)(len * cos(ang_rad) + 0.5);
}

static void idle_clock_hands_update(void)
{
    if (!line_clk_h || !clock_face_cont) {
        return;
    }
    lv_obj_update_layout(clock_face_cont);
    const lv_coord_t fw = lv_obj_get_content_width(clock_face_cont);
    const lv_coord_t fh = lv_obj_get_content_height(clock_face_cont);
    const int cx = (int)fw / 2;
    const int cy = (int)fh / 2;
#if BOARD_LCD_V_RES <= 240
    int r_unit = (cx - 12 < (int)fh / 2 - 12 ? cx - 12 : (int)fh / 2 - 12);
    if (r_unit < 8) {
        r_unit = 8;
    }
#else
    const int r_unit = (cx < (int)fh / 2 ? cx : (int)fh / 2);
#endif
    int h = g_idle_data.h % 24;
    int mi = g_idle_data.m;
    int se = g_idle_data.s;
    double hr = ((h % 12) + mi / 60.0 + se / 3600.0) / 12.0 * (2.0 * M_PI);
    double mr = (mi + se / 60.0) / 60.0 * (2.0 * M_PI);
    double sr = se / 60.0 * (2.0 * M_PI);
#if BOARD_LCD_V_RES <= 240
    const int rh = LV_MAX(7, r_unit * 30 / 58);
    const int rm = LV_MAX(9, r_unit * 42 / 58);
    const int rs = LV_MAX(10, r_unit * 48 / 58);
#else
    const int rh = LV_MAX(10, r_unit * 36 / 70);
    const int rm = LV_MAX(12, r_unit * 50 / 70);
    const int rs = LV_MAX(14, r_unit * 56 / 70);
#endif
    idle_clock_set_hand(clk_pt_h, cx, cy, rh, hr);
    idle_clock_set_hand(clk_pt_m, cx, cy, rm, mr);
    idle_clock_set_hand(clk_pt_s, cx, cy, rs, sr);
    lv_line_set_points(line_clk_h, clk_pt_h, 2);
    lv_line_set_points(line_clk_m, clk_pt_m, 2);
    lv_line_set_points(line_clk_s, clk_pt_s, 2);
    lv_obj_set_pos(line_clk_h, 0, 0);
    lv_obj_set_pos(line_clk_m, 0, 0);
    lv_obj_set_pos(line_clk_s, 0, 0);
    lv_obj_set_size(line_clk_h, fw, fh);
    lv_obj_set_size(line_clk_m, fw, fh);
    lv_obj_set_size(line_clk_s, fw, fh);
}

static void idle_clock_build_face(lv_obj_t *face)
{
    lv_obj_update_layout(face);
    const int fw = (int)lv_obj_get_content_width(face);
    const int fh = (int)lv_obj_get_content_height(face);
    const int cx = fw / 2;
    const int cy = fh / 2;

#if BOARD_LCD_V_RES <= 240
    /* Lùi số vào trong viền — montserrat_14 ~7px nửa chiều rộng ký tự. */
    const int inset = 10;
    double Rx = (double)(fw / 2 - inset);
    double Ry = (double)(fh / 2 - inset);
    if (Rx < 12.0) {
        Rx = 12.0;
    }
    if (Ry < 12.0) {
        Ry = 12.0;
    }
    for (int h = 1; h <= 12; h++) {
        double th = (h % 12) * 30.0 * (M_PI / 180.0);
        lv_obj_t *nb = lv_label_create(face);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", h);
        lv_label_set_text(nb, buf);
        lv_obj_set_style_text_font(nb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nb, lv_color_hex(IDLE_SCR_FG), 0);
        lv_coord_t ox = (lv_coord_t)(Rx * sin(th) + 0.5);
        lv_coord_t oy = (lv_coord_t)(-Ry * cos(th) + 0.5);
        lv_obj_align(nb, LV_ALIGN_CENTER, ox, oy);
    }
#else
    const int r_unit = (cx < cy ? cx : cy);
    const int r_out = r_unit - 4;
    for (int h = 0; h < 12; h++) {
        double th = h * 30.0 * (M_PI / 180.0);
        const bool is_main = (h % 3 == 0);
        const int r_in = is_main ? (r_unit - 18) : (r_unit - 12);
        static lv_point_t seg[12][2];
        seg[h][0].x = cx + (int)(r_in * sin(th) + 0.5);
        seg[h][0].y = cy - (int)(r_in * cos(th) + 0.5);
        seg[h][1].x = cx + (int)(r_out * sin(th) + 0.5);
        seg[h][1].y = cy - (int)(r_out * cos(th) + 0.5);
        lv_obj_t *tick = lv_line_create(face);
        lv_line_set_points(tick, seg[h], 2);
        lv_obj_set_pos(tick, 0, 0);
        lv_obj_set_size(tick, fw, fh);
        lv_obj_set_style_line_width(tick, is_main ? 4 : 2, 0);
        lv_obj_set_style_line_color(tick, lv_color_hex(is_main ? IDLE_SCR_FG : IDLE_SCR_ACCENT), 0);
        lv_obj_set_style_line_rounded(tick, true, 0);
    }
    const double R_num = (double)r_unit * 0.75;
    for (int h = 1; h <= 12; h++) {
        double th = (h % 12) * 30.0 * (M_PI / 180.0);
        lv_obj_t *nb = lv_label_create(face);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", h);
        lv_label_set_text(nb, buf);
        lv_obj_set_style_text_font(nb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(nb, lv_color_hex(IDLE_SCR_FG), 0);
        lv_coord_t ox = (lv_coord_t)(R_num * sin(th) + 0.5);
        lv_coord_t oy = (lv_coord_t)(-R_num * cos(th) + 0.5);
        lv_obj_align(nb, LV_ALIGN_CENTER, ox, oy);
    }
#endif
}

#if BOARD_ENABLE_WIFI
static lv_color_t s_idle_wifi_buf[IDLE_WIFI_CW * IDLE_WIFI_CH];

/* Quạt đối xứng quanh 270° (lên), góc 90 độ cho thanh thoát */
#define IDLE_WIFI_ARC_START 225
#define IDLE_WIFI_ARC_END   315

static void idle_wifi_draw_dot(lv_obj_t * canvas, lv_coord_t cx, lv_coord_t dot_top_y, lv_color_t col, lv_opa_t opa)
{
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col;
    rd.bg_opa = opa;
    rd.radius = LV_RADIUS_CIRCLE;
    const lv_coord_t dot_r = 3;
    lv_canvas_draw_rect(canvas, cx - dot_r, dot_top_y, 2 * dot_r, 2 * dot_r, &rd);
}

static void idle_wifi_redraw(lv_obj_t * canvas, int rssi)
{
    if (!canvas || !lv_obj_is_valid(canvas)) {
        return;
    }

    lv_canvas_fill_bg(canvas, lv_color_hex(IDLE_SCR_BG), LV_OPA_COVER);

    const lv_coord_t cx = IDLE_WIFI_CW / 2;
#if BOARD_LCD_V_RES <= 240
    const lv_coord_t pivot_y = IDLE_WIFI_CH - 5;
    static const lv_coord_t radii[] = { 3, 7, 11 };
    const lv_coord_t dot_r = 2;
    const lv_coord_t arc_w = 2;
#else
    const lv_coord_t pivot_y = IDLE_WIFI_CH - 4;
    static const lv_coord_t radii[] = { 6, 11, 16 };
    const lv_coord_t dot_r = 3;
    const lv_coord_t arc_w = 3;
#endif
    const lv_coord_t dot_top = pivot_y - dot_r;

    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.width = arc_w;
    arc_dsc.rounded = 1;

    const lv_color_t c_on = lv_color_hex(IDLE_SCR_FG);
    const lv_color_t c_off = lv_color_hex(0x94A3B8);
    const bool linked = (rssi > -120 && rssi < 0);

    /*
     * Số cung BẬT khi đã kết nối: luôn 1..3 (không dùng bars-1 → yếu chỉ còn chấm).
     * Ngưỡng dBm gần thực tế ESP.
     */
    int arcs_on = 0;
    if (linked) {
        if (rssi >= -57) {
            arcs_on = 3;
        } else if (rssi >= -68) {
            arcs_on = 2;
        } else {
            arcs_on = 1;
        }
    }

    if (!linked) {
        idle_wifi_draw_dot(canvas, cx, dot_top, c_off, LV_OPA_50);
        arc_dsc.color = c_off;
        arc_dsc.opa = LV_OPA_40;
        for (int i = 0; i < 3; i++) {
            lv_canvas_draw_arc(canvas, cx, pivot_y, radii[i], IDLE_WIFI_ARC_START, IDLE_WIFI_ARC_END, &arc_dsc);
        }
        return;
    }

    idle_wifi_draw_dot(canvas, cx, dot_top, c_on, LV_OPA_COVER);

    for (int i = 0; i < 3; i++) {
        const bool on = (i < arcs_on);
        arc_dsc.color = on ? c_on : c_off;
        arc_dsc.opa = on ? LV_OPA_COVER : LV_OPA_40;
        lv_canvas_draw_arc(canvas, cx, pivot_y, radii[i], IDLE_WIFI_ARC_START, IDLE_WIFI_ARC_END, &arc_dsc);
    }
}
#endif /* BOARD_ENABLE_WIFI */

static void update_ui_timer_cb(lv_timer_t * timer) {
    (void)timer;
    extern int g_ui_state;
    // Chỉ cập nhật khi đang ở idle screen và các label tồn tại
    if (!idle_scr || !lbl_time || !idle_lbl_date_line || g_ui_state != 0) {
        return;
    }

    char buf[72];
#if BOARD_ENABLE_WIFI
    if (idle_wifi_sig) {
        idle_wifi_redraw(idle_wifi_sig, g_idle_data.rssi);
    }
#else
    (void)idle_wifi_sig;
#endif

    idle_clock_hands_update();

#if BOARD_ENABLE_AZURE
    if (idle_azure_holder && azure_lbl_off && azure_lbl_up && azure_lbl_dn &&
        lv_obj_is_valid(idle_azure_holder)) {
        lv_obj_clear_flag(idle_azure_holder, LV_OBJ_FLAG_HIDDEN);
        if (g_idle_data.azure_on) {
            lv_obj_add_flag(azure_lbl_off, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(azure_lbl_up, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(azure_lbl_dn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(azure_lbl_up, lv_color_hex(IDLE_STATUS_OK), 0);
            lv_obj_set_style_text_opa(azure_lbl_up, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(azure_lbl_dn, lv_color_hex(IDLE_AZURE_DN_BLUE), 0);
            lv_obj_set_style_text_opa(azure_lbl_dn, LV_OPA_COVER, 0);
        } else {
            lv_obj_clear_flag(azure_lbl_off, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(azure_lbl_up, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(azure_lbl_dn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(azure_lbl_off, lv_color_hex(IDLE_SCR_MUTED), 0);
        }
    }
#endif

    if (g_idle_data.y >= 2020 && g_idle_data.wday >= 0 && g_idle_data.wday <= 6) {
        const int wd = g_idle_data.wday % 7;
        const char *wday_str = (wd == 0) ? "Chủ Nhật" : ((wd == 1) ? "Thứ Hai" : ((wd == 2) ? "Thứ Ba" : ((wd == 3) ? "Thứ Tư" : ((wd == 4) ? "Thứ Năm" : ((wd == 5) ? "Thứ Sáu" : "Thứ Bảy")))));
        snprintf(buf, sizeof(buf), "%s\n%02d/%02d/%04d", wday_str,
                 g_idle_data.d, g_idle_data.mo, g_idle_data.y);
        lv_label_set_text(idle_lbl_date_line, buf);
    } else {
        lv_label_set_text(idle_lbl_date_line, "Chờ đồng bộ\n--/--/----");
    }

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", g_idle_data.h, g_idle_data.m, g_idle_data.s);
    lv_label_set_text(lbl_time, buf);

    // Kiểm tra swipe pending (cũng được check ở swipe_check_timer_cb nhưng để chắc chắn)
    if (s_swipe_pending.pending) {
        s_swipe_pending.pending = false;
        show_swipe_popup_from_lvgl();
    }
}

// Timer luôn chạy (bất kể UI state) – đồng bộ việc tạo popup vào lvgl_task
static void swipe_check_timer_cb(lv_timer_t * timer) {
    if (s_swipe_pending.pending) {
        s_swipe_pending.pending = false;
        show_swipe_popup_from_lvgl();
    }
}

/** Nhãn một dòng, chữ chạy ngang trái→phải (không giãn dọc). */
static void idle_label_scroll_h(lv_obj_t *lbl, lv_coord_t w, const lv_font_t *font)
{
    const lv_coord_t lh = lv_font_get_line_height(font);
    lv_obj_set_width(lbl, w);
    lv_obj_set_height(lbl, lh);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *idle_card_style(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
#if BOARD_LCD_V_RES <= 240
    lv_obj_set_style_pad_left(c, 4, 0);
    lv_obj_set_style_pad_right(c, 4, 0);
    lv_obj_set_style_pad_top(c, 4, 0);
    lv_obj_set_style_pad_bottom(c, 4, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
#else
    lv_obj_set_style_pad_all(c, IDLE_BLOCK_GAP, 0);
    lv_obj_set_style_pad_row(c, IDLE_BLOCK_GAP, 0);
#endif
    lv_obj_set_style_bg_color(c, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(IDLE_SCR_CARD_BORDER), 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

int g_idle_layout_version = 2; // 1 = Old, 2 = New

static void build_idle_screen(void);

void lv_port_set_idle_layout(int v)
{
    if (v == 1 || v == 2) {
        if (g_idle_layout_version != v) {
            g_idle_layout_version = v;
            
            nvs_handle_t h;
            if (nvs_open("wifi_portal", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_i32(h, "ui_layout", v);
                nvs_commit(h);
                nvs_close(h);
            }

            if (idle_scr) {
                lv_obj_del(idle_scr);
                idle_scr = NULL;
            }
            build_idle_screen();
            if (g_ui_state == 0) {
                lv_scr_load(idle_scr);
            }
        }
    }
}

static void build_idle_screen(void)
{
    idle_scr = lv_obj_create(NULL);
    lv_obj_set_size(idle_scr, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(idle_scr, lv_color_hex(IDLE_SCR_BG), 0);
    lv_obj_set_style_bg_opa(idle_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(idle_scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(idle_scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(idle_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(idle_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(idle_scr, IDLE_SCR_PAD_X, 0);
    lv_obj_set_style_pad_right(idle_scr, IDLE_SCR_PAD_X, 0);
    lv_obj_set_style_pad_top(idle_scr, IDLE_BLOCK_GAP, 0);
    lv_obj_set_style_pad_bottom(idle_scr, IDLE_BLOCK_GAP, 0);
    lv_obj_set_style_pad_row(idle_scr, 0, 0);

    lv_obj_t *main_row = NULL;
    lv_obj_t *header_row = NULL;
    lv_obj_t *left_panel = NULL;
    lv_obj_t *right_panel = NULL;
    lv_obj_t *status_row = NULL;
    lv_obj_t *clock_card = NULL;

    const lv_coord_t left_txt_w = (lv_coord_t)(IDLE_LEFT_COL_W - 12);

    if (g_idle_layout_version == 1) {
        /* V1: Khung trái (Title, Brand, Date, Time) | Khung phải (Icons + Clock) */
        main_row = lv_obj_create(idle_scr);
        lv_obj_set_size(main_row, IDLE_SCR_INNER_W, IDLE_MAIN_ROW_H);
        lv_obj_set_style_bg_opa(main_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(main_row, 0, 0);
        lv_obj_set_style_pad_all(main_row, 0, 0);
        lv_obj_set_style_pad_column(main_row, IDLE_BLOCK_GAP, 0);
        lv_obj_set_layout(main_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(main_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(main_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(main_row, LV_OBJ_FLAG_SCROLLABLE);

        left_panel = idle_card_style(main_row, IDLE_LEFT_COL_W, LV_PCT(100));
        lv_obj_set_layout(left_panel, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(left_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(left_panel, 8, 0);
        lv_obj_set_style_clip_corner(left_panel, true, 0);

        idle_lbl_title = lv_label_create(left_panel);
        lv_label_set_text(idle_lbl_title, "MÁY CHẤM CÔNG");
        idle_label_scroll_h(idle_lbl_title, left_txt_w, &UI_FONT_BODY);
        lv_obj_set_style_text_color(idle_lbl_title, lv_color_hex(0x2563EB), 0);

        right_panel = idle_card_style(main_row, IDLE_RIGHT_COL_W, LV_PCT(100));
        lv_obj_set_layout(right_panel, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(right_panel, 8, 0);
        lv_obj_set_style_clip_corner(right_panel, true, 0);

        status_row = lv_obj_create(right_panel);
        lv_obj_set_width(status_row, LV_SIZE_CONTENT);
        lv_obj_set_height(status_row, IDLE_RIGHT_STATUS_H);
        lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_row, 0, 0);
        lv_obj_set_style_pad_column(status_row, IDLE_BLOCK_GAP, 0);
        lv_obj_set_layout(status_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        /* V2: Header (Title + Icons) | Main (Brand, Date, Time | Clock) */
        header_row = idle_card_style(idle_scr, IDLE_SCR_INNER_W, LV_SIZE_CONTENT);
        lv_obj_set_layout(header_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(header_row, 4, 0);
        lv_obj_set_style_pad_left(header_row, 12, 0);
        lv_obj_set_style_pad_right(header_row, 12, 0);

        idle_lbl_title = lv_label_create(header_row);
        lv_label_set_text(idle_lbl_title, "MÁY CHẤM CÔNG");
        lv_obj_set_flex_grow(idle_lbl_title, 1);
        lv_label_set_long_mode(idle_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_color(idle_lbl_title, lv_color_hex(0x2563EB), 0);
        lv_obj_set_style_text_font(idle_lbl_title, &UI_FONT_BODY, 0);
        lv_obj_set_style_pad_right(idle_lbl_title, 8, 0);

        status_row = lv_obj_create(header_row);
        lv_obj_set_width(status_row, LV_SIZE_CONTENT);
        lv_obj_set_height(status_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_row, 0, 0);
        lv_obj_set_style_pad_column(status_row, IDLE_BLOCK_GAP, 0);
        lv_obj_set_layout(status_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);

        main_row = lv_obj_create(idle_scr);
        lv_obj_set_width(main_row, IDLE_SCR_INNER_W);
        lv_obj_set_flex_grow(main_row, 1);
        lv_obj_set_style_bg_opa(main_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(main_row, 0, 0);
        lv_obj_set_style_pad_all(main_row, 0, 0);
        lv_obj_set_style_pad_column(main_row, IDLE_BLOCK_GAP, 0);
        lv_obj_set_layout(main_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(main_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(main_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(main_row, LV_OBJ_FLAG_SCROLLABLE);

        left_panel = idle_card_style(main_row, IDLE_LEFT_COL_W, LV_PCT(100));
        lv_obj_set_layout(left_panel, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(left_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(left_panel, 8, 0);
        lv_obj_set_style_clip_corner(left_panel, true, 0);

        right_panel = idle_card_style(main_row, IDLE_RIGHT_COL_W, LV_PCT(100));
        lv_obj_set_layout(right_panel, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(right_panel, 8, 0);
        lv_obj_set_style_clip_corner(right_panel, true, 0);
    }



#if BOARD_ENABLE_WIFI
    idle_wifi_sig = lv_canvas_create(status_row);
    lv_obj_set_size(idle_wifi_sig, IDLE_WIFI_CW, IDLE_WIFI_CH);
    lv_canvas_set_buffer(idle_wifi_sig, s_idle_wifi_buf, IDLE_WIFI_CW, IDLE_WIFI_CH, LV_IMG_CF_TRUE_COLOR);
    idle_wifi_redraw(idle_wifi_sig, -127);
#else
    idle_wifi_sig = lv_label_create(status_row);
    lv_label_set_text(idle_wifi_sig, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(idle_wifi_sig, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(idle_wifi_sig, lv_color_hex(IDLE_SCR_MUTED), 0);
#endif

#if BOARD_ENABLE_AZURE
    idle_azure_holder = lv_obj_create(status_row);
    lv_obj_set_size(idle_azure_holder, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(idle_azure_holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_azure_holder, 0, 0);
    lv_obj_set_style_pad_all(idle_azure_holder, 0, 0);
    lv_obj_set_style_pad_column(idle_azure_holder, IDLE_BLOCK_GAP, 0);
    lv_obj_set_layout(idle_azure_holder, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(idle_azure_holder, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(idle_azure_holder, LV_OBJ_FLAG_SCROLLABLE);

    azure_lbl_off = lv_label_create(idle_azure_holder);
    lv_label_set_text(azure_lbl_off, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(azure_lbl_off, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(azure_lbl_off, lv_color_hex(IDLE_SCR_MUTED), 0);

    azure_lbl_up = lv_label_create(idle_azure_holder);
    lv_label_set_text(azure_lbl_up, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(azure_lbl_up, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(azure_lbl_up, lv_color_hex(IDLE_STATUS_OK), 0);
    lv_obj_add_flag(azure_lbl_up, LV_OBJ_FLAG_HIDDEN);

    azure_lbl_dn = lv_label_create(idle_azure_holder);
    lv_label_set_text(azure_lbl_dn, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(azure_lbl_dn, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(azure_lbl_dn, lv_color_hex(IDLE_AZURE_DN_BLUE), 0);
    lv_obj_add_flag(azure_lbl_dn, LV_OBJ_FLAG_HIDDEN);
#endif



    /* Đọc brand text từ NVS nếu chưa có trong buffer */
    if (s_brand_text[0] == '\0') {
        wifi_portal_get_brand_text(s_brand_text, sizeof(s_brand_text));
    }
    idle_lbl_brand = lv_label_create(left_panel);
    lv_label_set_text(idle_lbl_brand, s_brand_text);
    lv_obj_set_width(idle_lbl_brand, left_txt_w);
    lv_label_set_long_mode(idle_lbl_brand, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(idle_lbl_brand, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(idle_lbl_brand, &UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(idle_lbl_brand, lv_color_hex(0xFFD700), 0);

    idle_lbl_date_line = lv_label_create(left_panel);
    lv_obj_set_width(idle_lbl_date_line, left_txt_w);
    lv_label_set_long_mode(idle_lbl_date_line, LV_LABEL_LONG_WRAP);
    lv_label_set_text(idle_lbl_date_line, "Chờ đồng bộ\n--/--/----");
    lv_obj_set_style_text_font(idle_lbl_date_line, &UI_FONT_BODY, 0);
    lv_obj_set_style_text_line_space(idle_lbl_date_line, 8, 0);
    lv_obj_set_style_text_color(idle_lbl_date_line, lv_color_hex(IDLE_SCR_FG), 0);

    lbl_time = lv_label_create(left_panel);
    lv_label_set_long_mode(lbl_time, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_time, left_txt_w);
    lv_obj_set_style_text_align(lbl_time, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(lbl_time, "00:00:00");
    lv_obj_set_style_text_font(lbl_time, &UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(IDLE_SCR_FG), 0);



#if BOARD_LCD_V_RES <= 240
    lv_coord_t face_w = IDLE_CLOCK_FACE_W;
    lv_coord_t face_h = face_w;
#else
    lv_coord_t face_max_w = (lv_coord_t)(IDLE_RIGHT_COL_W - 8);
    lv_coord_t face_max_h = (lv_coord_t)(BOARD_LCD_V_RES - 8);
    lv_coord_t face_sz = face_max_w < face_max_h ? face_max_w : face_max_h;
    if (face_sz < IDLE_CLOCK_MIN_FACE) {
        face_sz = IDLE_CLOCK_MIN_FACE;
    }
    lv_coord_t face_w = face_sz;
    lv_coord_t face_h = face_sz;
#endif

    clock_card = idle_card_style(right_panel, face_w + 4, face_h + 4);
    lv_obj_set_layout(clock_card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_align(clock_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#if BOARD_LCD_V_RES <= 240
    lv_obj_set_flex_grow(clock_card, 1);
#endif
    lv_obj_set_style_pad_all(clock_card, 2, 0);

    clock_face_cont = lv_obj_create(clock_card);
    lv_obj_set_size(clock_face_cont, face_w, face_h);
    lv_obj_center(clock_face_cont);
#if BOARD_LCD_V_RES <= 240
    lv_obj_set_style_radius(clock_face_cont, 10, 0);
    lv_obj_set_style_border_width(clock_face_cont, 2, 0);
    lv_obj_set_style_border_color(clock_face_cont, lv_color_hex(0x93C5FD), 0);
#else
    lv_obj_set_style_radius(clock_face_cont, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(clock_face_cont, 3, 0);
    lv_obj_set_style_border_color(clock_face_cont, lv_color_hex(0x93C5FD), 0);
#endif
    lv_obj_set_style_bg_color(clock_face_cont, lv_color_hex(0xFAFBFC), 0);
    lv_obj_set_style_bg_opa(clock_face_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(clock_face_cont, 4, 0);
    lv_obj_clear_flag(clock_face_cont, LV_OBJ_FLAG_SCROLLABLE);

    idle_clock_build_face(clock_face_cont);

    line_clk_h = lv_line_create(clock_face_cont);
#if BOARD_LCD_V_RES <= 240
    lv_obj_set_style_line_width(line_clk_h, 3, 0);
#else
    lv_obj_set_style_line_width(line_clk_h, 4, 0);
#endif
    lv_obj_set_style_line_color(line_clk_h, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_line_rounded(line_clk_h, true, 0);

    line_clk_m = lv_line_create(clock_face_cont);
    lv_obj_set_style_line_width(line_clk_m, 2, 0);
    lv_obj_set_style_line_color(line_clk_m, lv_color_hex(0x2563EB), 0);
    lv_obj_set_style_line_rounded(line_clk_m, true, 0);

    line_clk_s = lv_line_create(clock_face_cont);
    lv_obj_set_style_line_width(line_clk_s, 1, 0);
    lv_obj_set_style_line_color(line_clk_s, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_line_rounded(line_clk_s, true, 0);

    lv_obj_t *clk_hub = lv_obj_create(clock_face_cont);
    lv_obj_set_size(clk_hub, BOARD_LCD_V_RES <= 240 ? 6 : 10, BOARD_LCD_V_RES <= 240 ? 6 : 10);
    lv_obj_center(clk_hub);
    lv_obj_set_style_radius(clk_hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(clk_hub, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_border_width(clk_hub, 0, 0);
    lv_obj_clear_flag(clk_hub, LV_OBJ_FLAG_SCROLLABLE);

    idle_clock_hands_update();

    lv_scr_load(idle_scr);

    static bool s_timers_created = false;
    if (!s_timers_created) {
        lv_timer_create(update_ui_timer_cb, 500, NULL);
        lv_timer_create(swipe_check_timer_cb, 40, NULL);
        s_timers_created = true;
    }
}

static esp_lcd_panel_handle_t s_panel;
static lv_disp_draw_buf_t disp_buf;
static lv_color_t *buf1;
/** lvgl_task đã esp_task_wdt_add — feed WDT trong disp_flush khi vẽ nhiều vùng liền (tránh TWDT >5s). */
static volatile bool s_lvgl_wdt_ok;
static uint64_t g_last_activity_us = 0;

/* Hàm xả dữ liệu từ LVGL xuống màn hình */
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    if (s_lvgl_wdt_ok) {
        esp_task_wdt_reset();
    }
    /*
     * LVGL build với LV_COLOR_16_SWAP=1 → buffer chứa RGB565 đã byte‑swap.
     * esp_lcd_panel_draw_bitmap đọc src[i] như RGB565 chuẩn (chỉ swap khi
     * BOARD_LCD_RGB565_SWAP_BYTES=1 — hiện =0 để giữ lcd_ui nguyên). Hai pipeline
     * lệch nhau → border, fill, line bị sai màu (xanh lá, vàng đồng, cam gỉ).
     * Un‑swap tại đây để khớp driver mà không phá lcd_ui (custom UI vẫn dùng
     * RGB565 không swap như cũ).
     */
#if !BOARD_LCD_RGB565_SWAP_BYTES
    {
        const int32_t w = area->x2 - area->x1 + 1;
        const int32_t h = area->y2 - area->y1 + 1;
        uint16_t *p = (uint16_t *)color_p;
        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; i++) {
            p[i] = __builtin_bswap16(p[i]);
        }
    }
#endif
    esp_err_t dr = lcd_ui_draw_bitmap_sync(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    if (dr != ESP_OK) {
        ESP_LOGW(TAG, "disp_flush: %s", esp_err_to_name(dr));
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (s_lvgl_wdt_ok) {
        esp_task_wdt_reset();  /* Feed sau flush: flush dài có thể vượt 5s trên 1 frame lớn */
    }
    lv_disp_flush_ready(disp_drv);
}

/* LVGL Tick (cung cấp thời gian thực cho LVGL) */
static void lv_tick_task(void *arg)
{
    lv_tick_inc(2); // Cấp 2ms mỗi lần gọi
}

void lv_port_feed_wdt(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

/* Task xử lý chính của LVGL */
static void lvgl_task(void *arg)
{
    (void)arg;
    /* TWDT: chỉ reset khi add thành công (menuconfig có thể không bật user tasks) */
    const bool wdt_ok = (esp_task_wdt_add(xTaskGetCurrentTaskHandle()) == ESP_OK);
    s_lvgl_wdt_ok = wdt_ok;

    while (1) {
        /* Feed trước/sau lv_timer_handler — một lần gọi có thể kéo dài (nhiều flush SPI). */
        if (wdt_ok) {
            esp_task_wdt_reset();
        }
        vTaskDelay(pdMS_TO_TICKS(15));
        lv_timer_handler();
        lv_port_wifi_scan_poll_ui();
        lv_port_wifi_conn_poll_ui();
        if (g_force_ui_update) {
            g_force_ui_update = false;
            lv_port_refresh_current_screen();
        }
        
#if BOARD_ENABLE_LOGIN
        if (g_ui_state != 0 && (esp_timer_get_time() - g_last_activity_us) > 60000000ULL) {
            ESP_LOGI(TAG, "Inactivity timeout: Returning to Idle Screen");
            g_ui_state = 0;
            lv_scr_load((lv_obj_t *)lv_port_get_idle_scr());
        }
#endif

        /* Diagnostic log mỗi 60 giây để giám sát tài nguyên */
        // static uint64_t last_diag_us = 0;
        // uint64_t now_us = esp_timer_get_time();
        // if (now_us - last_diag_us > 60000000ULL) {
        //     last_diag_us = now_us;
        //     size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        //     size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        //     ESP_LOGI(TAG, "[HEARTBEAT] Free RAM: %u internal, %u PSRAM | Task: Prio 5, Stack OK", 
        //              (unsigned)free_internal, (unsigned)free_spiram);
        // }

        if (wdt_ok) {
            esp_task_wdt_reset();
        }
    }
}

void lv_port_refresh_current_screen(void)
{
#if !BOARD_ENABLE_LOGIN
    return;
#endif
    if (g_ui_state == 2) {
        lv_port_show_menu_screen(g_menu_active_item);
    } else if (g_ui_state == 3) {
        lv_port_show_settings_menu(g_menu_active_item);
    } else if (g_ui_state == 4) {
        lv_port_show_change_password(g_old_pin, g_new_pin, g_pwd_active_field, g_pwd_error);
    } else if (g_ui_state == 5 || g_ui_state == 6 || g_ui_state == 7 || g_ui_state == 13) {
        lv_port_show_wifi_manager(g_ui_state, g_wifi_selected_ssid, g_wifi_entered_pass);
    } else if (g_ui_state == 8) {
        lv_port_show_log_screen(g_log_page);
    } else if (g_ui_state == 9) {
        lv_port_show_card_list(g_card_page, false);
    } else if (g_ui_state == 10) {
        lv_port_show_card_list(g_card_page, true);
    } else if (g_ui_state == 11) {
        lv_port_show_card_edit(g_edit_uid, g_edit_name, g_edit_id, g_edit_field);
    } else if (g_ui_state == 12) {
        lv_port_show_confirm_screen(g_confirm_type, g_confirm_arg_str, g_confirm_arg_str2);
    }
}

static lv_obj_t * swipe_popup = NULL;
static lv_timer_t * swipe_timer = NULL;
static lv_obj_t * swipe_info_col = NULL;

static void swipe_popup_close_cb(lv_timer_t * timer) {
    (void)timer;
    swipe_info_col = NULL;
    if (swipe_popup) {
        lv_obj_del(swipe_popup);
        swipe_popup = NULL;
    }
    swipe_timer = NULL;
}

/** Nhãn popup quẹt thẻ — wrap, cao theo nội dung (không kéo giãn khoảng trống). */
static lv_obj_t *swipe_add_text_label(lv_obj_t *parent, const char *text, uint32_t color_hex, bool wrap)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_long_mode(lbl, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_text(lbl, text && text[0] ? text : "-");
    lv_obj_set_style_text_font(lbl, &arial_vn_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color_hex), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    return lbl;
}

static void show_swipe_popup_from_lvgl(void) {
    if (lv_disp_get_default() == NULL) {
        return;
    }

    if (swipe_popup) {
        lv_obj_del(swipe_popup);
        swipe_popup = NULL;
    }
    if (swipe_timer) {
        lv_timer_del(swipe_timer);
        swipe_timer = NULL;
    }
    swipe_info_col = NULL;

    const lv_coord_t sw_w = UI_SWIPE_POPUP_W;
    const lv_coord_t sw_h = UI_SWIPE_POPUP_H;

    swipe_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(swipe_popup, sw_w, sw_h);
    lv_obj_center(swipe_popup);

    if (s_swipe_pending.success) {
        lv_obj_set_style_bg_color(swipe_popup, lv_color_hex(0x064E3B), 0);
        lv_obj_set_style_border_color(swipe_popup, lv_color_hex(0x10B981), 0);
    } else {
        lv_obj_set_style_bg_color(swipe_popup, lv_color_hex(0x7F1D1D), 0);
        lv_obj_set_style_border_color(swipe_popup, lv_color_hex(0xEF4444), 0);
    }
    lv_obj_set_style_border_width(swipe_popup, 3, 0);
    lv_obj_set_style_radius(swipe_popup, 12, 0);
    lv_obj_set_layout(swipe_popup, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(swipe_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(swipe_popup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(swipe_popup, 6, 0);
    lv_obj_set_style_pad_row(swipe_popup, 4, 0);
    lv_obj_clear_flag(swipe_popup, LV_OBJ_FLAG_SCROLLABLE);

    swipe_info_col = lv_obj_create(swipe_popup);
    lv_obj_set_width(swipe_info_col, LV_PCT(100));
    lv_obj_set_height(swipe_info_col, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(swipe_info_col, sw_h - 14, 0);
    lv_obj_set_style_bg_opa(swipe_info_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(swipe_info_col, 0, 0);
    lv_obj_set_style_pad_all(swipe_info_col, 0, 0);
    lv_obj_set_layout(swipe_info_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(swipe_info_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(swipe_info_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(swipe_info_col, 2, 0);
    lv_obj_add_flag(swipe_info_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(swipe_info_col, LV_DIR_VER);

    swipe_add_text_label(swipe_info_col, s_swipe_pending.display_name, 0xFFFFFF, true);
    swipe_add_text_label(swipe_info_col, s_swipe_pending.emp_id_line, 0xCBD5E1, true);
    swipe_add_text_label(swipe_info_col, s_swipe_pending.time_str, 0x94A3B8, false);

    swipe_timer = lv_timer_create(swipe_popup_close_cb, 3000, NULL);
    lv_timer_set_repeat_count(swipe_timer, 1);
}

// Gọi từ rfid_task - chỉ ghi dữ liệu, KHÔNG gọi LVGL API trực tiếp
void lv_port_show_swipe_result(const char *display_name, const char *emp_id_line, const char *time_s, bool success,
                               int check_type)
{
    strncpy(s_swipe_pending.display_name, display_name ? display_name : "", sizeof(s_swipe_pending.display_name) - 1);
    strncpy(s_swipe_pending.emp_id_line, emp_id_line ? emp_id_line : "", sizeof(s_swipe_pending.emp_id_line) - 1);
    strncpy(s_swipe_pending.time_str, time_s ? time_s : "", sizeof(s_swipe_pending.time_str) - 1);
    s_swipe_pending.display_name[sizeof(s_swipe_pending.display_name) - 1] = '\0';
    s_swipe_pending.emp_id_line[sizeof(s_swipe_pending.emp_id_line) - 1] = '\0';
    s_swipe_pending.time_str[sizeof(s_swipe_pending.time_str) - 1] = '\0';
    s_swipe_pending.success = success;
    s_swipe_pending.check_type = check_type;
    s_swipe_pending.pending = true;
    g_last_activity_us = esp_timer_get_time();
}

void lv_port_show_swipe_result_with_img(const char *display_name, const char *emp_id_line, const char *time_s,
                                        const char *img_path, bool show_photo, bool success, int check_type)
{
    (void)img_path;
    (void)show_photo;
    lv_port_show_swipe_result(display_name, emp_id_line, time_s, success, check_type);
}

void *lv_port_get_idle_scr(void)
{
    return (void *)idle_scr;
}

void lv_port_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
    
    nvs_handle_t h;
    int32_t val = 2;
    if (nvs_open("wifi_portal", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "ui_layout", &val) == ESP_OK) {
            if (val == 1 || val == 2) {
                g_idle_layout_version = (int)val;
            }
        }
        nvs_close(h);
    }
    
    /* Khởi tạo thư viện lõi LVGL */
    lv_init();
    
    g_last_activity_us = esp_timer_get_time();


    /* Buffer LVGL: ưu tiên RAM nội DMA (ổn định hơn SPIRAM khi WiFi/Azure tranh bus). */
    uint32_t buf_size = BOARD_LCD_H_RES * (uint32_t)BOARD_LCD_SPI_CHUNK_LINES;
    const size_t buf_bytes = buf_size * sizeof(lv_color_t);
    buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1) {
        buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    }
    
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, buf_size);

    /* Đăng ký Display driver (Màn hình) */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BOARD_LCD_H_RES;
    disp_drv.ver_res = BOARD_LCD_V_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_t * disp = lv_disp_drv_register(&disp_drv);

#if LV_USE_THEME_DEFAULT
    /* Light theme khớp màn chờ (nền F1F5F9) — tránh chồng dark theme + màn sáng. */
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_LIGHT_BLUE), false,
                          LV_FONT_DEFAULT);
#endif
    lv_disp_set_bg_color(disp, lv_color_hex(IDLE_SCR_BG));
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(lv_layer_sys(), LV_OPA_TRANSP, 0);

    /* Tạo hardware timer tick cho LVGL (Chu kỳ 2ms) */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t periodic_timer;
    esp_timer_create(&periodic_timer_args, &periodic_timer);
    esp_timer_start_periodic(periodic_timer, 2000); // 2ms = 2000us

    /*
     * LVGL không thread-safe: phải dựng màn hình xong *trước* khi chạy lvgl_task.
     * (Trước đây tạo task trước build_idle_screen → lv_timer_handler và main cùng gọi LVGL → treo/đơ.)
     */
    build_idle_screen();
    update_ui_timer_cb(NULL);

    /* Tăng stack lên 12KB để an toàn khi dùng nhiều font/widget phức tạp */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 12288, NULL, 5, NULL, 1);
}

/* ----------------------------------------------------------------
 * Cập nhật chữ thương hiệu trên màn idle (gọi từ LVGL task)
 * ---------------------------------------------------------------- */
void lv_port_set_brand_text(const char *txt)
{
    if (!txt) return;
    extern int g_ui_state;
    /* Cập nhật buffer in-memory */
    snprintf(s_brand_text, sizeof(s_brand_text), "%s", txt);
    /* Cập nhật label LVGL trực tiếp nếu màn idle đang hiển thị */
    if (idle_lbl_brand && g_ui_state == 0) {
        lv_label_set_text(idle_lbl_brand, s_brand_text);
    }
}
