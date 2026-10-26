#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "audio.h"
#include "wake.h"
#include "model_path.h"   /* esp-sr: map the model partition so WakeNet can load */"
#include "net.h"
#include "ui.h"
#include "touch.h"
#include "config.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "herVoice starting, device_id=0x%08X", CFG_DEVICE_ID);

    // Init subsystems in order
    ESP_ERROR_CHECK(ui_init());
    ui_set_state(UI_STATE_IDLE);

    // Map the SR model partition (WakeNet) and CREATE the model BEFORE any I2S
    // DMA. The prebuilt wakenet lib parses the hufzip header via strcmp on
    // pointers into the flash-mmap model; if I2S DMA is already running it
    // invalidates the flash cache and those reads return garbage, corrupting the
    // model params (intermittent LoadProhibited in the SysTick ISR).
    // Order: model create -> audio init/test tone -> capture task -> detection.
    srmodel_list_t *srmodels = esp_srmodel_init("model");
    if (!srmodels) {
        ESP_LOGW(TAG, "esp_srmodel_init failed — wake word disabled, touch only");
    }
    ESP_ERROR_CHECK(wake_init());

    ESP_ERROR_CHECK(audio_init());
    audio_play_test_tone(440, 600);   /* A4 beep — confirms speaker path */

    // Start audio capture and wake detection — independent of network
    audio_start_capture_task();
    wake_start_detection_task();

    // Touch controller (after ui_init which sets up I2C bus)
    ret = touch_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch_init failed: %s (continuing without touch)", esp_err_to_name(ret));
    }

    // net_init blocks until connected or all retries exhausted
    ret = net_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "net_init failed, will retry in background");
        ui_set_state(UI_STATE_ERROR);
    }

    ESP_LOGI(TAG, "All subsystems initialized");

    net_start_stream_task();

    ESP_LOGI(TAG, "Tasks started, entering idle loop");

    // Main task just monitors and feeds watchdog
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGD(TAG, "alive, free heap=%lu", esp_get_free_heap_size());
    }
}
