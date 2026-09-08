idf_component_register(
    SRCS main.c app_rfid.c app_azure.c app_ota.c app_audio.c lcd_ui.c lcd_panel_config.c mfrc522.c wifi_portal.c portal_web.c sd_card.c card_profile.c
         scan_log.c sd_png.c stb_image_impl.c lv_port.c lv_port_screens.c lv_port_jpeg.c
         ui_log_model.c ui_card_cache.c arial_vn_24.c arial_vn_32.c attendance_day.c ds3231.c mem_guard.c
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_driver_spi esp_driver_i2s esp_driver_i2c esp_lcd esp_lcd_ili9341 esp_mm esp_timer esp_wifi esp_netif esp_event esp_http_server mdns
             fatfs sdmmc mbedtls mqtt json esp_https_ota app_update spi_flash)

target_add_binary_data(${COMPONENT_LIB} "azure_iot_ca.pem" TEXT)
target_add_binary_data(${COMPONENT_LIB} "portal.html.gz" BINARY)

# Moi lan build ghi lai gio compile — ninja khong compile lai esp_app_desc.c
# (file khong doi) nen portal cu hien cung mot moc thoi gian.
set(RFID_STAMP_C "${CMAKE_BINARY_DIR}/generated/build_stamp.c")
add_custom_target(gen_build_stamp ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} "-DSTAMP_C=${RFID_STAMP_C}" -P "${CMAKE_CURRENT_LIST_DIR}/gen_build_stamp.cmake"
    BYPRODUCTS ${RFID_STAMP_C}
    COMMENT "Generate firmware compile timestamp"
)
set_source_files_properties(${RFID_STAMP_C} PROPERTIES GENERATED TRUE)
target_sources(${COMPONENT_LIB} PRIVATE ${RFID_STAMP_C})
add_dependencies(${COMPONENT_LIB} gen_build_stamp)

# LVGL compile: tim thay lv_psram_mem.h (heap widget → PSRAM).
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
