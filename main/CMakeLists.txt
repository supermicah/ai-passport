idf_component_register(
    SRCS "main.c" "pager_ble.c" "pager_keys.c"
    INCLUDE_DIRS "."
    REQUIRES bsp app_update bt esp_hid nvs_flash
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pager_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/pager_font_24.c"
)
