#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Cấu hình driver LCD đang dùng (đọc NVS lúc boot, hoặc mặc định từ menuconfig). */
typedef struct {
    uint8_t id;
    const char *name;
    bool use_vendor_init;
    bool mirror_x;
    bool mirror_y;
    bool invert_color;
    uint32_t pclk_hz;
} lcd_panel_profile_t;

/** Gọi sau nvs_flash_init(), trước lcd_ui_init(). */
esp_err_t lcd_panel_config_init(void);

const lcd_panel_profile_t *lcd_panel_get_active(void);

uint8_t lcd_panel_get_id(void);

/** Lưu NVS; áp dụng sau esp_restart(). */
esp_err_t lcd_panel_set_id(uint8_t panel_id);

int lcd_panel_profile_count(void);
const lcd_panel_profile_t *lcd_panel_get_by_index(int index);
