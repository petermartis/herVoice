idf_component_register(
    SRCS "audio_capture.c" "audio_playback.c" "ring_buffer.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s freertos
)
