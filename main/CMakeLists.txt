idf_component_register(
    SRCS main.c app_rfid.c app_azure.c app_ota.c app_audio.c lcd_ui.c lcd_panel_config.c mfrc522.c wifi_portal.c portal_web.c sd_card.c card_profile.c
         scan_log.c sd_png.c stb_image_impl.c lv_port.c lv_port_screens.c lv_port_jpeg.c
         ui_log_model.c ui_card_cache.c arial_vn_24.c arial_vn_32.c
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_driver_spi esp_driver_i2s esp_lcd esp_lcd_ili9341 esp_mm esp_timer esp_wifi esp_netif esp_event esp_http_server
             fatfs sdmmc mbedtls mqtt json esp_https_ota app_update)

target_add_binary_data(${COMPONENT_LIB} "azure_iot_ca.pem" TEXT)
