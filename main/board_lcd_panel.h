#pragma once

/**
 * Chọn loại màn LCD (cùng bus SPI2: GPIO 39/40/9/10/8).
 *
 * Cách 1 — menuconfig (khuyên dùng):
 *   idf.py menuconfig  →  "Man hinh LCD"  →  chọn profile  →  build flash
 *
 * Cách 2 — sửa tay (trước khi build), bỏ qua menuconfig:
 *   #define BOARD_LCD_PANEL_ID BOARD_LCD_PANEL_GMT028_28
 *
 * Lưu ý: Không tự nhận màn khi cắm — phải chọn đúng profile rồi flash lại.
 * Hai màn 2.8" dùng chung dây; chỉ khác init / mirror / tốc độ SPI.
 */

#include "sdkconfig.h"

#define BOARD_LCD_PANEL_GMT028_28        1
#define BOARD_LCD_PANEL_ILI9341_LEGACY_28 2

#ifndef BOARD_LCD_PANEL_ID
#if defined(CONFIG_LCD_PANEL_GMT028_28)
#define BOARD_LCD_PANEL_ID BOARD_LCD_PANEL_GMT028_28
#else
#define BOARD_LCD_PANEL_ID BOARD_LCD_PANEL_ILI9341_LEGACY_28
#endif
#endif

#if BOARD_LCD_PANEL_ID == BOARD_LCD_PANEL_GMT028_28
#define BOARD_LCD_PANEL_NAME "GMT028-2.8-ILI9341"
#elif BOARD_LCD_PANEL_ID == BOARD_LCD_PANEL_ILI9341_LEGACY_28
#define BOARD_LCD_PANEL_NAME "ILI9341-2.8-legacy"
#else
#error "BOARD_LCD_PANEL_ID khong hop le (1=GMT028, 2=legacy)"
#endif
