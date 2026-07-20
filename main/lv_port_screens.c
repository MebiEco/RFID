/**
 * Màn chức năng LVGL (Menu, Settings, WiFi, Log, Card, Confirm).
 */
#include "lv_port.h"
#include "lvgl.h"
#include "app_rfid.h"
#include "board_pins.h"
#include "ui_layout.h"
#include "ui_log_model.h"
#include "ui_card_cache.h"
#include "card_profile.h"
#include "wifi_portal.h"
#include "scan_log.h"
#include "esp_timer.h" // For getting tick count

#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if BOARD_ENABLE_WIFI
#include "esp_wifi.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_azure.h"
#include "app_audio.h"

LV_FONT_DECLARE(arial_vn_24);
LV_FONT_DECLARE(arial_vn_32);
LV_FONT_DECLARE(lv_font_montserrat_14);

static const char *TAG = "lv_port_ui";

static void ui_refresh(void)
{
    g_force_ui_update = true;
}

/* ------------- Theme ------------- */
static void style_scr_dark(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF1F5F9), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0x0F172A), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

/* ------------- Forward ------------- */
static lv_obj_t *scr_menu;
static lv_obj_t *menu_btns[5];

static lv_obj_t *scr_settings;
static lv_obj_t *set_btns[3];

static lv_obj_t *scr_pwd;
static lv_obj_t *ta_pwd_old;
static lv_obj_t *ta_pwd_new;
static lv_obj_t *lbl_pwd_err;
static lv_obj_t *pwd_kb_ref;

static lv_obj_t *scr_wifi;
static lv_obj_t *wifi_title_lbl;
static lv_obj_t *wifi_list_box;
static lv_obj_t *wifi_ta_pass;
static lv_obj_t *wifi_kb;
static lv_obj_t *wifi_lbl_ssid;
static lv_obj_t *wifi_btn_back_bt;
static lv_obj_t *wifi_btn_save_bt;
static lv_obj_t *wifi_btn_del_bt;
static int       wifi_last_state = -999;

static lv_obj_t *scr_log;
static lv_obj_t *log_row_lbl[UI_LOG_ROWS_PER_PAGE];
static lv_obj_t *log_row_sub_lbl[UI_LOG_ROWS_PER_PAGE];
static lv_obj_t *lbl_log_title;

static lv_obj_t *scr_cards;
static lv_obj_t *lbl_cards_hdr;
static lv_obj_t *card_row_btns[CARD_PROFILE_PAGE_ROWS];
static lv_obj_t *card_del_btns[CARD_PROFILE_PAGE_ROWS];
static lv_obj_t *btn_card_prev;
static lv_obj_t *btn_card_next;

static lv_obj_t *scr_edit;
static lv_obj_t *ta_edit_name;
static lv_obj_t *ta_edit_id;
static lv_obj_t *kb_edit;

static lv_obj_t *confirm_box;
static bool      s_wifi_conn_waiting = false;
static uint32_t  s_wifi_conn_start_tick = 0;
static lv_obj_t *wifi_conn_msg_lbl;

static void sync_edit_from_ta(void);

/* ------------- Confirm logic (mirror app_touch) ------------- */
static void confirm_del_cb(lv_event_t *e)
{
    (void)e;
    if (confirm_box) {
        lv_obj_del(confirm_box);
        confirm_box = NULL;
    }
    g_ui_state = g_confirm_from_state;
    ui_refresh();
}

static void confirm_yes_cb(lv_event_t *e)
{
    (void)e;
    bool action_ok = false;

    if (confirm_box && g_confirm_type != 4) {
        lv_obj_del(confirm_box);
        confirm_box = NULL;
    }

    if (g_confirm_type == 1) {
        esp_err_t del_err = card_profile_delete(g_confirm_arg_str);
        if (del_err != ESP_OK) {
            ESP_LOGW(TAG, "Xoa the that bai UID=%s (%s)", g_confirm_arg_str, esp_err_to_name(del_err));
            g_ui_state = g_confirm_from_state;
        } else {
            int32_t msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
#if BOARD_ENABLE_AZURE
            app_azure_send_card_event(g_confirm_arg_str, g_confirm_arg_str2, "", 604, msg_idx);
#endif
            scan_log_append_admin(g_confirm_arg_str, g_confirm_arg_str2, "", "DEL", msg_idx);
            ui_card_cache_invalidate();
            g_ui_state = g_confirm_from_state;
            action_ok = true;
        }
    } else if (g_confirm_type == 2) {
        sync_edit_from_ta();
        esp_err_t sav = card_profile_save(g_edit_uid, g_edit_name, g_edit_id);
        if (sav != ESP_OK) {
            ESP_LOGW(TAG, "Luu the that bai UID=%s (%s)", g_edit_uid, esp_err_to_name(sav));
            g_ui_state = g_confirm_from_state;
            wifi_last_state = -999;
            ui_refresh();
            return;
        }
        int32_t msg_idx = app_azure_get_and_increment_msg_index(MSG_IDX_ADMIN);
#if BOARD_ENABLE_AZURE
        app_azure_send_card_event(g_edit_uid, g_edit_name, g_edit_id, 603, msg_idx);
#endif
        scan_log_append_admin(g_edit_uid, g_edit_name, g_edit_id, "SAVE", msg_idx);
        ui_card_cache_invalidate();
        g_ui_state = g_edit_from_state;
        action_ok = true;
    } else if (g_confirm_type == 3) {
        wifi_list_remove(g_confirm_arg_str);
        g_ui_state = g_confirm_from_state;
        action_ok = true;
    } else if (g_confirm_type == 4) {
        wifi_portal_connect_to(g_wifi_selected_ssid, g_wifi_entered_pass);
        s_wifi_conn_waiting = true;
        s_wifi_conn_start_tick = (uint32_t)(esp_timer_get_time() / 1000);
        
        // Show connecting status in confirm box
        if (confirm_box) {
            lv_obj_clean(confirm_box);
        }
        lv_obj_t *t = lv_label_create(confirm_box);
        lv_label_set_text(t, "ĐANG KẾT NỐI...");
        lv_obj_set_style_text_font(t, &arial_vn_24, 0);
        lv_obj_center(t);
        wifi_conn_msg_lbl = t;
        
        return; // Don't delete confirm_box yet
    } else if (g_confirm_type == 5) {
        if (app_login_save_new_pin(g_new_pin) != ESP_OK) {
            ESP_LOGW(TAG, "Luu PIN NVS that bai");
            g_pwd_error = true;
            g_ui_state = 4;
        } else {
            memset(g_old_pin, 0, sizeof(g_old_pin));
            memset(g_new_pin, 0, sizeof(g_new_pin));
            g_ui_state = 3;
            action_ok = true;
        }
    }

    if (action_ok) {
        app_audio_play_confirm();
    }

    wifi_last_state = -999;
    ui_refresh();
}

