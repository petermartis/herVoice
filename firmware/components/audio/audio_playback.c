#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "es8311.h"
#include "audio.h"
#include "ring_buffer.h"
#include "sdkconfig.h"

static const char *TAG = "AUDIO_PLAY";

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85C (V2) — ES8311 DAC codec + power amp
 *
 * Speaker path: ESP32 I2S DOUT (GPIO47) → ES8311 (I2C 0x18) → power amp
 * PA_EN = GPIO15 — drive HIGH to unmute the amplifier (confirmed from
 *                  official Waveshare demo: pinMode(15,OUTPUT); digitalWrite(15,HIGH))
 * I2C bus: SDA=GPIO11, SCL=GPIO10, already installed by ui_init().
 * I2S TX channel (g_tx_chan) is created and enabled by audio_capture.c.
 */
#define I2C_PORT            I2C_NUM_0
#define PA_EN_GPIO          GPIO_NUM_15

#define SAMPLE_RATE         CONFIG_HERVOICE_SAMPLE_RATE
#define PLAYBACK_CHUNK      512
#define PLAYBACK_BUF_SAMPLES 8192
#define PLAYBACK_TASK_STACK 8192
#define PLAYBACK_TASK_PRIO  6

static i2s_chan_handle_t  s_tx_chan = NULL;
static ring_buf_t         s_playback_ring;
static TaskHandle_t       s_playback_task_handle = NULL;
static volatile bool      s_flush_requested = false;

static void playback_task(void *arg)
{
    /* Consumer reads mono; I2S is stereo → duplicate L=R */
    int16_t mono[PLAYBACK_CHUNK];
    int16_t stereo[PLAYBACK_CHUNK * 2];
    size_t  bytes_written;

    ESP_LOGI(TAG, "Playback task started");
    while (1) {
        size_t got = ring_buf_read(&s_playback_ring, mono, PLAYBACK_CHUNK,
                                   pdMS_TO_TICKS(20));
        if (got == 0) {
            if (s_flush_requested) {
                s_flush_requested = false;
            }
            memset(stereo, 0, sizeof(stereo));
        } else {
            for (size_t i = 0; i < PLAYBACK_CHUNK; i++) {
                int16_t s = (i < got) ? mono[i] : 0;
                stereo[i * 2]     = s;
                stereo[i * 2 + 1] = s;
            }
        }
        i2s_channel_write(s_tx_chan, stereo, sizeof(stereo),
                          &bytes_written, portMAX_DELAY);
    }
}

/* Called from audio_init() in audio_capture.c after I2S channels are enabled */
esp_err_t audio_playback_init(i2s_chan_handle_t tx_chan)
{
    s_tx_chan = tx_chan;

    esp_err_t ret = ring_buf_init(&s_playback_ring, PLAYBACK_BUF_SAMPLES);
    if (ret != ESP_OK) return ret;

    /* ── ES8311 speaker codec ────────────────────────────────────────────
     * I2C bus already up (400 kHz, installed by ui_init → tca9554_init).
     * ES8311_ADDRRES_0 = 0x18 (CE pin low on this board).
     * MCLK is already running (ESP32 drives GPIO2) by the time we reach here.
     */
    es8311_handle_t es8311_handle = es8311_create(I2C_PORT, ES8311_ADDRRES_0);
    if (es8311_handle == NULL) {
        ESP_LOGE(TAG, "es8311_create failed");
        return ESP_ERR_NO_MEM;
    }

    const es8311_clock_config_t es8311_clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = SAMPLE_RATE * 256,  /* 4 096 000 Hz */
        .sample_frequency   = SAMPLE_RATE,
    };
    ret = es8311_init(es8311_handle, &es8311_clk,
                      ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "es8311_init: %s", esp_err_to_name(ret));
        return ret;
    }

    int vol_actual = 0;
    es8311_voice_volume_set(es8311_handle, 50, &vol_actual);
    ESP_LOGI(TAG, "ES8311 ready — speaker at %u Hz, volume=%d", SAMPLE_RATE, vol_actual);

    /* ── PA_EN: drive GPIO15 HIGH to unmute the power amplifier ─────── */
    const gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PA_EN_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_EN_GPIO, 1);
    ESP_LOGI(TAG, "PA_EN GPIO%d HIGH — amplifier unmuted", PA_EN_GPIO);

    xTaskCreatePinnedToCore(playback_task, "audio_play", PLAYBACK_TASK_STACK,
                            NULL, PLAYBACK_TASK_PRIO, &s_playback_task_handle, 1);
    return ESP_OK;
}

void audio_play_test_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    const uint32_t n_samples = (SAMPLE_RATE * duration_ms) / 1000;
    const int16_t  amplitude = 20000;   /* ~60% of full scale */
    int16_t        chunk[256];
    uint32_t       written = 0;

    ESP_LOGI(TAG, "Playing test tone: %lu Hz, %lu ms (%lu samples)",
             (unsigned long)freq_hz, (unsigned long)duration_ms, (unsigned long)n_samples);

    while (written < n_samples) {
        uint32_t todo = n_samples - written;
        if (todo > 256) todo = 256;
        for (uint32_t i = 0; i < todo; i++) {
            float angle = 2.0f * (float)M_PI * freq_hz * (written + i) / SAMPLE_RATE;
            chunk[i] = (int16_t)(amplitude * sinf(angle));
        }
        ring_buf_write(&s_playback_ring, chunk, todo);
        written += todo;
    }
    /* Wait for the ring buffer to drain so tone is audible before caller continues */
    vTaskDelay(pdMS_TO_TICKS(duration_ms + 50));
}

void audio_play_frames(const int16_t *buf, size_t samples)
{
    ring_buf_write(&s_playback_ring, buf, samples);
}

void audio_playback_flush(void)
{
    s_flush_requested = true;
    while (ring_buf_available(&s_playback_ring) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50));  /* extra for DMA drain */
}
