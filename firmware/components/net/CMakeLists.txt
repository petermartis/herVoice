idf_component_register(
    SRCS "net_stream.c" "wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES audio wake ui timer esp_wifi esp_event lwip freertos esp_timer
)