/* ------------- Menu ------------- */
static void menu_btn_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    g_menu_active_item = id;
    if (id == 0) {
        g_ui_state = 3;
        lv_port_show_settings_menu(0);
    } else if (id == 1) {
        g_log_page = 0;
        g_ui_state = 8;
        lv_port_show_log_screen(0);
    } else if (id == 2) {
        g_card_page = 0;
        g_ui_state = 9;
        lv_port_show_card_list(0, false);
    } else if (id == 3) {
        g_card_page = 0;
        g_ui_state = 10;
        lv_port_show_card_list(0, true);
    } else {
        g_ui_state = 0;
        lv_scr_load((lv_obj_t *)lv_port_get_idle_scr());
    }
    ui_refresh();
}

void lv_port_show_menu_screen(int active_item)
{
    if (!scr_menu) {
        scr_menu = lv_obj_create(NULL);
        style_scr_dark(scr_menu);

        lv_obj_t *hdr = lv_label_create(scr_menu);
        lv_label_set_text(hdr, "MENU HỆ THỐNG");
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_set_style_text_font(hdr, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x2563EB), 0);

        const char *titles[] = {"Cài Đặt", "Nhật Ký", "DS Thẻ", "Đăng Ký", "Quay Lại"};
        /* Lưới 2x2 + nút quay lại */
        for (int i = 0; i < 4; i++) {
            int row = i / 2;
            int col = i % 2;
            menu_btns[i] = lv_btn_create(scr_menu);
            lv_obj_set_size(menu_btns[i], UI_MENU_BTN_W, UI_MENU_BTN_H);
            lv_obj_set_pos(menu_btns[i],
                           col ? UI_MENU_COL1_X : UI_MENU_COL0_X,
                           UI_MENU_GRID_Y + row * (UI_MENU_BTN_H + UI_MENU_ROW_GAP));
            lv_obj_t *lb = lv_label_create(menu_btns[i]);
            lv_label_set_text(lb, titles[i]);
            lv_obj_center(lb);
            lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
            lv_obj_add_event_cb(menu_btns[i], menu_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        menu_btns[4] = lv_btn_create(scr_menu);
        lv_obj_set_size(menu_btns[4], UI_MENU_BACK_W, UI_MENU_BACK_H);
        lv_obj_align(menu_btns[4], LV_ALIGN_BOTTOM_MID, 0, -UI_BACK_BOTTOM_Y);
        lv_obj_t *lb = lv_label_create(menu_btns[4]);
        lv_label_set_text(lb, titles[4]);
        lv_obj_center(lb);
        lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(menu_btns[4], menu_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)4);
    }
    for (int i = 0; i < 5; i++) {
        lv_color_t c = (i == active_item) ? lv_color_hex(0x2563EB) : lv_color_hex(0x0F172A);
        lv_obj_set_style_bg_color(menu_btns[i], c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(menu_btns[i], 0), lv_color_hex(0xFFFFFF), 0);
    }
    lv_scr_load(scr_menu);
}

/* ------------- Settings ------------- */
static void settings_btn_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    g_menu_active_item = id;
    if (id == 0) {
        g_ui_state = 5;
        wifi_last_state = -999;
        lv_port_show_wifi_manager(5, g_wifi_selected_ssid, g_wifi_entered_pass);
        lv_port_wifi_request_scan();
    } else if (id == 1) {
        memset(g_old_pin, 0, sizeof(g_old_pin));
        memset(g_new_pin, 0, sizeof(g_new_pin));
        g_pwd_active_field = 0;
        g_pwd_error = false;
        g_ui_state = 4;
        lv_port_show_change_password(g_old_pin, g_new_pin, g_pwd_active_field, g_pwd_error);
    } else {
        g_ui_state = 2;
        lv_port_show_menu_screen(0);
    }
    ui_refresh();
}

