idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES audio wake net ui touch nvs_flash esp_wifi esp_event
)
