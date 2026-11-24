/* herVoice mic_streamer: production audio component, raw PCM out the USB
 * Serial/JTAG console in checksummed framed chunks. Used to capture the
 * wake-word corpus through the puck's mic array while utterances play over
 * a loudspeaker. No WiFi, no UI, no WakeNet, no logging in the data stream. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/usb_serial_jtag.h"
#include "audio.h"
#include "sdkconfig.h"

#define CHUNK_SAMPLES 128   /* 8 ms at 16 kHz; 256 B payload keeps USB-CDC TX chunked writes atomic */

void app_main(void)
{
    nvs_flash_init();

    /* ui_init() normally owns I2C0 install in production firmware; audio
     * (ES7210) depends on it. Install it here instead. */
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_11,
        .scl_io_num = GPIO_NUM_10,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &i2c_conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

    /* We own the USB Serial/JTAG peripheral (console disabled via Kconfig) */
    usb_serial_jtag_driver_config_t usbcfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 2048,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usbcfg));

    vTaskDelay(pdMS_TO_TICKS(2000));   /* let USB CDC enumerate */

    ESP_ERROR_CHECK(audio_init());
    audio_start_capture_task();

    printf("MIC_STREAMER_READY %d\n", CONFIG_HERVOICE_SAMPLE_RATE);
    fflush(stdout);
    usleep(200000);

    static int16_t buf[CHUNK_SAMPLES];
    /* pkt = magic(4) len(2) seq(2) csum(4) payload */
    static uint8_t pkt[12 + CHUNK_SAMPLES * 2];
    uint32_t seq = 0;
    while (1) {
        size_t got = audio_get_frames(buf, CHUNK_SAMPLES, pdMS_TO_TICKS(200));
        if (got == 0) continue;
        uint16_t n = (uint16_t)(got * sizeof(int16_t));
        uint32_t csum = seq;
        const uint8_t *pl = (const uint8_t *)buf;
        for (uint16_t i = 0; i < n; i++) csum = (csum * 31u + pl[i]) & 0xFFFFFFFFu;
        memcpy(pkt, "HVPC", 4);
        pkt[4] = n & 0xFF; pkt[5] = n >> 8;
        pkt[6] = seq & 0xFF; pkt[7] = (seq >> 8) & 0xFF;
        pkt[8] = csum & 0xFF; pkt[9] = (csum >> 8) & 0xFF;
        pkt[10] = (csum >> 16) & 0xFF; pkt[11] = (csum >> 24) & 0xFF;
        memcpy(pkt + 12, buf, n);
        size_t off = 0, total = 12 + (size_t)n;
        while (off < total) {   /* driver API: NO VFS LF->CRLF translation */
            int w = usb_serial_jtag_write_bytes(pkt + off, total - off,
                                                pdMS_TO_TICKS(500));
            if (w <= 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
            off += (size_t)w;
        }
        seq++;
    }
}
