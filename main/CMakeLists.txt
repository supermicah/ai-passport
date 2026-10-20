idf_component_register(
    SRCS "main.c"
         "page_turner.c"
         "smoking_model.c"
         "smoking_store.c"
         "tools_wifi.c"
         "tools_weather.c"
         "tools_radio.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_audio_codec esp_event esp_hid esp_http_client esp_http_server esp_hw_support esp_netif esp_timer esp_wifi lwip mbedtls nvs_flash
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/smoking_font_18.c"
)
