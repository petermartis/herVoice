#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "timer.h"
#include "ui.h"
#include "audio.h"   /* audio_play_test_tone */

static const char *TAG = "KTIMER";

static volatile int          s_remain;          /* >0: counting */
static esp_timer_handle_t    s_periodic;
static TaskHandle_t          s_beep_task;

static void tick_cb(void *arg)
{
    if (s_remain > 0) {
        s_remain--;
        if (s_remain > 0) {
            ui_timer_display(s_remain);
        } else {
            esp_timer_stop(s_periodic);
            ui_timer_display(UI_TIMER_DONE);
            if (s_beep_task) xTaskNotifyGive(s_beep_task);
        }
    }
}

static void beep_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Triple 880 Hz chirp — classic kitchen timer, run from OUR task:
         * audio_play_test_tone blocks until the ring drains. */
        for (int i = 0; i < 3; i++) {
            audio_play_test_tone(880, 180);
            vTaskDelay(pdMS_TO_TICKS(160));
        }
        ui_timer_display(UI_TIMER_DONE);
        /* Auto-clear the alert after 20 s unless a new timer started. */
        vTaskDelay(pdMS_TO_TICKS(20000));
        if (s_remain <= 0) ui_timer_display(UI_TIMER_HIDE);
    }
}

void timer_start(int seconds)
{
    if (seconds < 1) seconds = 1;
    if (seconds > 24 * 3600) seconds = 24 * 3600;
    if (!s_periodic) {
        const esp_timer_create_args_t a = { .callback = tick_cb, .name = "ktmr" };
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_periodic));
        xTaskCreate(beep_task, "ktimer", 3072, NULL, 5, &s_beep_task);
    }
    esp_timer_stop(s_periodic);           /* ignore "not running" */
    s_remain = seconds;
    ui_timer_display(seconds);
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_periodic, 1000000));
    ESP_LOGI(TAG, "Timer started: %d s", seconds);
}

void timer_cancel(void)
{
    s_remain = 0;
    if (s_periodic) esp_timer_stop(s_periodic);
    ui_timer_display(UI_TIMER_HIDE);
    ESP_LOGI(TAG, "Timer cancelled");
}

int timer_remaining(void)
{
    return (int)s_remain;
}
