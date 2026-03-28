#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mn_iface.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "audio.h"
#include "wake.h"
#include "sdkconfig.h"

static const char *TAG = "WAKE";

/* WakeNet window: 30ms at 16kHz = 480 samples */
#define SAMPLE_RATE         CONFIG_HERVOICE_SAMPLE_RATE
#define WINDOW_MS           30
#define WINDOW_SAMPLES      ((SAMPLE_RATE * WINDOW_MS) / 1000)
#define WAKE_THRESHOLD      CONFIG_HERVOICE_WAKE_THRESHOLD
#define WAKE_MODEL_NAME     "wn9_sophia_tts"
#define DEBOUNCE_MS         1500
#define EVENT_QUEUE_SIZE    4
#define DETECT_TASK_STACK   8192
#define DETECT_TASK_PRIO    4

static QueueHandle_t      s_event_queue = NULL;
static TaskHandle_t       s_detect_task_handle = NULL;

static const esp_wn_iface_t *s_wakenet = NULL;
static model_iface_data_t   *s_model   = NULL;

static void detection_task(void *arg)
{
    int16_t  window[WINDOW_SAMPLES];
    uint32_t last_wake_ms = 0;

    ESP_LOGI(TAG, "Wake word detection task started (window=%d samples)", WINDOW_SAMPLES);

    while (1) {
        size_t got = audio_get_frames(window, WINDOW_SAMPLES, pdMS_TO_TICKS(100));
        if (got < WINDOW_SAMPLES) {
            continue;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* Run WakeNet */
        int wakeword_index = s_wakenet->detect(s_model, window);

        if (wakeword_index > 0) {
            /* Debounce */
            if ((now_ms - last_wake_ms) < DEBOUNCE_MS) {
                ESP_LOGD(TAG, "Wake word debounced");
                continue;
            }
            last_wake_ms = now_ms;

            ESP_LOGI(TAG, "Wake word detected! index=%d", wakeword_index);

            wake_event_t evt = {
                .type         = WAKE_EVENT_WAKEWORD_DETECTED,
                .timestamp_ms = now_ms,
            };
            xQueueSend(s_event_queue, &evt, pdMS_TO_TICKS(100));
            vad_reset();
        }
    }
}

esp_err_t wake_init(void)
{
    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(wake_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* Load WakeNet model — non-fatal; touch screen is the primary wake trigger */
    s_wakenet = esp_wn_handle_from_name(WAKE_MODEL_NAME);
    if (!s_wakenet) {
        ESP_LOGW(TAG, "WakeNet model '%s' not available — using touch trigger only",
                 WAKE_MODEL_NAME);
        return ESP_OK;
    }

    s_model = s_wakenet->create(WAKE_MODEL_NAME, DET_MODE_90);
    if (!s_model) {
        ESP_LOGW(TAG, "WakeNet create failed — using touch trigger only");
        s_wakenet = NULL;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WakeNet initialized, model chunk_num=%d",
             s_wakenet->get_samp_chunksize(s_model));
    return ESP_OK;
}

void wake_start_detection_task(void)
{
    if (!s_wakenet || !s_model) {
        ESP_LOGW(TAG, "No WakeNet model, skipping detection task");
        return;
    }
    xTaskCreatePinnedToCore(detection_task, "wake_detect", DETECT_TASK_STACK,
                            NULL, DETECT_TASK_PRIO, &s_detect_task_handle, 1);
}

QueueHandle_t wake_get_event_queue(void)
{
    return s_event_queue;
}
