#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "es7210.h"
#include "audio.h"
#include "ring_buffer.h"
#include "sdkconfig.h"

static const char *TAG = "AUDIO_CAP";

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85C (V2) — ES7210 4-ch ADC codec
 *
 * Mic path: 2× MEMS mics → ES7210 (I2C 0x40) → ESP32 I2S DIN (GPIO39)
 * I2C bus:  SDA=GPIO11, SCL=GPIO10, already installed by ui_init().
 *
 * Full-duplex I2S (I2S_NUM_0) — mic RX and speaker TX share one port
 * so GPIO2 (MCLK) is owned by a single peripheral with no conflict:
 *   MCLK=GPIO2  BCK=GPIO48  WS=GPIO38
 *   DIN =GPIO39  (mic  ← ES7210)
 *   DOUT=GPIO47  (spkr → ES8311)
 */
#define I2C_PORT            I2C_NUM_0
#define ES7210_ADDR         0x40

#define I2S_PORT            I2S_NUM_0
#define I2S_MCLK_GPIO       GPIO_NUM_2
#define I2S_BCK_GPIO        GPIO_NUM_48
#define I2S_WS_GPIO         GPIO_NUM_38
#define I2S_DIN_GPIO        GPIO_NUM_39
#define I2S_DOUT_GPIO       GPIO_NUM_47

#define SAMPLE_RATE         CONFIG_HERVOICE_SAMPLE_RATE
#define DMA_BUF_SAMPLES     512
#define DMA_BUF_COUNT       4
#define RING_BUF_SAMPLES    (SAMPLE_RATE / 2)   /* 500 ms mono */
#define CAPTURE_TASK_STACK  4096
#define CAPTURE_TASK_PRIO   5
#define RMS_REPORT_MS       1000   /* log mic RMS every second */

static i2s_chan_handle_t  s_rx_chan = NULL;
       i2s_chan_handle_t  g_tx_chan = NULL;   /* exported to audio_playback.c */
static ring_buf_t         s_capture_ring;
static TaskHandle_t       s_capture_task_handle = NULL;

/* Defined in audio_playback.c — called after I2S is up */
esp_err_t audio_playback_init(i2s_chan_handle_t tx_chan);

static void capture_task(void *arg)
{
    /* ES7210 outputs stereo: L=MIC1, R=MIC2 interleaved */
    int16_t dma_buf[DMA_BUF_SAMPLES * 2];
    int16_t mono_buf[DMA_BUF_SAMPLES];
    size_t  bytes_read;

    /* RMS tracking */
    int64_t  energy       = 0;
    uint32_t total_frames = 0;
    const uint32_t report_frames = SAMPLE_RATE * RMS_REPORT_MS / 1000;

    ESP_LOGI(TAG, "Capture task started");
    while (1) {
        esp_err_t ret = i2s_channel_read(s_rx_chan, dma_buf, sizeof(dma_buf),
                                          &bytes_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_read: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t frames = (bytes_read / sizeof(int16_t)) / 2;

        /* Average L and R → mono, accumulate energy */
        for (size_t i = 0; i < frames; i++) {
            int16_t s = (int16_t)(((int32_t)dma_buf[i * 2] +
                                    (int32_t)dma_buf[i * 2 + 1]) >> 1);
            mono_buf[i] = s;
            energy += (int32_t)s * s;
        }
        ring_buf_write(&s_capture_ring, mono_buf, frames);

        total_frames += frames;
        if (total_frames >= report_frames) {
            uint32_t rms = (uint32_t)sqrtf((float)(energy / total_frames));
            ESP_LOGI(TAG, "mic RMS=%lu %s",
                     (unsigned long)rms,
                     rms == 0   ? "— SILENT (check ES7210 wiring)" :
                     rms < 50   ? "— very quiet (background noise)" :
                     rms < 500  ? "— OK (ambient)" : "— LOUD");
            energy = 0;
            total_frames = 0;
        }
    }
}

esp_err_t audio_init(void)
{
    esp_err_t ret = ring_buf_init(&s_capture_ring, RING_BUF_SAMPLES);
    if (ret != ESP_OK) return ret;

    /* ── ES7210 microphone codec ─────────────────────────────────────────
     * I2C bus already up (400 kHz, installed by ui_init → tca9554_init).
     * Settings match Waveshare V2 reference demo:
     *   I2S format, MCLK=256×Fs, 16-bit, MIC_BIAS=2.87 V, gain=36 dB.
     */
    es7210_dev_handle_t es7210_handle = NULL;
    const es7210_i2c_config_t es7210_i2c = {
        .i2c_port = I2C_PORT,
        .i2c_addr = ES7210_ADDR,
    };
    ret = es7210_new_codec(&es7210_i2c, &es7210_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "es7210_new_codec: %s", esp_err_to_name(ret));
        return ret;
    }
    const es7210_codec_config_t es7210_cfg = {
        .sample_rate_hz = SAMPLE_RATE,
        .mclk_ratio     = 256,
        .i2s_format     = ES7210_I2S_FMT_I2S,
        .bit_width      = ES7210_I2S_BITS_16B,
        .mic_bias       = ES7210_MIC_BIAS_2V87,
        .mic_gain       = ES7210_MIC_GAIN_36DB,
        .flags          = { .tdm_enable = 0 },
    };
    ret = es7210_config_codec(es7210_handle, &es7210_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "es7210_config_codec: %s", esp_err_to_name(ret));
        return ret;
    }
    /* volume_db range: -95 to +32 dB; 0 = unity (gain comes from mic_gain) */
    ret = es7210_config_volume(es7210_handle, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "es7210_config_volume: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "ES7210 ready — 2-ch mic at %u Hz, gain=36 dB", SAMPLE_RATE);

    /* ── Full-duplex I2S (one port owns MCLK/BCK/WS + DIN + DOUT) ───────
     * Pass both &g_tx_chan and &s_rx_chan to i2s_new_channel so they share
     * the same hardware instance; no GPIO conflict on MCLK=GPIO2.
     */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_SAMPLES;
    ret = i2s_new_channel(&chan_cfg, &g_tx_chan, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* TX first — it owns the clock output */
    ret = i2s_channel_init_std_mode(g_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_init TX: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_init RX: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Enable TX before RX so MCLK is running when ES7210 starts clocking */
    ret = i2s_channel_enable(g_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable TX: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable RX: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S full-duplex running at %u Hz", SAMPLE_RATE);

    /* Initialize ES8311 speaker codec and PA_EN */
    ret = audio_playback_init(g_tx_chan);
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
