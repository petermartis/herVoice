idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_lcd esp_lcd_st77916 freertos esp_timer driver
)
