set(srcs "vad.c")

if(CONFIG_HERVOICE_WAKE_ENGINE_WAKENET)
    list(APPEND srcs "wakeword.c")
else()
    list(APPEND srcs "wakeword_mww.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES audio freertos esp_timer esp-sr micro_wake_word ESPMicroSpeechFeatures espressif__esp-tflite-micro
)

if(NOT CONFIG_HERVOICE_WAKE_ENGINE_WAKENET)
    target_add_binary_data(${COMPONENT_LIB} "model/charlie_ss_quant.tflite" BINARY)
endif()
