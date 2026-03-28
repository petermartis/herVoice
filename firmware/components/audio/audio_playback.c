#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "audio.h"
#include "ring_buffer.h"
#include "sdkconfig.h"

static const char *TAG = "AUDIO_PLAY";

/* Waveshare ESP32-S3-Touch-LCD-1.85C speaker pins */
#define SPK_I2S_PORT        I2S_NUM_1
#define SPK_WS_GPIO         GPIO_NUM_15
#define SPK_BCK_GPIO        GPIO_NUM_16
#define SPK_DOUT_GPIO       GPIO_NUM_17

#define SAMPLE_RATE         CONFIG_HERVOICE_SAMPLE_RATE
#define PLAYBACK_CHUNK      512
#define PLAYBACK_BUF_SAMPLES 8192
#define PLAYBACK_TASK_STACK 4096
#define PLAYBACK_TASK_PRIO  6

static i2s_chan_handle_t s_spk_chan = NULL;
static ring_buf_t        s_playback_ring;
static TaskHandle_t      s_playback_task_handle = NULL;
static volatile bool     s_flush_requested = false;

static void playback_task(void *arg)
{
    int16_t chunk[PLAYBACK_CHUNK];
    int16_t silence[PLAYBACK_CHUNK];
    size_t  bytes_written;

    memset(silence, 0, sizeof(silence));
    ESP_LOGI(TAG, "Playback task started");

    while (1) {
        size_t got = ring_buf_read(&s_playback_ring, chunk, PLAYBACK_CHUNK,
                                   pdMS_TO_TICKS(20));
        if (got == 0) {
            if (s_flush_requested) {
                s_flush_requested = false;
            }
            /* Underrun: output silence */
            i2s_channel_write(s_spk_chan, silence, sizeof(silence),
                              &bytes_written, portMAX_DELAY);
        } else {
            /* Pad remainder with silence if partial chunk */
            if (got < PLAYBACK_CHUNK) {
                memset(chunk + got, 0, (PLAYBACK_CHUNK - got) * sizeof(int16_t));
            }
            i2s_channel_write(s_spk_chan, chunk, sizeof(chunk),
                              &bytes_written, portMAX_DELAY);
        }
    }
}

/* Called from audio_init() in audio_capture.c after mic init */
esp_err_t audio_playback_init(void)
{
    esp_err_t ret = ring_buf_init(&s_playback_ring, PLAYBACK_BUF_SAMPLES);
    if (ret != ESP_OK) return ret;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = PLAYBACK_CHUNK;
    ret = i2s_new_channel(&chan_cfg, &s_spk_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel speaker: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCK_GPIO,
            .ws   = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(s_spk_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_spk_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable speaker channel: %s", esp_err_to_name(ret));
        return ret;
    }

    xTaskCreatePinnedToCore(playback_task, "audio_play", PLAYBACK_TASK_STACK,
                            NULL, PLAYBACK_TASK_PRIO, &s_playback_task_handle, 1);

    ESP_LOGI(TAG, "Speaker I2S initialized at %d Hz", SAMPLE_RATE);
    return ESP_OK;
}

void audio_play_frames(const int16_t *buf, size_t samples)
{
    ring_buf_write(&s_playback_ring, buf, samples);
}

void audio_playback_flush(void)
{
    s_flush_requested = true;
    /* Wait until ring buffer is drained */
    while (ring_buf_available(&s_playback_ring) > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(50)); /* extra for DMA drain */
}
