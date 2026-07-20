#include "lcd_panel_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "board_lcd_panel.h"

static const char *TAG = "lcd_panel";

#define NVS_NS "device_cfg"
#define NVS_KEY_LCD_PANEL "lcd_panel"

static const lcd_panel_profile_t s_profiles[] = {
    {
        .id = BOARD_LCD_PANEL_GMT028_28,
        .name = "GMT028 2.8 inch ILI9341 IPS",
        .use_vendor_init = true,
        .mirror_x = false,
        .mirror_y = false,
        .invert_color = true,
        .pclk_hz = 16 * 1000 * 1000,
    },
    {
        .id = BOARD_LCD_PANEL_ILI9341_LEGACY_28,
        .name = "ILI9341 2.8 inch (cau hinh cu)",
        .use_vendor_init = false,
        .mirror_x = true,
        .mirror_y = true,
        .invert_color = false,
        .pclk_hz = 26 * 1000 * 1000,
    },
};

static uint8_t s_active_id;

static const lcd_panel_profile_t *profile_by_id(uint8_t id)
{
    for (size_t i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (s_profiles[i].id == id) {
            return &s_profiles[i];
        }
    }
    return NULL;
}

static uint8_t default_panel_id(void)
{
#if BOARD_LCD_PANEL_ID == BOARD_LCD_PANEL_GMT028_28
    return BOARD_LCD_PANEL_GMT028_28;
#elif BOARD_LCD_PANEL_ID == BOARD_LCD_PANEL_ILI9341_LEGACY_28
    return BOARD_LCD_PANEL_ILI9341_LEGACY_28;
#else
    return BOARD_LCD_PANEL_GMT028_28;
#endif
}

esp_err_t lcd_panel_config_init(void)
{
    s_active_id = default_panel_id();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS chua san sang — dung profile mac dinh build (%s)", BOARD_LCD_PANEL_NAME);
        return ESP_OK;
    }

    uint8_t v = 0;
    if (nvs_get_u8(h, NVS_KEY_LCD_PANEL, &v) == ESP_OK && profile_by_id(v) != NULL) {
        s_active_id = v;
    }
    nvs_close(h);

    const lcd_panel_profile_t *p = profile_by_id(s_active_id);
    ESP_LOGI(TAG, "Profile LCD: %s (id=%u, pclk=%u MHz, mirror=%d,%d, invert=%d, vendor=%d)",
             p ? p->name : "?", (unsigned)s_active_id, p ? (unsigned)(p->pclk_hz / 1000000u) : 0u,
             p ? p->mirror_x : 0, p ? p->mirror_y : 0, p ? p->invert_color : 0, p ? p->use_vendor_init : 0);
    return ESP_OK;
}

const lcd_panel_profile_t *lcd_panel_get_active(void)
{
    const lcd_panel_profile_t *p = profile_by_id(s_active_id);
    return p ? p : &s_profiles[0];
}

uint8_t lcd_panel_get_id(void)
{
    return s_active_id;
}

esp_err_t lcd_panel_set_id(uint8_t panel_id)
{
    if (profile_by_id(panel_id) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_LCD_PANEL, panel_id);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_active_id = panel_id;
        ESP_LOGI(TAG, "Da luu profile LCD id=%u (%s)", (unsigned)panel_id, profile_by_id(panel_id)->name);
    }
    return err;
}

int lcd_panel_profile_count(void)
{
    return (int)(sizeof(s_profiles) / sizeof(s_profiles[0]));
}

const lcd_panel_profile_t *lcd_panel_get_by_index(int index)
{
    if (index < 0 || index >= lcd_panel_profile_count()) {
        return NULL;
    }
    return &s_profiles[index];
}
