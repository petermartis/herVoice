idf_component_register(SRCS "esp_lcd_st77916.c" "esp_lcd_st77916_spi.c" "esp_lcd_st77916_mipi.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES "esp_lcd" "driver")

target_compile_definitions(__idf_esp_lcd_st77916 PRIVATE
    ESP_LCD_ST77916_VER_MAJOR=2
    ESP_LCD_ST77916_VER_MINOR=0
    ESP_LCD_ST77916_VER_PATCH=2
)
