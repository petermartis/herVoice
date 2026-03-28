idf_component_register(
    SRCS "wakeword.c" "vad.c"
    INCLUDE_DIRS "include"
    REQUIRES audio esp-sr freertos esp_timer
)
