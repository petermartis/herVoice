#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "audio.h"
#include "ring_buffer.h"
#include "sdkconfig.h"

static const char *TAG = "AUDIO_CAP";

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85C — confirmed from schematic + wiki:
 * Onboard mic is MSM261S4030H0R / ICS-43434 (standard I2S, not PDM)
 * MIC_WS  = GPIO2  (LRCLK / word select)
 * MIC_SCK = GPIO15 (BCLK)        NOTE: GPIO15 also used as PA_EN in audio_playback.c
 * MIC_DIN = GPIO39 (DATA in)
 */
#define MIC_I2S_PORT        I2S_NUM_0
#define MIC_WS_GPIO         GPIO_NUM_2
#define MIC_SCK_GPIO        GPIO_NUM_15
#define MIC_DIN_GPIO        GPIO_NUM_39

#define SAMPLE_RATE         CONFIG_HERVOICE_SAMPLE_RATE
#define DMA_BUF_SAMPLES     512
#define DMA_BUF_COUNT       4
#define RING_BUF_SAMPLES    (SAMPLE_RATE / 2)  /* 500ms */
#define CAPTURE_TASK_STACK  4096
#define CAPTURE_TASK_PRIO   5

static i2s_chan_handle_t  s_mic_chan = NULL;
static ring_buf_t         s_capture_ring;
static TaskHandle_t       s_capture_task_handle = NULL;

/* Forward declaration for playback init defined in audio_playback.c */
esp_err_t audio_playback_init(void);

static void capture_task(void *arg)
{
    int16_t dma_buf[DMA_BUF_SAMPLES];
    size_t  bytes_read;

    ESP_LOGI(TAG, "Capture task started");
    while (1) {
        esp_err_t ret = i2s_channel_read(s_mic_chan, dma_buf, sizeof(dma_buf),
                                          &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_read error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t samples = bytes_read / sizeof(int16_t);
        ring_buf_write(&s_capture_ring, dma_buf, samples);
    }
}

esp_err_t audio_init(void)
{
    /* Init capture ring buffer */
    esp_err_t ret = ring_buf_init(&s_capture_ring, RING_BUF_SAMPLES);
    if (ret != ESP_OK) return ret;

    /* Configure I2S PDM RX (microphone) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_SAMPLES;
    ret = i2s_new_channel(&chan_cfg, NULL, &s_mic_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws   = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(s_mic_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode (mic): %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_mic_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable mic channel: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Microphone I2S initialized at %d Hz", SAMPLE_RATE);

    /* Initialize speaker/playback side */
    ret = audio_playback_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_playback_init: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

void audio_start_capture_task(void)
{
    xTaskCreatePinnedToCore(capture_task, "audio_cap", CAPTURE_TASK_STACK,
                            NULL, CAPTURE_TASK_PRIO, &s_capture_task_handle, 1);
}

size_t audio_get_frames(int16_t *buf, size_t samples, TickType_t timeout_ticks)
{
    return ring_buf_read(&s_capture_ring, buf, samples, timeout_ticks);
}
