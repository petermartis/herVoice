#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKE_EVENT_WAKEWORD_DETECTED = 0,
    WAKE_EVENT_SPEECH_START,
    WAKE_EVENT_SPEECH_END,
} wake_event_type_t;

typedef struct {
    wake_event_type_t type;
    uint32_t          timestamp_ms;
} wake_event_t;

/* Must be called after audio_init() */
esp_err_t wake_init(void);

/* Starts the wakeword detection FreeRTOS task */
void wake_start_detection_task(void);

/* Returns the queue from which net_stream_task reads wake events */
QueueHandle_t wake_get_event_queue(void);

/* VAD API used internally by wakeword.c but also accessible externally */
bool vad_is_speech(const int16_t *samples, size_t count);
void vad_reset(void);

#ifdef __cplusplus
}
#endif
