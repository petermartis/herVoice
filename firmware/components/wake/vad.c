#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "wake.h"
#include "sdkconfig.h"

static const char *TAG = "VAD";

#define VAD_START_THRESHOLD  CONFIG_HERVOICE_VAD_START_THRESHOLD
#define VAD_STOP_THRESHOLD   CONFIG_HERVOICE_VAD_STOP_THRESHOLD
#define VAD_SILENCE_MS       CONFIG_HERVOICE_VAD_SILENCE_TIMEOUT_MS
#define SAMPLE_RATE          CONFIG_HERVOICE_SAMPLE_RATE

static bool     s_in_speech     = false;
static uint32_t s_silence_start = 0;

static uint32_t rms(const int16_t *samples, size_t count)
{
    if (count == 0) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        sum += (uint64_t)(s * s);
    }
    return (uint32_t)sqrtf((float)(sum / count));
}

bool vad_is_speech(const int16_t *samples, size_t count)
{
    uint32_t energy = rms(samples, count);
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if (!s_in_speech) {
        if (energy >= VAD_START_THRESHOLD) {
            s_in_speech = true;
            s_silence_start = 0;
            ESP_LOGD(TAG, "Speech START (energy=%lu)", energy);
            return true;
        }
        return false;
    } else {
        /* Currently in speech */
        if (energy < VAD_STOP_THRESHOLD) {
            if (s_silence_start == 0) {
                s_silence_start = now_ms;
            } else if ((now_ms - s_silence_start) >= (uint32_t)VAD_SILENCE_MS) {
                s_in_speech = false;
                s_silence_start = 0;
                ESP_LOGD(TAG, "Speech END (silence timeout)");
                return false;
            }
        } else {
            s_silence_start = 0; /* Reset silence timer on active speech */
        }
        return true;
    }
}

void vad_reset(void)
{
    s_in_speech = false;
    s_silence_start = 0;
}
