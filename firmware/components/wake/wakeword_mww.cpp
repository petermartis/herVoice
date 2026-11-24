/* wakeword_mww.cpp — microWakeWord (TFLM) wake engine for herVoice.
 *
 * Replaces the closed Espressif WakeNet path with an open microWakeWord
 * streaming model ("charlie"). Drives the vendored standalone
 * micro_wake_word_standalone component (patched for esp-tflite-micro
 * >=1.4 include layout) with audio from the production capture ring and
 * the model embedded in the app binary.
 *
 * Same extern "C" surface as wakeword.c (wake.h), selected at build time
 * by CONFIG_HERVOICE_WAKE_ENGINE choice.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "audio.h"
#include "wake.h"
#include "sdkconfig.h"

#include "esphome/core/helpers.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"

static const char *TAG = "WAKE_MWW";

/* Model bytes embedded via target_add_binary_data (wake CMakeLists) */
extern const uint8_t charlie_model_start[] asm("_binary_charlie_ss_quant_tflite_start");
extern const uint8_t charlie_model_end[]   asm("_binary_charlie_ss_quant_tflite_end");

#define MWW_ARENA_BYTES    (32 * 1024)
#define MWW_CUTOFF         CONFIG_HERVOICE_MWW_CUTOFF_PCT / 100.0f
#define MWW_SLIDING_WINDOW 5
#define MWW_STEP_MS        10
#define MIC_STAGING_SAMPLES 1024
#define EVENT_QUEUE_SIZE   4
#define DETECT_TASK_STACK  8192
#define DETECT_TASK_PRIO   4

static QueueHandle_t s_event_queue = NULL;
static volatile bool s_suspended = false;
static volatile bool s_detected_flag = false;

/* Feed the component from the production capture ring instead of an I2S mic */
class HervoiceMic : public esphome::microphone::Microphone {
public:
    void start() override { state_ = esphome::microphone::STATE_RUNNING; }
    void stop() override { state_ = esphome::microphone::STATE_STOPPED; }
    size_t read(int16_t *buf, size_t len) override
    {
        size_t want = len / sizeof(int16_t);
        if (want > MIC_STAGING_SAMPLES) want = MIC_STAGING_SAMPLES;
        size_t got = audio_get_frames(m_staging_, want, pdMS_TO_TICKS(100));
        if (got == 0) return 0;
        memcpy(buf, m_staging_, got * sizeof(int16_t));
        return got * sizeof(int16_t);
    }
private:
    int16_t m_staging_[MIC_STAGING_SAMPLES];
};

static esphome::micro_wake_word::MicroWakeWord s_mww;
static HervoiceMic s_mic;

static void detection_task(void *arg)
{
    ESP_LOGI(TAG, "microWakeWord detection task started (model %u bytes, cutoff %.2f)",
             (unsigned)(charlie_model_end - charlie_model_start), (double)MWW_CUTOFF);

    s_mww.set_features_step_size(MWW_STEP_MS);
    s_mww.setup();
    s_mww.add_wake_word_model(charlie_model_start, MWW_CUTOFF,
                              MWW_SLIDING_WINDOW, "charlie", MWW_ARENA_BYTES);
    s_mww.set_microphone(&s_mic);
    s_mww.add_detection_callback([](std::string word) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "Wake word '%s' detected!", word.c_str());
        wake_event_t evt = { .type = WAKE_EVENT_WAKEWORD_DETECTED, .timestamp_ms = now_ms };
        xQueueSend(s_event_queue, &evt, pdMS_TO_TICKS(100));
        vad_reset();
        s_detected_flag = true;
    });
    s_mww.start();

    uint32_t last_hb_ms = 0;
    while (1) {
        if (s_suspended) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        s_mww.loop();
        if (s_detected_flag) {
            s_detected_flag = false;
            s_mww.start();          /* state machine returned IDLE after detection */
        }
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - last_hb_ms >= 5000) {
            last_hb_ms = now_ms;
            ESP_LOGI(TAG, "WAKE HB: mww running=%d hw=%u freeheap=%u",
                     (int)s_mww.is_running(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)xPortGetFreeHeapSize());
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

extern "C" {

esp_err_t wake_init(void)
{
    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(wake_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "microWakeWord engine: model=%u B arena=%d B step=%d ms window=%d",
             (unsigned)(charlie_model_end - charlie_model_start), MWW_ARENA_BYTES,
             MWW_STEP_MS, MWW_SLIDING_WINDOW);
    return ESP_OK;
}

void wake_start_detection_task(void)
{
    xTaskCreatePinnedToCore(detection_task, "wake_detect", DETECT_TASK_STACK,
                            NULL, DETECT_TASK_PRIO, NULL, 1);
}

QueueHandle_t wake_get_event_queue(void)
{
    return s_event_queue;
}

void wake_detection_suspend(void) { s_suspended = true; }
void wake_detection_resume(void)  { s_suspended = false; }

}  /* extern "C" */
