idf_component_register(
    SRCS main.c app_rfid.c app_azure.c app_audio.c ili9488_panel.c lcd_ui.c mfrc522.c wifi_portal.c sd_card.c card_profile.c
         scan_log.c sd_png.c stb_image_impl.c app_touch.c
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_driver_spi esp_driver_i2s esp_lcd esp_timer esp_wifi esp_netif esp_event esp_http_server
             fatfs sdmmc mbedtls mqtt json)
