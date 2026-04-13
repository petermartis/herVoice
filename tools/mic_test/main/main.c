/*
 * herVoice — microphone diagnostic
 *
 * Initialises the I2S mic ONLY (no speaker, no PA_EN on GPIO15)
 * and prints audio statistics every 500ms so we can confirm
 * whether the mic is delivering data.
 *
 * Waveshare ESP32-S3-Touch-LCD-1.85C mic wiring (from schematic):
 *   MIC_WS  = GPIO2   (LRCLK / word select)
 *   MIC_SCK = GPIO15  (BCLK)
 *   MIC_DIN = GPIO39  (DATA)
 *
 * Expected output when mic works:  RMS > 0, samples vary around 0
 * Expected output when broken:     RMS = 0, all samples are zero
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "MIC_TEST";

/* ── Pin assignments (must match schematic exactly) ─────────────────── */
#define MIC_I2S_PORT    I2S_NUM_0
#define MIC_WS_GPIO     GPIO_NUM_2
#define MIC_SCK_GPIO    GPIO_NUM_15
#define MIC_DIN_GPIO    GPIO_NUM_39

#define SAMPLE_RATE     16000
#define DMA_BUF_SAMPLES 512
#define DMA_BUF_COUNT   4
#define READ_SAMPLES    1024  /* stereo: 512 frames × 2 channels */
#define REPORT_EVERY_MS 500

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Give USB CDC/JTAG time to enumerate and host to connect */
    vTaskDelay(pdMS_TO_TICKS(3000));

    printf("\n\n");
    printf("==============================================\n");
    printf("  herVoice MIC DIAGNOSTIC\n");
    printf("  GPIO: WS=%d  SCK=%d  DIN=%d  rate=%d Hz\n",
           MIC_WS_GPIO, MIC_SCK_GPIO, MIC_DIN_GPIO, SAMPLE_RATE);
    printf("  NOTE: GPIO15 (SCK) NOT touched by PA_EN\n");
    printf("==============================================\n\n");

    /* ── I2S channel ─────────────────────────────────────────────────── */
    i2s_chan_handle_t rx_chan = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_SAMPLES;

    ret = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) {
        printf("[FAIL] i2s_new_channel: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        return;
    }
    printf("[OK] i2s_new_channel\n");

    /* ── Standard I2S config — MSB, 16-bit STEREO (two mics, L+R) ───── */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_GPIO,
            .ws   = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        printf("[FAIL] i2s_channel_init_std_mode: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        return;
    }
    printf("[OK] i2s_channel_init_std_mode\n");

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) {
        printf("[FAIL] i2s_channel_enable: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        return;
    }
    printf("[OK] i2s_channel_enable — mic running at %d Hz\n\n", SAMPLE_RATE);
    printf("%-5s  %-8s %-8s  %-8s %-8s  %s\n",
           "tick", "RMS_L", "RMS_R", "nz_L/tot", "nz_R/tot", "status");
    printf("-----  -------- --------  -------- --------  --------\n");

    /* stereo interleaved: L, R, L, R ... */
    int16_t buf[READ_SAMPLES];
    uint32_t tick = 0;

    int64_t  energy_L = 0, energy_R = 0;
    uint32_t nz_L = 0,    nz_R = 0;
    uint32_t frames = 0;  /* stereo frames (each = 1 L + 1 R sample) */

    /* frames per report: 500ms worth */
    const uint32_t frames_per_report = SAMPLE_RATE * REPORT_EVERY_MS / 1000;

    while (1) {
        size_t bytes_read = 0;
        ret = i2s_channel_read(rx_chan, buf, sizeof(buf), &bytes_read,
                               pdMS_TO_TICKS(200));

        if (ret != ESP_OK) {
            printf("[WARN] i2s_channel_read: %s\n", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t n = bytes_read / sizeof(int16_t);  /* total samples */
        /* interleaved stereo: even index = L, odd = R */
        for (size_t i = 0; i + 1 < n; i += 2) {
            int32_t l = buf[i], r = buf[i+1];
            energy_L += l * l;  energy_R += r * r;
            if (l != 0) nz_L++;
            if (r != 0) nz_R++;
        }
        frames += n / 2;

        if (frames >= frames_per_report) {
            uint32_t rms_L = (uint32_t)sqrtf((float)(energy_L / frames));
            uint32_t rms_R = (uint32_t)sqrtf((float)(energy_R / frames));

            const char *status;
            if (nz_L == 0 && nz_R == 0) {
                status = "BOTH ZERO — clock/pin problem";
            } else if (nz_L == 0) {
                status = "L=DEAD  R=alive";
            } else if (nz_R == 0) {
                status = "L=alive  R=DEAD";
            } else if (rms_L < 10 && rms_R < 10) {
                status = "both very quiet";
            } else {
                status = "BOTH OK";
            }

            printf("%-5lu  %-8lu %-8lu  %5lu/%-4lu %5lu/%-4lu  %s\n",
                   (unsigned long)tick,
                   (unsigned long)rms_L, (unsigned long)rms_R,
                   (unsigned long)nz_L,  (unsigned long)frames,
                   (unsigned long)nz_R,  (unsigned long)frames,
                   status);

            energy_L = energy_R = 0;
            nz_L = nz_R = frames = 0;
            tick++;
        }
    }
}
