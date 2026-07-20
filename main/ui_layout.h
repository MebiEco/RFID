#pragma once

/**
 * Kích thước / lề UI theo độ phân giải màn (BOARD_LCD_H_RES × BOARD_LCD_V_RES).
 * Giữ nguyên mọi màn hình và widget — chỉ co giãn cho vừa 320×240.
 */
#include "board_pins.h"

/** 320×240 — TFT 2.8" ILI9341. */
#define UI_SCR_PAD_X           8
#define UI_HDR_TOP_Y           6
#define UI_FONT_HDR            arial_vn_24
#define UI_FONT_BODY           arial_vn_24

#define UI_LIST_ROW_H          32
#define UI_LIST_ROW_GAP        2
#define UI_LIST_TOP_Y          22
#define UI_LIST_ROW_W          (BOARD_LCD_H_RES - 16)
#define UI_LIST_DEL_W          38
#define UI_NAV_BTN_H           28
#define UI_NAV_BTN_W           120
#define UI_BACK_BTN_H          32
#define UI_BACK_BTN_W          (BOARD_LCD_H_RES - 24)
#define UI_NAV_BOTTOM_Y        34
#define UI_BACK_BOTTOM_Y       4

#define UI_MENU_BTN_W          146
#define UI_MENU_BTN_H          50
#define UI_MENU_GRID_Y         26
#define UI_MENU_COL0_X         10
#define UI_MENU_COL1_X         (UI_MENU_COL0_X + UI_MENU_BTN_W + 8)
#define UI_MENU_ROW_GAP        6
#define UI_MENU_BACK_H         36
#define UI_MENU_BACK_W         (BOARD_LCD_H_RES - 28)

#define UI_SETTINGS_BTN_W      (BOARD_LCD_H_RES - 28)
#define UI_SETTINGS_BTN_H      44
#define UI_SETTINGS_BTN_Y0     52
#define UI_SETTINGS_BTN_STEP   50

#define UI_WIFI_LIST_W         (BOARD_LCD_H_RES - 10)
#define UI_WIFI_LIST_H         (BOARD_LCD_V_RES - 100)
#define UI_WIFI_LIST_TOP_Y     32
#define UI_WIFI_KB_H           120
#define UI_WIFI_KB_BOTTOM_OFF  40
#define UI_WIFI_ROW_W          (BOARD_LCD_H_RES - 32)

#define UI_PWD_KB_H            118
#define UI_EDIT_KB_H           112
#define UI_EDIT_KB_BOTTOM_OFF  40

#define UI_CONFIRM_W           (BOARD_LCD_H_RES - 16)
#define UI_CONFIRM_H           (BOARD_LCD_V_RES - 28)

/** Popup quẹt thẻ: vừa nội dung, ảnh trái — chữ phải (320×240). */
#define UI_SWIPE_POPUP_M       10
#define UI_SWIPE_POPUP_W       (BOARD_LCD_H_RES - 2 * UI_SWIPE_POPUP_M)
#define UI_SWIPE_POPUP_H       198
#define UI_SWIPE_IMG_W         66
#define UI_SWIPE_IMG_H         82
#define UI_SWIPE_NOIMG_H       UI_SWIPE_POPUP_H

/** Vị trí Y hàng danh sách (log / thẻ) theo chỉ số hàng. */
static inline int ui_list_row_y(int row_index)
{
    return UI_LIST_TOP_Y + row_index * (UI_LIST_ROW_H + UI_LIST_ROW_GAP);
}
