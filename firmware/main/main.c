#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "audio.h"
#include "wake.h"
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

    ESP_ERROR_CHECK(audio_init());

    ESP_ERROR_CHECK(wake_init());

    // net_init also starts Wi-Fi and connects; blocks until connected or returns error
    ret = net_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "net_init failed, will retry in background");
        ui_set_state(UI_STATE_ERROR);
    }

    // Touch controller (after ui_init which sets up I2C bus)
    ret = touch_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch_init failed: %s (continuing without touch)", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "All subsystems initialized");

    // Start tasks (each internally creates its FreeRTOS task)
    audio_start_capture_task();
    wake_start_detection_task();
    net_start_stream_task();

    ESP_LOGI(TAG, "Tasks started, entering idle loop");

    // Main task just monitors and feeds watchdog
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGD(TAG, "alive, free heap=%lu", esp_get_free_heap_size());
    }
}
