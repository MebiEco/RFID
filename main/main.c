#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "board_pins.h"
#if BOARD_ENABLE_RFID
#include "app_rfid.h"
#endif
#if BOARD_ENABLE_AZURE
#include "app_azure.h"
#endif
#if BOARD_ENABLE_AUDIO
#include "app_audio.h"
#endif
#if BOARD_ENABLE_LCD
#include "lcd_ui.h"
#include "lcd_panel_config.h"
#include "lv_port.h"
#endif
#if BOARD_ENABLE_RFID
#include "mfrc522.h"
#endif
#if BOARD_ENABLE_RFID && BOARD_RC522_SHARE_SD_SPI_BUS
#include "driver/spi_master.h"
#endif
#if BOARD_RC522_SHARE_SD_SPI_BUS || BOARD_ENABLE_SD
#include "sd_card.h"
#endif
#if BOARD_ENABLE_SD
#include "scan_log.h"
#endif
#if BOARD_ENABLE_WIFI
#include "wifi_portal.h"
#endif

static const char *TAG = "main";

void app_main(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rrs = "khac";
    switch (rr) {
    case ESP_RST_POWERON: rrs = "BAT_NGUON"; break;
    case ESP_RST_SW: rrs = "PHAN_MEM(Monitor/ota/esp_restart...)"; break;
    case ESP_RST_PANIC: rrs = "PANIC/CRASH"; break;
    case ESP_RST_INT_WDT: rrs = "INT_WDT"; break;
    case ESP_RST_TASK_WDT: rrs = "TASK_WDT"; break;
    case ESP_RST_WDT: rrs = "WDT"; break;
    case ESP_RST_BROWNOUT: rrs = "BROWNOUT(nguon_yeu)"; break;
    case ESP_RST_USB: rrs = "USB"; break;
    case ESP_RST_DEEPSLEEP: rrs = "DEEPSLEEP"; break;
    default: break;
    }
    ESP_LOGW(TAG, "Reset reason: %s (%d)", rrs, (int)rr);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { 
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if BOARD_ENABLE_LCD
    ESP_ERROR_CHECK(lcd_panel_config_init());
#endif

#if BOARD_ENABLE_WIFI
    /* Bớt log dư (ADDBA, wpa, phy...) — chỉ còn ERROR+ cho các tag ồn hệ thống */
    // esp_log_level_set("wifi", ESP_LOG_ERROR);
    // esp_log_level_set("wpa", ESP_LOG_ERROR);
    // esp_log_level_set("wifi_init", ESP_LOG_ERROR);
    // esp_log_level_set("esp_netif", ESP_LOG_WARN);
    // esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    // esp_log_level_set("phy", ESP_LOG_ERROR);
    // esp_log_level_set("pp", ESP_LOG_WARN);
#endif

#if BOARD_ENABLE_LCD
    ret = lcd_ui_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD: %s", esp_err_to_name(ret));
        return;
    }
    
    // Tạm thời ẩn giao diện cũ
    // vTaskDelay(pdMS_TO_TICKS(500));

    // Khởi tạo LVGL và truyền panel vào
    lv_port_init(lcd_ui_get_panel());
#endif

    /**
     * Mount SD sớm (trước WiFi/RC522): tránh phy WiFi + portal làm sụt nguồn / nhiễu lúc CMD8.
     * Nếu không bật LCD, vẫn mount ở đây (sau NVS) — khỏi đợi 2s.
     */
#if BOARD_ENABLE_SD
    ret = sd_card_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD: %s", esp_err_to_name(ret));
#if BOARD_ENABLE_LCD
        lcd_ui_show_centered("Loi File");
#endif
    } else {
        ESP_LOGI(TAG, "SD mount OK — %s", BOARD_SD_MOUNT_POINT);
        scan_log_flush_pending();
        scan_log_trim_at_boot();
    }
#endif

#if BOARD_ENABLE_WIFI
    ret = wifi_portal_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi portal: %s", esp_err_to_name(ret));
    }
#endif

#if BOARD_ENABLE_RFID
    if (RC522_IRQ_GPIO != GPIO_NUM_NC) 
    {
        gpio_config_t irq = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = (RC522_IRQ_GPIO >= 0) ? (1ULL << RC522_IRQ_GPIO) : 0,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        (void)gpio_config(&irq);
    }

    ret = mfrc522_init_bitbang(RC522_SCK_GPIO, RC522_MOSI_GPIO, RC522_MISO_GPIO, RC522_CS_GPIO,
                               (int)RC522_RST_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RC522: %s", esp_err_to_name(ret));
#if BOARD_ENABLE_LCD
        lcd_ui_show_centered("RC522 loi");
#endif
        return;
    }

    ESP_LOGI(TAG, "RC522 bit-bang init OK (3-bus mode)");
#endif /* BOARD_ENABLE_RFID */

#if BOARD_RC522_SHARE_SD_SPI_BUS && BOARD_ENABLE_RFID && !BOARD_ENABLE_SD
    ESP_LOGW(TAG,
             "RFID_ONLY / tat SD: chi init SPI bus cho RC522 — khong mount the nho (khong doc FAT). Bat BOARD_RFID_ONLY 0 + BOARD_ENABLE_SD de dung SD.");
#endif

#if BOARD_ENABLE_AZURE
    /* Start Azure IoT background task */
    app_azure_start();
#endif

#if BOARD_ENABLE_AUDIO
    /* Âm thanh: 1=WiFi+NTP+SD, 2/3=IN/OUT — trước app_rfid */
    app_audio_start();
#if BOARD_AUDIO_STRESS_TEST
    /* Thử nghiệm: xếp hàng 1+2+3.wav; tắt bằng BOARD_AUDIO_STRESS_TEST 0 trong board_pins.h */
    if (sd_card_is_mounted()) {
        app_audio_stress_queue_all_three();
    }
#endif
#endif

#if BOARD_ENABLE_RFID
    app_rfid_start();
#endif

    {
        const size_t in_tot = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        const size_t in_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t sp_tot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const size_t sp_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG,
                 "Heap: INTERNAL dung ~%u / %u bytes | SPIRAM dung ~%u / %u bytes",
                 (unsigned)(in_tot - in_free), (unsigned)in_tot,
                 (unsigned)(sp_tot > 0 ? sp_tot - sp_free : 0), (unsigned)sp_tot);
    }
}
