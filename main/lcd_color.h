#pragma once

/**
 * Lớp màu thống nhất: mọi pixel tới LCD đều là RGB565 little-endian (chuẩn R high 5 bit),
 * rồi `lcd_color_to_bus` nếu cần đổi endian SPI. Không swap kênh R/B/G ở đây — chỉ
 * trong driver SPI666 (theo BOARD_LCD_COLOR_PIPELINE_BGR) hoặc BOARD_SD_IMAGE_SWAP_RB
 * khi *file JPEG* sai thứ tự kênh.
 */
#include <stdint.h>

#include "board_pins.h"

static inline uint16_t lcd_color_from888(uint8_t r, uint8_t g, uint8_t b)
{
    return BOARD_RGB565_FROM888(r, g, b);
}

static inline uint16_t lcd_color_to_bus(uint16_t rgb565_le)
{
#if BOARD_LCD_RGB565_SWAP_BYTES
    return __builtin_bswap16(rgb565_le);
#else
    return rgb565_le;
#endif
}

/**
 * Pixel bus cho viền letterbox JPEG: BOARD_SD_IMAGE_LETTERBOX_* + to_bus — giống nền UI, không SWAP_RB ảnh.
 */
static inline uint16_t lcd_color_letterbox_bus(void)
{
    return lcd_color_to_bus(
        lcd_color_from888(BOARD_SD_IMAGE_LETTERBOX_R, BOARD_SD_IMAGE_LETTERBOX_G, BOARD_SD_IMAGE_LETTERBOX_B));
}
