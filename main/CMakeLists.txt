idf_component_register(
    SRCS "main.c" "mic_input.c" "board_power.c" "status_display.c"
    INCLUDE_DIRS "."
    REQUIRES bt driver esp_driver_i2s nvs_flash esp_lcd
)
