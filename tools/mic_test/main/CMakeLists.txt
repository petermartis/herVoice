idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -lm)