void lv_port_show_settings_menu(int active_item)
{
    if (!scr_settings) {
        scr_settings = lv_obj_create(NULL);
        style_scr_dark(scr_settings);

        lv_obj_t *hdr = lv_label_create(scr_settings);
        lv_label_set_text(hdr, "CÀI ĐẶT");
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_set_style_text_font(hdr, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x2563EB), 0);

        const char *titles[] = {"QUẢN LÝ WIFI", "ĐỔI MẬT KHẨU", "QUAY LẠI"};
        for (int i = 0; i < 3; i++) {
            set_btns[i] = lv_btn_create(scr_settings);
            lv_obj_set_size(set_btns[i], UI_SETTINGS_BTN_W, UI_SETTINGS_BTN_H);
            lv_obj_align(set_btns[i], LV_ALIGN_TOP_MID, 0, UI_SETTINGS_BTN_Y0 + i * UI_SETTINGS_BTN_STEP);
            lv_obj_t *lb = lv_label_create(set_btns[i]);
            lv_label_set_text(lb, titles[i]);
            lv_obj_center(lb);
            lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
            lv_obj_add_event_cb(set_btns[i], settings_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
    for (int i = 0; i < 3; i++) {
        lv_color_t c = (i == active_item) ? lv_color_hex(0x2563EB) : lv_color_hex(0x0F172A);
        lv_obj_set_style_bg_color(set_btns[i], c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(set_btns[i], 0), lv_color_hex(0xFFFFFF), 0);
    }
    lv_scr_load(scr_settings);
}

/* ------------- Change password ------------- */
static void pwd_kb_cb(lv_event_t *e)
{
    /* lv_keyboard đã có lv_keyboard_def_event_cb — không gọi lại (tránh nhập đúp). */
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        strncpy(g_old_pin, lv_textarea_get_text(ta_pwd_old), sizeof(g_old_pin) - 1);
        strncpy(g_new_pin, lv_textarea_get_text(ta_pwd_new), sizeof(g_new_pin) - 1);
        if (strlen(g_new_pin) == 0) {
            g_pwd_error = true;
            lv_port_show_change_password(g_old_pin, g_new_pin, g_pwd_active_field, true);
            ui_refresh();
            return;
        }
        if (!app_login_verify_pin(g_old_pin)) {
            g_pwd_error = true;
            memset(g_old_pin, 0, sizeof(g_old_pin));
            g_pwd_active_field = 0;
            lv_port_show_change_password(g_old_pin, g_new_pin, g_pwd_active_field, true);
            ui_refresh();
            return;
        }
        g_confirm_type = 5;
        g_confirm_from_state = 4;
        g_ui_state = 12;
        lv_port_show_confirm_screen(5, NULL, NULL);
    } else if (code == LV_EVENT_CANCEL) {
        g_ui_state = 3;
        lv_port_show_settings_menu(2);
    }
}

static void pwd_field_cb(lv_event_t *e)
{
    intptr_t f = (intptr_t)lv_event_get_user_data(e);
    g_pwd_active_field = (int)f;
}

void lv_port_show_change_password(const char *old_pin, const char *new_pin, int active_field, bool is_error)
{
    if (!scr_pwd) {
        scr_pwd = lv_obj_create(NULL);
        style_scr_dark(scr_pwd);

        lv_obj_t *hdr = lv_label_create(scr_pwd);
        lv_label_set_text(hdr, "ĐỔI MẬT KHẨU");
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_set_style_text_font(hdr, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x2563EB), 0);

        lbl_pwd_err = lv_label_create(scr_pwd);
        lv_obj_align(lbl_pwd_err, LV_ALIGN_TOP_MID, 0, 28);
        lv_obj_set_style_text_font(lbl_pwd_err, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl_pwd_err, lv_color_hex(0xDC2626), 0);

        ta_pwd_old = lv_textarea_create(scr_pwd);
        lv_textarea_set_one_line(ta_pwd_old, true);
        lv_textarea_set_password_mode(ta_pwd_old, true);
        lv_obj_set_width(ta_pwd_old, BOARD_LCD_H_RES - 40);
        lv_obj_align(ta_pwd_old, LV_ALIGN_TOP_MID, 0, 48);
        lv_obj_set_style_bg_color(ta_pwd_old, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(ta_pwd_old, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(ta_pwd_old, lv_color_hex(0x3B82F6), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_pwd_old, lv_color_hex(0xCBD5E1), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ta_pwd_old, 2, 0);
        lv_obj_set_style_text_font(ta_pwd_old, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(ta_pwd_old, pwd_field_cb, LV_EVENT_FOCUSED, (void *)0);

        ta_pwd_new = lv_textarea_create(scr_pwd);
        lv_textarea_set_one_line(ta_pwd_new, true);
        lv_textarea_set_password_mode(ta_pwd_new, true);
        lv_obj_set_width(ta_pwd_new, BOARD_LCD_H_RES - 40);
        lv_obj_align(ta_pwd_new, LV_ALIGN_TOP_MID, 0, 88);
        lv_obj_set_style_bg_color(ta_pwd_new, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(ta_pwd_new, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(ta_pwd_new, lv_color_hex(0x3B82F6), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_pwd_new, lv_color_hex(0xCBD5E1), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ta_pwd_new, 2, 0);
        lv_obj_set_style_text_font(ta_pwd_new, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(ta_pwd_new, pwd_field_cb, LV_EVENT_FOCUSED, (void *)1);

        pwd_kb_ref = lv_keyboard_create(scr_pwd);
        lv_keyboard_set_mode(pwd_kb_ref, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(pwd_kb_ref, active_field == 0 ? ta_pwd_old : ta_pwd_new);
        lv_keyboard_set_popovers(pwd_kb_ref, true);
        lv_obj_set_style_bg_color(pwd_kb_ref, lv_color_hex(0xF1F5F9), 0);
        lv_obj_set_style_text_font(pwd_kb_ref, LV_FONT_DEFAULT, 0);
        lv_obj_set_size(pwd_kb_ref, BOARD_LCD_H_RES, UI_PWD_KB_H);
        lv_obj_align(pwd_kb_ref, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(pwd_kb_ref, pwd_kb_cb, LV_EVENT_ALL, NULL);
    }

    lv_label_set_text(lbl_pwd_err, is_error ? "LỖI MẬT KHẨU!" : "");
    lv_textarea_set_text(ta_pwd_old, old_pin ? old_pin : "");
    lv_textarea_set_text(ta_pwd_new, new_pin ? new_pin : "");
    if (pwd_kb_ref) {
        lv_keyboard_set_textarea(pwd_kb_ref, active_field == 0 ? ta_pwd_old : ta_pwd_new);
    }

    lv_scr_load(scr_pwd);
}

/* ------------- WiFi ------------- */
/** Không gọi esp_wifi_scan_start trực tiếp trong LVGL/event: scan đồng bộ chặn lvgl_task → TWDT reset. */
#define WIFI_SCAN_WORKER_STACK_BYTES 16384

static volatile bool s_wifi_scan_busy;
/** Worker set, lvgl_task xử lý — tránh lv_async_call / lv_mem từ thread khác (heap LVGL không mutex). */
static volatile bool s_wifi_scan_ui_pending;

static void wifi_conn_done_timer_cb(lv_timer_t *t)
{
    int success = (int)(intptr_t)t->user_data;
    if (confirm_box) {
        lv_obj_del(confirm_box);
        confirm_box = NULL;
    }
    if (success) {
        g_ui_state = 5;
        lv_port_show_wifi_manager(5, g_wifi_selected_ssid, g_wifi_entered_pass);
    } else {
        // Stay on state 7 or 13 (Details/Entry)
        lv_port_show_wifi_manager(g_ui_state, g_wifi_selected_ssid, g_wifi_entered_pass);
    }
    ui_refresh();
}

void lv_port_wifi_conn_poll_ui(void)
{
    if (!s_wifi_conn_waiting) return;

    wifi_conn_status_t status = wifi_portal_get_conn_status();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    
    bool done = false;
    if (status == WIFI_STATUS_CONNECTED) {
        lv_label_set_text(wifi_conn_msg_lbl, "#059669 KẾT NỐI THÀNH CÔNG#");
        lv_label_set_recolor(wifi_conn_msg_lbl, true);
        done = true;
    } else if (status == WIFI_STATUS_FAIL) {
        lv_label_set_text(wifi_conn_msg_lbl, "#DC2626 SAI MẬT KHẨU\nHOẶC LỖI KẾT NỐI#");
        lv_label_set_recolor(wifi_conn_msg_lbl, true);
        done = true;
    } else if (now - s_wifi_conn_start_tick > 15000) {
        lv_label_set_text(wifi_conn_msg_lbl, "#DC2626 QUÁ THỜI GIAN#");
        lv_label_set_recolor(wifi_conn_msg_lbl, true);
        done = true;
    }

    if (done) {
        s_wifi_conn_waiting = false;
        // Wait 2s to show result then close
        lv_timer_t * timer = lv_timer_create(wifi_conn_done_timer_cb, 2000, (void*)(intptr_t)(status == WIFI_STATUS_CONNECTED ? 1 : 0));
        lv_timer_set_repeat_count(timer, 1);
    }
}

void lv_port_wifi_scan_poll_ui(void)
{
    if (!s_wifi_scan_ui_pending) {
        return;
    }
    s_wifi_scan_ui_pending = false;
    s_wifi_scan_busy = false;
    g_ui_state = 6;
    wifi_last_state = -999;
    lv_port_show_wifi_manager(g_ui_state, g_wifi_selected_ssid, g_wifi_entered_pass);
    ui_refresh();
}

static void wifi_scan_worker_task(void *arg)
{
    (void)arg;
#if BOARD_ENABLE_WIFI
    wifi_scan_config_t scan_config = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(err));
        g_wifi_scan_count = 0;
    } else {
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num == 0) {
            g_wifi_scan_count = 0;
        } else {
            uint16_t max_rec = ap_num;
            if (max_rec > 48) {
                max_rec = 48;
            }
            wifi_ap_record_t *aps = malloc((size_t)max_rec * sizeof(wifi_ap_record_t));
            if (!aps) {
                ESP_LOGW(TAG, "wifi scan: malloc AP list failed");
                g_wifi_scan_count = 0;
            } else {
                uint16_t rec = max_rec;
                esp_err_t gr = esp_wifi_scan_get_ap_records(&rec, aps);
                if (gr != ESP_OK) {
                    ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records: %s", esp_err_to_name(gr));
                    g_wifi_scan_count = 0;
                } else {
                    g_wifi_scan_count = (int)rec;
                    if (g_wifi_scan_count > 5) {
                        g_wifi_scan_count = 5;
                    }
                    for (int j = 0; j < g_wifi_scan_count; j++) {
                        strncpy(g_wifi_scan_res[j], (char *)aps[j].ssid, 32);
                        g_wifi_scan_res[j][32] = '\0';
                    }
                }
                free(aps);
            }
        }
    }
#else
    g_wifi_scan_count = 0;
    ESP_LOGW(TAG, "WiFi disabled (BOARD_ENABLE_WIFI=0)");
#endif
    s_wifi_scan_ui_pending = true;
    vTaskDelete(NULL);
}

void lv_port_wifi_request_scan(void)
{
    if (s_wifi_scan_busy) {
        return;
    }
    s_wifi_scan_busy = true;
    BaseType_t ok =
        xTaskCreate(wifi_scan_worker_task, "wifi_scan", WIFI_SCAN_WORKER_STACK_BYTES, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_wifi_scan_busy = false;
        ESP_LOGW(TAG, "xTaskCreate(wifi_scan) failed");
    }
}

static void wifi_ssid_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_ui_state == 5) {
        char pass[64];
        wifi_list_get_item(idx, g_wifi_selected_ssid, pass);
        strncpy(g_wifi_entered_pass, pass, sizeof(g_wifi_entered_pass) - 1);
        g_soft_kb_upper = false;
        g_soft_kb_sym_page = 0;
        g_ui_state = 13; // State 13: Xem chi tiết WiFi đã lưu (Không hiện bàn phím)
        lv_port_show_wifi_manager(13, g_wifi_selected_ssid, g_wifi_entered_pass);
    } else if (g_ui_state == 6) {
        strncpy(g_wifi_selected_ssid, g_wifi_scan_res[idx], sizeof(g_wifi_selected_ssid) - 1);
        g_wifi_selected_ssid[sizeof(g_wifi_selected_ssid) - 1] = '\0';
        memset(g_wifi_entered_pass, 0, sizeof(g_wifi_entered_pass));
        g_soft_kb_upper = false;
        g_soft_kb_sym_page = 0;
        g_ui_state = 7;
        lv_port_show_wifi_manager(7, g_wifi_selected_ssid, g_wifi_entered_pass);
    }
    ui_refresh();
}

static void wifi_back_cb(lv_event_t *e)
{
    (void)e;
    if (g_ui_state == 5 || g_ui_state == 6) {
        g_ui_state = 3;
        lv_port_show_settings_menu(2);
    } else if (g_ui_state == 7) {
        g_ui_state = 6;
        lv_port_show_wifi_manager(6, g_wifi_selected_ssid, g_wifi_entered_pass);
    } else if (g_ui_state == 13) {
        g_ui_state = 5;
        lv_port_show_wifi_manager(5, g_wifi_selected_ssid, g_wifi_entered_pass);
    }
    wifi_last_state = -999;
    ui_refresh();
}

static void wifi_scan_more_cb(lv_event_t *e)
{
    (void)e;
    lv_port_wifi_request_scan();
}

static void wifi_del_btn_cb(lv_event_t *e)
{
    (void)e;
    g_confirm_type = 3;
    g_confirm_from_state = 5;
    strncpy(g_confirm_arg_str, g_wifi_selected_ssid, sizeof(g_confirm_arg_str) - 1);
    g_confirm_arg_str[sizeof(g_confirm_arg_str) - 1] = '\0';
    g_ui_state = 12;
    lv_port_show_confirm_screen(3, g_confirm_arg_str, NULL);
    ui_refresh();
}

static void wifi_save_cb(lv_event_t *e)
{
    (void)e;
    strncpy(g_wifi_entered_pass, lv_textarea_get_text(wifi_ta_pass), sizeof(g_wifi_entered_pass) - 1);
    g_wifi_entered_pass[sizeof(g_wifi_entered_pass) - 1] = '\0';
    g_confirm_type = 4;
    g_confirm_from_state = 7;
    g_ui_state = 12;
    lv_port_show_confirm_screen(4, g_wifi_selected_ssid, NULL);
    ui_refresh();
}

void lv_port_show_wifi_manager(int state, const char *selected_ssid, const char *entered_pass)
{
    if (!scr_wifi) {
        scr_wifi = lv_obj_create(NULL);
        style_scr_dark(scr_wifi);
        wifi_title_lbl = lv_label_create(scr_wifi);
        lv_obj_align(wifi_title_lbl, LV_ALIGN_TOP_MID, 0, UI_HDR_TOP_Y);
        lv_obj_set_style_text_font(wifi_title_lbl, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(wifi_title_lbl, lv_color_hex(0x2563EB), 0);

        wifi_list_box = lv_obj_create(scr_wifi);
        lv_obj_set_size(wifi_list_box, UI_WIFI_LIST_W, UI_WIFI_LIST_H);
        lv_obj_align(wifi_list_box, LV_ALIGN_TOP_MID, 0, UI_WIFI_LIST_TOP_Y);
        lv_obj_set_style_bg_opa(wifi_list_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wifi_list_box, 0, 0);
        lv_obj_set_flex_flow(wifi_list_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(wifi_list_box, 6, 0);

        wifi_lbl_ssid = lv_label_create(scr_wifi);
        lv_obj_align(wifi_lbl_ssid, LV_ALIGN_TOP_MID, 0, UI_WIFI_LIST_TOP_Y + 4);
        lv_obj_set_style_text_font(wifi_lbl_ssid, &UI_FONT_BODY, 0);

        wifi_ta_pass = lv_textarea_create(scr_wifi);
        lv_textarea_set_one_line(wifi_ta_pass, true);
        lv_obj_set_width(wifi_ta_pass, BOARD_LCD_H_RES - 32);
        lv_obj_align(wifi_ta_pass, LV_ALIGN_TOP_MID, 0, UI_WIFI_LIST_TOP_Y + 36);
        lv_obj_set_style_bg_color(wifi_ta_pass, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(wifi_ta_pass, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(wifi_ta_pass, lv_color_hex(0x3B82F6), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(wifi_ta_pass, lv_color_hex(0xCBD5E1), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(wifi_ta_pass, 2, 0);
        lv_obj_set_style_text_font(wifi_ta_pass, &UI_FONT_BODY, 0);

        wifi_kb = lv_keyboard_create(scr_wifi);
        lv_keyboard_set_mode(wifi_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(wifi_kb, wifi_ta_pass);
        lv_keyboard_set_popovers(wifi_kb, true);
        lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(0xF1F5F9), 0);
        lv_obj_set_style_text_font(wifi_kb, LV_FONT_DEFAULT, 0);
        lv_obj_set_size(wifi_kb, BOARD_LCD_H_RES, UI_WIFI_KB_H);
        lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, -UI_WIFI_KB_BOTTOM_OFF);

        wifi_btn_back_bt = lv_btn_create(scr_wifi);
        lv_obj_set_size(wifi_btn_back_bt, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(wifi_btn_back_bt, LV_ALIGN_BOTTOM_LEFT, UI_SCR_PAD_X, -UI_BACK_BOTTOM_Y);
        lv_obj_t *lb = lv_label_create(wifi_btn_back_bt);
        lv_label_set_text(lb, "QUAY LAI");
        lv_obj_center(lb);
        lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(wifi_btn_back_bt, wifi_back_cb, LV_EVENT_CLICKED, NULL);

        wifi_btn_save_bt = lv_btn_create(scr_wifi);
        lv_obj_set_size(wifi_btn_save_bt, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(wifi_btn_save_bt, LV_ALIGN_BOTTOM_RIGHT, -UI_SCR_PAD_X, -UI_BACK_BOTTOM_Y);
        lb = lv_label_create(wifi_btn_save_bt);
        lv_label_set_text(lb, "KẾT NỐI");
        lv_obj_center(lb);
        lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(wifi_btn_save_bt, wifi_save_cb, LV_EVENT_CLICKED, NULL);

        wifi_btn_del_bt = lv_btn_create(scr_wifi);
        lv_obj_set_size(wifi_btn_del_bt, 72, UI_NAV_BTN_H);
        lv_obj_align(wifi_btn_del_bt, LV_ALIGN_BOTTOM_MID, 0, -UI_BACK_BOTTOM_Y);
        lv_obj_set_style_bg_color(wifi_btn_del_bt, lv_color_hex(0xDC2626), 0);
        lb = lv_label_create(wifi_btn_del_bt);
        lv_label_set_text(lb, "XÓA");
        lv_obj_center(lb);
        lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(wifi_btn_del_bt, wifi_del_btn_cb, LV_EVENT_CLICKED, NULL);
        
        lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, -UI_WIFI_KB_BOTTOM_OFF);
    }

    char title[40];
    if (state == 5) {
        snprintf(title, sizeof(title), "WIFI ĐÃ LƯU");
    } else if (state == 6) {
        snprintf(title, sizeof(title), "CHỌN WIFI");
    } else if (state == 13) {
        snprintf(title, sizeof(title), "CHI TIẾT WIFI");
    } else {
        snprintf(title, sizeof(title), "NHẬP MẬT KHẨU WIFI");
    }
    lv_label_set_text(wifi_title_lbl, title);

    bool show_list = (state == 5 || state == 6);
    bool show_ssid = (state == 7 || state == 13);
    bool show_pass = (state == 7);
    bool show_kb   = (state == 7);

    if (show_list) {
        lv_obj_clear_flag(wifi_list_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_lbl_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_btn_back_bt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_btn_save_bt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_btn_del_bt, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clean(wifi_list_box);
        int display_cnt = (state == 5) ? wifi_list_get_count() : g_wifi_scan_count;
        if (display_cnt > 5) {
            display_cnt = 5;
        }
        for (int i = 0; i < display_cnt; i++) {
            lv_obj_t *row = lv_btn_create(wifi_list_box);
            lv_obj_set_width(row, UI_WIFI_ROW_W);
            lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0xCBD5E1), 0);
            char ssid[40];
            if (state == 5) {
                wifi_list_get_item(i, ssid, NULL);
            } else {
                strncpy(ssid, g_wifi_scan_res[i], sizeof(ssid) - 1);
                ssid[sizeof(ssid) - 1] = '\0';
            }
            lv_obj_t *rl = lv_label_create(row);
            lv_label_set_text(rl, ssid);
            lv_obj_center(rl);
            lv_obj_set_style_text_font(rl, &UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(rl, lv_color_hex(0x0F172A), 0);
            lv_obj_add_event_cb(row, wifi_ssid_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        if (state == 5) {
            lv_obj_t *btn_scan = lv_btn_create(wifi_list_box);
            lv_obj_set_width(btn_scan, UI_WIFI_ROW_W);
            lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(btn_scan, 1, 0);
            lv_obj_set_style_border_color(btn_scan, lv_color_hex(0xCBD5E1), 0);
            lv_obj_t *ls = lv_label_create(btn_scan);
            lv_label_set_text(ls, "+ QUÉT THÊM WIFI");
            lv_obj_center(ls);
            lv_obj_set_style_text_font(ls, &UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(ls, lv_color_hex(0x2563EB), 0);
            lv_obj_add_event_cb(btn_scan, wifi_scan_more_cb, LV_EVENT_CLICKED, NULL);
        }
    } else if (show_ssid) {
        lv_obj_add_flag(wifi_list_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_lbl_ssid, LV_OBJ_FLAG_HIDDEN);
        
        if (show_pass) {
            lv_obj_clear_flag(wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wifi_ta_pass, LV_OBJ_FLAG_HIDDEN);
        }

        if (show_kb) {
            lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(wifi_btn_back_bt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_btn_save_bt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_btn_del_bt, LV_OBJ_FLAG_HIDDEN);

        if (!show_kb) {
            lv_obj_align(wifi_lbl_ssid, LV_ALIGN_CENTER, 0, -28);
        } else {
            lv_obj_align(wifi_lbl_ssid, LV_ALIGN_TOP_MID, 0, UI_WIFI_LIST_TOP_Y + 4);
            lv_obj_set_width(wifi_ta_pass, BOARD_LCD_H_RES - 32);
            lv_obj_align(wifi_ta_pass, LV_ALIGN_TOP_MID, 0, UI_WIFI_LIST_TOP_Y + 36);
        }

        char hdr[48];
        snprintf(hdr, sizeof(hdr), "%s", selected_ssid ? selected_ssid : "");
        lv_label_set_text(wifi_lbl_ssid, hdr);
        lv_textarea_set_text(wifi_ta_pass, entered_pass ? entered_pass : "");
        lv_keyboard_set_textarea(wifi_kb, wifi_ta_pass);
    }

    wifi_last_state = state;
    lv_scr_load(scr_wifi);
}

/* ------------- Log ------------- */
static void log_nav_cb(lv_event_t *e)
{
    intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    if (dir == 0) {
        if (g_log_page > 0) {
            g_log_page--;
        }
    } else {
        g_log_page++;
    }
    lv_port_show_log_screen(g_log_page);
    ui_refresh();
}

static void log_back_cb(lv_event_t *e)
{
    (void)e;
    g_ui_state = 2;
    lv_port_show_menu_screen(1);
    ui_refresh();
}

void lv_port_show_log_screen(int page)
{
    ui_log_row_t rows[UI_LOG_ROWS_PER_PAGE];
    int n = ui_log_model_load_page(page, rows, UI_LOG_ROWS_PER_PAGE);

    if (!scr_log) {
        scr_log = lv_obj_create(NULL);
        style_scr_dark(scr_log);

        lbl_log_title = lv_label_create(scr_log);
        lv_obj_align(lbl_log_title, LV_ALIGN_TOP_MID, 0, UI_HDR_TOP_Y);
        lv_obj_set_style_text_font(lbl_log_title, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl_log_title, lv_color_hex(0x2563EB), 0);

        for (int i = 0; i < UI_LOG_ROWS_PER_PAGE; i++) {
            lv_obj_t *box = lv_btn_create(scr_log);
            lv_obj_set_size(box, UI_LIST_ROW_W, UI_LIST_ROW_H);
            lv_obj_align(box, LV_ALIGN_TOP_LEFT, UI_SCR_PAD_X, ui_list_row_y(i));
            lv_obj_set_style_bg_color(box, lv_color_hex(i % 2 ? 0xFFFFFF : 0xF1F5F9), 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0xCBD5E1), 0);
            lv_obj_set_style_radius(box, 10, 0);
            lv_obj_set_style_shadow_opa(box, 0, 0); // Tắt bóng đổ nút cho giống danh sách
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

            log_row_lbl[i] = lv_label_create(box);
            lv_label_set_long_mode(log_row_lbl[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_width(log_row_lbl[i], UI_LIST_ROW_W - 12);
            lv_obj_align(log_row_lbl[i], LV_ALIGN_TOP_LEFT, 4, 2);
            lv_obj_set_style_text_font(log_row_lbl[i], &UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(log_row_lbl[i], lv_color_hex(0x2563EB), 0);

            log_row_sub_lbl[i] = lv_label_create(box);
            lv_label_set_long_mode(log_row_sub_lbl[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_width(log_row_sub_lbl[i], UI_LIST_ROW_W - 12);
            lv_obj_align(log_row_sub_lbl[i], LV_ALIGN_BOTTOM_LEFT, 4, -2);
            lv_obj_set_style_text_font(log_row_sub_lbl[i], &UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(log_row_sub_lbl[i], lv_color_hex(0x64748B), 0);
        }

        lv_obj_t *bprev = lv_btn_create(scr_log);
        lv_obj_set_size(bprev, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(bprev, LV_ALIGN_BOTTOM_LEFT, UI_SCR_PAD_X, -UI_NAV_BOTTOM_Y);
        lv_obj_t *lp = lv_label_create(bprev);
        lv_label_set_text(lp, "TRƯỚC");
        lv_obj_center(lp);
        lv_obj_set_style_text_font(lp, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(bprev, log_nav_cb, LV_EVENT_CLICKED, (void *)0);

        lv_obj_t *bnext = lv_btn_create(scr_log);
        lv_obj_set_size(bnext, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(bnext, LV_ALIGN_BOTTOM_RIGHT, -UI_SCR_PAD_X, -UI_NAV_BOTTOM_Y);
        lv_obj_t *ln = lv_label_create(bnext);
        lv_label_set_text(ln, "SAU >");
        lv_obj_center(ln);
        lv_obj_set_style_text_font(ln, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(bnext, log_nav_cb, LV_EVENT_CLICKED, (void *)1);

        lv_obj_t *bback = lv_btn_create(scr_log);
        lv_obj_set_size(bback, UI_BACK_BTN_W, UI_BACK_BTN_H);
        lv_obj_align(bback, LV_ALIGN_BOTTOM_MID, 0, -UI_BACK_BOTTOM_Y);
        lv_obj_t *lb = lv_label_create(bback);
        lv_label_set_text(lb, "QUAY LẠI");
        lv_obj_center(lb);
        lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(bback, log_back_cb, LV_EVENT_CLICKED, NULL);
    }

    char tbuf[48];
    snprintf(tbuf, sizeof(tbuf), "NHẬT KÝ - Trang %d", page + 1);
    lv_label_set_text(lbl_log_title, tbuf);

    for (int i = 0; i < UI_LOG_ROWS_PER_PAGE; i++) {
        if (i < n) {
            char info[96];
            snprintf(info, sizeof(info), "%s %s | ID:%s", rows[i].date, rows[i].time, rows[i].id);
            
            lv_label_set_text(log_row_lbl[i], rows[i].name[0] ? rows[i].name : "THẺ LẠ");
            lv_label_set_text(log_row_sub_lbl[i], info);
            lv_obj_clear_flag(lv_obj_get_parent(log_row_lbl[i]), LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lv_obj_get_parent(log_row_lbl[i]), LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_scr_load(scr_log);
}

/* ------------- Card list ------------- */
static CardProfileEntry_t s_row_entries[CARD_PROFILE_PAGE_ROWS];

static void card_row_cb(lv_event_t *e)
{
    int ri = (int)(intptr_t)lv_event_get_user_data(e);
    CardProfileEntry_t *ent = &s_row_entries[ri];
    strncpy(g_edit_uid, ent->uid, sizeof(g_edit_uid) - 1);
    strncpy(g_edit_name, ent->name, sizeof(g_edit_name) - 1);
    strncpy(g_edit_id, ent->id, sizeof(g_edit_id) - 1);
    g_edit_field = 0;
    g_edit_from_state = g_ui_state;
    g_soft_kb_upper = false;
    g_soft_kb_sym_page = 0;
    g_ui_state = 11;
    lv_port_show_card_edit(g_edit_uid, g_edit_name, g_edit_id, g_edit_field);
    ui_refresh();
}

static void card_del_cb(lv_event_t *e)
{
    int ri = (int)(intptr_t)lv_event_get_user_data(e);
    CardProfileEntry_t *ent = &s_row_entries[ri];
    g_confirm_type = 1;
    g_confirm_from_state = g_ui_state;
    strncpy(g_confirm_arg_str, ent->uid, sizeof(g_confirm_arg_str) - 1);
    g_confirm_arg_str[sizeof(g_confirm_arg_str) - 1] = '\0';
    strncpy(g_confirm_arg_str2, ent->name, sizeof(g_confirm_arg_str2) - 1);
    g_confirm_arg_str2[sizeof(g_confirm_arg_str2) - 1] = '\0';
    g_ui_state = 12;
    lv_port_show_confirm_screen(1, g_confirm_arg_str, g_confirm_arg_str2);
    ui_refresh();
}

static void card_nav_cb(lv_event_t *e)
{
    intptr_t dir = (intptr_t)lv_event_get_user_data(e);
    int total = ui_card_cache_total(g_ui_state == 10);
    int maxp = (total <= 0) ? 0 : (total - 1) / CARD_PROFILE_PAGE_ROWS;
    if (dir == 0) {
        if (g_card_page > 0) {
            g_card_page--;
        }
    } else {
        if (g_card_page < maxp) {
            g_card_page++;
        }
    }
    lv_port_show_card_list(g_card_page, g_ui_state == 10);
    ui_refresh();
}

static void card_back_cb(lv_event_t *e)
{
    (void)e;
    g_ui_state = 2;
    lv_port_show_menu_screen(2);
    ui_refresh();
}

void lv_port_show_card_list(int page, bool only_unregistered)
{
    int pg = page;
    int cnt = ui_card_cache_load_page(&pg, only_unregistered, s_row_entries, CARD_PROFILE_PAGE_ROWS);
    (void)cnt;

    if (!scr_cards) {
        scr_cards = lv_obj_create(NULL);
        style_scr_dark(scr_cards);

        lbl_cards_hdr = lv_label_create(scr_cards);
        lv_obj_align(lbl_cards_hdr, LV_ALIGN_TOP_MID, 0, UI_HDR_TOP_Y);
        lv_obj_set_style_text_font(lbl_cards_hdr, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(lbl_cards_hdr, lv_color_hex(0x2563EB), 0);

        for (int i = 0; i < CARD_PROFILE_PAGE_ROWS; i++) {
            card_row_btns[i] = lv_btn_create(scr_cards);
            lv_obj_set_size(card_row_btns[i], UI_LIST_ROW_W - UI_LIST_DEL_W - 4, UI_LIST_ROW_H);
            lv_obj_align(card_row_btns[i], LV_ALIGN_TOP_LEFT, UI_SCR_PAD_X, ui_list_row_y(i));
            lv_obj_set_style_bg_color(card_row_btns[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(card_row_btns[i], 1, 0);
            lv_obj_set_style_border_color(card_row_btns[i], lv_color_hex(0xCBD5E1), 0);
            
            lv_obj_t *nm = lv_label_create(card_row_btns[i]);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_width(nm, UI_LIST_ROW_W - UI_LIST_DEL_W - 16);
            lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 4, 2);
            lv_obj_set_style_text_font(nm, &UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(nm, lv_color_hex(0x0F172A), 0);

            lv_obj_t *sub = lv_label_create(card_row_btns[i]);
            lv_label_set_long_mode(sub, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_width(sub, UI_LIST_ROW_W - UI_LIST_DEL_W - 16);
            lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 4, -2);
            lv_obj_set_style_text_font(sub, &UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(sub, lv_color_hex(0x64748B), 0); // Màu xám nhạt hơn cho dễ đọc
            lv_obj_add_event_cb(card_row_btns[i], card_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            card_del_btns[i] = lv_btn_create(scr_cards);
            lv_obj_set_size(card_del_btns[i], UI_LIST_DEL_W, UI_LIST_ROW_H);
            lv_obj_align(card_del_btns[i], LV_ALIGN_TOP_RIGHT, -UI_SCR_PAD_X, ui_list_row_y(i));
            lv_obj_set_style_bg_color(card_del_btns[i], lv_color_hex(0xDC2626), 0);
            lv_obj_t *dx = lv_label_create(card_del_btns[i]);
            lv_label_set_text(dx, "X");
            lv_obj_center(dx);
            lv_obj_add_event_cb(card_del_btns[i], card_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        btn_card_prev = lv_btn_create(scr_cards);
        lv_obj_set_size(btn_card_prev, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(btn_card_prev, LV_ALIGN_BOTTOM_LEFT, UI_SCR_PAD_X, -UI_NAV_BOTTOM_Y);
        lv_obj_t *lp = lv_label_create(btn_card_prev);
        lv_label_set_text(lp, "TRƯỚC");
        lv_obj_center(lp);
        lv_obj_set_style_text_font(lp, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(btn_card_prev, card_nav_cb, LV_EVENT_CLICKED, (void *)0);

        btn_card_next = lv_btn_create(scr_cards);
        lv_obj_set_size(btn_card_next, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(btn_card_next, LV_ALIGN_BOTTOM_RIGHT, -UI_SCR_PAD_X, -UI_NAV_BOTTOM_Y);
        lv_obj_t *ln = lv_label_create(btn_card_next);
        lv_label_set_text(ln, "SAU");
        lv_obj_center(ln);
        lv_obj_set_style_text_font(ln, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(btn_card_next, card_nav_cb, LV_EVENT_CLICKED, (void *)1);

        lv_obj_t *bback = lv_btn_create(scr_cards);
        lv_obj_set_size(bback, UI_BACK_BTN_W, UI_BACK_BTN_H);
        lv_obj_align(bback, LV_ALIGN_BOTTOM_MID, 0, -UI_BACK_BOTTOM_Y);
        lv_obj_t *lb = lv_label_create(bback);
        lv_label_set_text(lb, "QUAY LẠI");
        lv_obj_center(lb);
        lv_obj_set_style_text_font(lb, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(bback, card_back_cb, LV_EVENT_CLICKED, NULL);
    }

    char ht[56];
    snprintf(ht, sizeof(ht), "%s (tr.%d)", only_unregistered ? "THẺ LẠ" : "DANH SÁCH THẺ", g_card_page + 1);
    lv_label_set_text(lbl_cards_hdr, ht);

    for (int i = 0; i < CARD_PROFILE_PAGE_ROWS; i++) {
        lv_obj_t *nm = lv_obj_get_child(card_row_btns[i], 0);
        lv_obj_t *sub = lv_obj_get_child(card_row_btns[i], 1);
        if (i < cnt) {
            char line_sub[96];
            snprintf(line_sub, sizeof(line_sub), "ID:%s | UID:%s",
                     s_row_entries[i].id[0] ? s_row_entries[i].id : "---",
                     s_row_entries[i].uid);
            lv_label_set_text(nm, s_row_entries[i].name[0] ? s_row_entries[i].name : "(Chưa đặt tên)");
            lv_obj_set_style_text_font(nm, &UI_FONT_BODY, 0);
            lv_label_set_text(sub, line_sub);
            lv_obj_set_style_text_font(sub, &UI_FONT_BODY, 0);
            lv_obj_clear_flag(card_row_btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(card_del_btns[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(card_row_btns[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(card_del_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_scr_load(scr_cards);
}

/* ------------- Card edit ------------- */
static void sync_edit_from_ta(void)
{
    strncpy(g_edit_name, lv_textarea_get_text(ta_edit_name), sizeof(g_edit_name) - 1);
    g_edit_name[sizeof(g_edit_name) - 1] = '\0';
    strncpy(g_edit_id, lv_textarea_get_text(ta_edit_id), sizeof(g_edit_id) - 1);
    g_edit_id[sizeof(g_edit_id) - 1] = '\0';
}

static void edit_save_cb(lv_event_t *e)
{
    (void)e;
    sync_edit_from_ta();
    g_confirm_type = 2;
    g_confirm_from_state = g_ui_state; /* 11 = form sua the — HUY quay lai dung man */
    g_ui_state = 12;
    lv_port_show_confirm_screen(2, NULL, NULL);
    ui_refresh();
}

static void edit_field_cb(lv_event_t *e)
{
    intptr_t f = (intptr_t)lv_event_get_user_data(e);
    g_edit_field = (int)f;
    lv_keyboard_set_textarea(kb_edit, g_edit_field == 0 ? ta_edit_name : ta_edit_id);
}

static void edit_cancel_cb(lv_event_t *e)
{
    (void)e;
    sync_edit_from_ta();
    g_ui_state = g_edit_from_state;
    lv_port_show_card_list(g_card_page, g_ui_state == 10);
    ui_refresh();
}

void lv_port_show_card_edit(const char *uid, const char *name, const char *id, int active_field)
{
    if (!scr_edit) {
        scr_edit = lv_obj_create(NULL);
        style_scr_dark(scr_edit);

        lv_obj_t *hdr = lv_label_create(scr_edit);
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, UI_HDR_TOP_Y);
        lv_obj_set_style_text_font(hdr, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x2563EB), 0);

        ta_edit_name = lv_textarea_create(scr_edit);
        lv_textarea_set_one_line(ta_edit_name, true);
        lv_obj_set_width(ta_edit_name, BOARD_LCD_H_RES - 32);
        lv_obj_align(ta_edit_name, LV_ALIGN_TOP_MID, 0, 32);
        lv_obj_set_style_bg_color(ta_edit_name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(ta_edit_name, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(ta_edit_name, lv_color_hex(0x3B82F6), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_edit_name, lv_color_hex(0xCBD5E1), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ta_edit_name, 2, 0);
        lv_obj_set_style_text_font(ta_edit_name, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(ta_edit_name, edit_field_cb, LV_EVENT_FOCUSED, (void *)0);

        ta_edit_id = lv_textarea_create(scr_edit);
        lv_textarea_set_one_line(ta_edit_id, true);
        lv_obj_set_width(ta_edit_id, BOARD_LCD_H_RES - 32);
        lv_obj_align(ta_edit_id, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_set_style_bg_color(ta_edit_id, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_color(ta_edit_id, lv_color_hex(0x0F172A), 0);
        lv_obj_set_style_border_color(ta_edit_id, lv_color_hex(0x3B82F6), LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_edit_id, lv_color_hex(0xCBD5E1), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ta_edit_id, 2, 0);
        lv_obj_set_style_text_font(ta_edit_id, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(ta_edit_id, edit_field_cb, LV_EVENT_FOCUSED, (void *)1);

        kb_edit = lv_keyboard_create(scr_edit);
        lv_keyboard_set_mode(kb_edit, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_popovers(kb_edit, true);
        lv_obj_set_style_bg_color(kb_edit, lv_color_hex(0xF1F5F9), 0);
        lv_obj_set_style_text_font(kb_edit, LV_FONT_DEFAULT, 0);
        lv_obj_set_size(kb_edit, BOARD_LCD_H_RES, UI_EDIT_KB_H);
        lv_obj_align(kb_edit, LV_ALIGN_BOTTOM_MID, 0, -UI_EDIT_KB_BOTTOM_OFF);

        lv_obj_t *bc = lv_btn_create(scr_edit);
        lv_obj_set_size(bc, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(bc, LV_ALIGN_BOTTOM_LEFT, UI_SCR_PAD_X, -UI_BACK_BOTTOM_Y);
        lv_obj_t *lc = lv_label_create(bc);
        lv_label_set_text(lc, "HỦY");
        lv_obj_center(lc);
        lv_obj_set_style_text_font(lc, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(bc, edit_cancel_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *bs = lv_btn_create(scr_edit);
        lv_obj_set_size(bs, UI_NAV_BTN_W, UI_NAV_BTN_H);
        lv_obj_align(bs, LV_ALIGN_BOTTOM_RIGHT, -UI_SCR_PAD_X, -UI_BACK_BOTTOM_Y);
        lv_obj_t *ls = lv_label_create(bs);
        lv_label_set_text(ls, "LƯU");
        lv_obj_center(ls);
        lv_obj_set_style_text_font(ls, &UI_FONT_BODY, 0);
        lv_obj_add_event_cb(bs, edit_save_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *hdr = lv_obj_get_child(scr_edit, 0);
    char ht[48];
    snprintf(ht, sizeof(ht), "THẺ: %.16s", uid ? uid : "");
    lv_label_set_text(hdr, ht);

    lv_textarea_set_text(ta_edit_name, name ? name : "");
    lv_textarea_set_text(ta_edit_id, id ? id : "");
    lv_keyboard_set_textarea(kb_edit, active_field == 0 ? ta_edit_name : ta_edit_id);

    lv_scr_load(scr_edit);
}

/* ------------- Confirm ------------- */
void lv_port_show_confirm_screen(int type, const char *arg1, const char *arg2)
{
    if (confirm_box) {
        lv_obj_del(confirm_box);
        confirm_box = NULL;
    }

    confirm_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(confirm_box, UI_CONFIRM_W, UI_CONFIRM_H);
    lv_obj_center(confirm_box);
    lv_obj_set_style_bg_color(confirm_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(confirm_box, 2, 0);
    lv_obj_set_style_border_color(confirm_box, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_radius(confirm_box, 12, 0);

    const char *title = "XÁC NHẬN";
    const char *msg1 = "";
    lv_color_t hdr_c = lv_color_hex(0x2563EB);
    if (type == 1) {
        title = "XÓA THẺ";
        msg1 = "Bạn chắc chắn xóa thẻ này?";
        hdr_c = lv_color_hex(0xDC2626);
    } else if (type == 2) {
        title = "LƯU THẺ";
        msg1 = "Bạn chắc chắn lưu thẻ?";
    } else if (type == 3) {
        title = "XÓA WIFI";
        msg1 = "Bạn chắc chắn xóa mạng?";
        hdr_c = lv_color_hex(0xDC2626);
    } else if (type == 4) {
        title = "LƯU WIFI";
        msg1 = "Bạn chắc chắn lưu mạng?";
    } else if (type == 5) {
        title = "ĐỔI MẬT KHẨU";
        msg1 = "Xác nhận đổi mật khẩu?";
    }

    lv_obj_t *ht = lv_label_create(confirm_box);
    lv_label_set_text(ht, title);
    lv_obj_align(ht, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_color(ht, hdr_c, 0);
    lv_obj_set_style_text_font(ht, &UI_FONT_BODY, 0);

    lv_obj_t *m = lv_label_create(confirm_box);
    lv_label_set_text(m, msg1);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_text_font(m, &UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(m, lv_color_hex(0x0F172A), 0);

    if (arg1 && arg1[0]) {
        lv_obj_t *m2 = lv_label_create(confirm_box);
        lv_label_set_text(m2, arg1);
        lv_obj_align(m2, LV_ALIGN_TOP_MID, 0, 90);
        lv_obj_set_style_text_font(m2, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(m2, lv_color_hex(0x475569), 0);
    }
    if (type == 1 && arg2 && arg2[0]) {
        lv_obj_t *m3 = lv_label_create(confirm_box);
        lv_label_set_text(m3, arg2);
        lv_obj_align(m3, LV_ALIGN_TOP_MID, 0, 100);
        lv_obj_set_style_text_font(m3, &UI_FONT_BODY, 0);
        lv_obj_set_style_text_color(m3, lv_color_hex(0x64748B), 0);
    }

    lv_obj_t *bn = lv_btn_create(confirm_box);
    lv_obj_set_size(bn, 110, 48);
    lv_obj_align(bn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_t *ln = lv_label_create(bn);
    lv_label_set_text(ln, "HỦY");
    lv_obj_center(ln);
    lv_obj_set_style_text_font(ln, &UI_FONT_BODY, 0);
    lv_obj_add_event_cb(bn, confirm_del_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *by = lv_btn_create(confirm_box);
    lv_obj_set_size(by, 120, 40);
    lv_obj_align(by, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_t *ly = lv_label_create(by);
    lv_label_set_text(ly, "ĐỒNG Ý");
    lv_obj_center(ly);
    lv_obj_set_style_text_font(ly, &UI_FONT_BODY, 0);
    lv_obj_add_event_cb(by, confirm_yes_cb, LV_EVENT_CLICKED, NULL);
}
