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
#include "app_ota.h"
#if BOARD_ENABLE_WIFI
#include "portal_web.h"
#include "ds3231.h"
#endif

static const char *TAG = "main";

/** Giam log linh tinh: mac dinh WARN; boot + app_azure INFO (khoi dong + JSON gui Azure). */
static void app_configure_log_levels(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set("boot", ESP_LOG_INFO);
    esp_log_level_set("main", ESP_LOG_INFO);
#if BOARD_ENABLE_RFID
    esp_log_level_set("app_rfid", ESP_LOG_INFO);
#endif
    /* ESP-IDF / stack hay spam khi chay binh thuong */
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("wifi_init", ESP_LOG_ERROR);
    esp_log_level_set("wpa", ESP_LOG_ERROR);
    esp_log_level_set("phy", ESP_LOG_ERROR);
    esp_log_level_set("pp", ESP_LOG_WARN);
    esp_log_level_set("net80211", ESP_LOG_ERROR);
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
    esp_log_level_set("esp-tls", ESP_LOG_NONE);
    esp_log_level_set("transport", ESP_LOG_NONE);
    esp_log_level_set("httpd", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("esp_netif", ESP_LOG_ERROR);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_ERROR);
    esp_log_level_set("system_api", ESP_LOG_ERROR);
    esp_log_level_set("gpio", ESP_LOG_ERROR);
    esp_log_level_set("lvgl", ESP_LOG_ERROR);
    esp_log_level_set("LVGL", ESP_LOG_ERROR);
    esp_log_level_set("card_profile", ESP_LOG_ERROR);
    esp_log_level_set("card_prof", ESP_LOG_ERROR);
    esp_log_level_set("scan_log", ESP_LOG_ERROR);
    esp_log_level_set("app_audio", ESP_LOG_ERROR);
    esp_log_level_set("app_azure", ESP_LOG_INFO);
    esp_log_level_set("wifi_portal", ESP_LOG_ERROR);
    esp_log_level_set("lv_port", ESP_LOG_ERROR);
    esp_log_level_set("lv_port_ui", ESP_LOG_ERROR);
    esp_log_level_set("lv_port_jpeg", ESP_LOG_ERROR);
    esp_log_level_set("portal_web", ESP_LOG_ERROR);
    esp_log_level_set("sd_card", ESP_LOG_ERROR);
    esp_log_level_set("ds3231", ESP_LOG_ERROR);
    esp_log_level_set("app_ota", ESP_LOG_WARN);
}

/** Tom tat trang thai khoi dong — hien tren Log Terminal. */
static void app_log_boot_status(void)
{
    ESP_LOGI("boot", "--- Boot ---");
    ESP_LOGI("boot", "NVS: OK");
#if BOARD_ENABLE_SD
    ESP_LOGI("boot", "SD: %s", sd_card_is_mounted() ? "OK" : "LOI");
#endif
#if BOARD_ENABLE_LCD
    ESP_LOGI("boot", "LCD/LVGL: OK");
#endif
#if BOARD_ENABLE_WIFI
    {
        int wn = wifi_list_get_count();
        wifi_conn_status_t ws = wifi_portal_get_conn_status();
        const char *wst = (ws == WIFI_STATUS_CONNECTED) ? "OK" :
                          (ws == WIFI_STATUS_CONNECTING) ? "dang ket noi" :
                          (ws == WIFI_STATUS_FAIL) ? "LOI" :
                          (wn > 0) ? "dang ket noi" : "chua cau hinh";
        ESP_LOGI("boot", "WiFi STA: %s", wst);
        ESP_LOGI("boot", "Portal web: OK (192.168.4.1)");
        ESP_LOGI("boot", "Gio: %s", wifi_portal_time_is_valid() ? "OK" : "cho NTP/RTC");
#if BOARD_ENABLE_DS3231
        ESP_LOGI("boot", "DS3231: %s", ds3231_is_ready() ? "OK" : "khong co / loi");
#endif
        char ah[64], ad[64], am[16];
        wifi_portal_get_azure(ah, sizeof(ah), ad, sizeof(ad), am, sizeof(am));
        ESP_LOGI("boot", "Azure: %s", (ad[0] != '\0') ? "dang ket noi..." : "chua cau hinh");
    }
#endif
#if BOARD_ENABLE_RFID
    ESP_LOGI("boot", "RC522: OK");
#endif
#if BOARD_ENABLE_AUDIO
    ESP_LOGI("boot", "Audio: san sang");
#endif
#if BOARD_ENABLE_RFID
    ESP_LOGI("boot", "RFID task: OK");
#endif
    ESP_LOGI("boot", "------------");
}

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
    if (rr != ESP_RST_POWERON) {
        ESP_LOGW(TAG, "Reset reason: %s (%d)", rrs, (int)rr);
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { 
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    app_configure_log_levels();

#if BOARD_ENABLE_WIFI
    portal_web_log_init();
#endif

    app_ota_boot_guard();

#if BOARD_ENABLE_SD
    /* Bounce DMA Internal SOM — truoc LCD/LVGL/WiFi an manh Internal. */
    if (sd_card_reserve_dma_bounce() != ESP_OK) {
        ESP_LOGW(TAG, "SD DMA bounce: chua cap duoc — SD co the 0x101 khi heap manh");
    }
#endif

#if BOARD_ENABLE_LCD
    ESP_ERROR_CHECK(lcd_panel_config_init());
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
        scan_log_flush_pending();
        scan_log_trim_at_boot();
    }
#endif

#if BOARD_ENABLE_WIFI
    ret = wifi_portal_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi portal: %s", esp_err_to_name(ret));
    }
    /* Neu co lenh OTA tu xa dang cho trong NVS, uu tien tiep tuc som khi he thong con nhe. */
    (void)app_ota_resume_pending();
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
    /* Khong reserve I2S luc boot — tranh chiem DMA Internal (LCD SPI can headroom).
     * Loa: thu tao kenh luc phat; fail thi bo qua, UI/SD van chay. */
    app_audio_start();
#if BOARD_AUDIO_STRESS_TEST
    if (sd_card_is_mounted()) {
        app_audio_stress_queue_all_three();
    }
#endif
#endif

#if BOARD_ENABLE_RFID
    app_rfid_start();
#endif

    app_ota_schedule_validate_delayed();

    app_log_boot_status();
}
