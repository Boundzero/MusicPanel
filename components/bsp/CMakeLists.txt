# The active board implementation is selected by Kconfig. Each board lives in
# its own subdirectory under boards/ and is compiled in only when selected, so
# no board pulls in another board's pins or drivers.

set(bsp_srcs "src/bsp.cpp")

if(CONFIG_BSP_BOARD_ESP32_4848S040)
    list(APPEND bsp_srcs "boards/esp32_4848s040/board.cpp")
endif()

idf_component_register(
    SRCS ${bsp_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "boards"
    REQUIRES driver esp_lcd esp_timer lvgl esp_lcd_touch
    PRIV_REQUIRES logging esp_lcd_st7701 esp_lcd_touch_gt911 esp_lcd_panel_io_additions
)
