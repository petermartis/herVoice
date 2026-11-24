#include <string.h>
#include <stdlib.h>
#include <math.h>
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
static volatile bool      s_suspended        = false;

static const esp_wn_iface_t *s_wakenet = NULL;
static model_iface_data_t   *s_model   = NULL;
static int      s_chunk_size  = 0;
static int16_t *s_detect_buf  = NULL;
static uint32_t s_last_rms_ms = 0;
static uint32_t s_frames_seen = 0;
static uint32_t s_peak_rms    = 0;

static void detection_task(void *arg)
{
    uint32_t last_wake_ms = 0;

    ESP_LOGI(TAG, "Wake word detection task started (chunk=%d samples, buf=%p)",
             s_chunk_size, (void *)s_detect_buf);

    while (1) {
        if (s_suspended) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t got = audio_get_frames(s_detect_buf, s_chunk_size, pdMS_TO_TICKS(100));
        if (got < s_chunk_size) {
            continue;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* AGC: use the model's own volume gain (target -30 dBFS) instead of a
         * fixed multiplier. Only affects s_detect_buf (feeds WakeNet); the raw
         * ring buffer / STT path is untouched. */
        float wgain = s_wakenet->get_vol_gain(s_model, -30.0f);
        if (wgain > 0.1f && wgain < 100.0f) {
            for (size_t i = 0; i < (size_t)s_chunk_size; i++) {
                int32_t v = (int32_t)s_detect_buf[i] * (int32_t)wgain;
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                s_detect_buf[i] = (int16_t)v;
            }
        }

        /* RMS of this chunk (after gain); track the peak over the 2 s HB window
         * so a spoken "Sophia" can't hide between samples */
        uint64_t e2 = 0;
        for (size_t i = 0; i < (size_t)s_chunk_size; i++) {
            int32_t smp = s_detect_buf[i];
            e2 += (uint64_t)(smp * smp);
        }
        uint32_t rms = (uint32_t)sqrtf((float)(e2 / (size_t)s_chunk_size));
        if (rms > s_peak_rms) {
            s_peak_rms = rms;
        }

        /* Run WakeNet - the model MUST be fed exactly its chunk size */
        int wakeword_index = s_wakenet->detect(s_model, s_detect_buf);

        /* Heartbeat: peak + current RMS over 2 s (a "Sophia" shows as a peak jump) */
        if (now_ms - s_last_rms_ms >= 2000) {
            ESP_LOGI(TAG, "WAKE HB: peak_rms=%lu cur_rms=%lu frames=%lu",
                     (unsigned long)s_peak_rms, (unsigned long)rms, (unsigned long)s_frames_seen);
            s_peak_rms = 0;
            s_last_rms_ms = now_ms;
        }
        s_frames_seen++;

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


/* Session control: park the detector for the duration of a session so the
 * session reader gets the capture ring exclusively. Two readers on the
 * 500 ms ring split the mic stream, so the session would only upload a
 * fragment of the utterance (whisper hears nothing -> empty transcript ->
 * no reply). */
void wake_detection_suspend(void)
{
    /* Flag, not vTaskSuspend: suspending the task while it was between a
     * per-sample mutex take/give inside ring_buf_read left the ring mutex
     * owned by a suspended task and deadlocked the capture + net tasks
     * (stuck "Recording"). The flag parks the task only at a clean loop
     * boundary, where it holds no locks. */
    s_suspended = true;
}

void wake_detection_resume(void)
{
    s_suspended = false;
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

    s_model = s_wakenet->create(WAKE_MODEL_NAME, DET_MODE_95);
    if (!s_model) {
        ESP_LOGW(TAG, "WakeNet create failed — using touch trigger only");
        s_wakenet = NULL;
        return ESP_OK;
    }

    s_chunk_size = s_wakenet->get_samp_chunksize(s_model);
    if (s_chunk_size <= 0) {
        ESP_LOGE(TAG, "WakeNet get_samp_chunksize failed (%d)", s_chunk_size);
        s_wakenet = NULL;
        s_model = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    s_detect_buf = malloc((size_t)s_chunk_size * sizeof(int16_t));
    if (!s_detect_buf) {
        ESP_LOGE(TAG, "Failed to allocate wake detect buffer (%d samples)", s_chunk_size);
        s_wakenet = NULL;
        s_model = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Keep the model's calibrated threshold. Forcing 0.4 (the most sensitive
     * value the engine accepts) let ambient sound open sessions several
     * times an hour. Range is 0.4-0.9999; higher = fewer false wakes. */
    float thr = s_wakenet->get_det_threshold(s_model, 1);
    ESP_LOGI(TAG, "WakeNet: chunk=%d (aligned), mode=DET_MODE_95, threshold word1 %f",
             s_chunk_size, (double)thr);
    /* DIAGNOSTIC: what does the model actually expect? */
    int   ch_num   = s_wakenet->get_channel_num(s_model);
    int   smp_rate = s_wakenet->get_samp_rate(s_model);
    float vgain    = s_wakenet->get_vol_gain(s_model, -30.0f);
    ESP_LOGI(TAG, "WAKE DIAG: channels=%d samp_rate=%d vol_gain(-30dB)=%f",
             ch_num, smp_rate, (double)vgain);
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
