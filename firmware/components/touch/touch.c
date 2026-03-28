#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "touch.h"
#include "wake.h"   /* for wake_get_event_queue() and wake_event_t */

static const char *TAG = "TOUCH";

/* CST816S — confirmed from Waveshare schematic */
#define TOUCH_I2C_NUM       I2C_NUM_0        /* shared with TCA9554/RTC/IMU */
#define TOUCH_I2C_ADDR      0x15
#define TOUCH_INT_GPIO      GPIO_NUM_4

/* CST816S register map */
#define REG_GESTURE         0x01
#define REG_FINGER_NUM      0x02
#define REG_XPOS_H          0x03
#define REG_XPOS_L          0x04
#define REG_YPOS_H          0x05
#define REG_YPOS_L          0x06
#define REG_BPC0H           0xB0
#define REG_MOTION_MASK     0xEC
#define REG_IRQ_CTL         0xFA
#define IRQ_EN_TOUCH        0x40
#define IRQ_EN_CHANGE       0x20

/* Gesture codes */
#define GESTURE_NONE        0x00
#define GESTURE_SWIPE_UP    0x01
#define GESTURE_SWIPE_DOWN  0x02
#define GESTURE_SWIPE_LEFT  0x03
#define GESTURE_SWIPE_RIGHT 0x04
#define GESTURE_CLICK       0x05
#define GESTURE_DOUBLE_CLICK 0x0B
#define GESTURE_LONG_PRESS  0x0C

#define LONG_PRESS_MS       500
#define TOUCH_TASK_STACK    3072
#define TOUCH_TASK_PRIO     4

static volatile bool        s_muted         = false;
static QueueHandle_t        s_int_queue     = NULL;  /* ISR -> task notification */

/* ── I2C helpers ─────────────────────────────────────────────────────────── */

static esp_err_t cst816_read(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1)
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t cst816_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TOUCH_I2C_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* ── GPIO ISR ────────────────────────────────────────────────────────────── */

static void IRAM_ATTR touch_isr_handler(void *arg)
{
    uint32_t val = 1;
    xQueueSendFromISR(s_int_queue, &val, NULL);
}

/* ── Touch task ──────────────────────────────────────────────────────────── */

static void touch_task(void *arg)
{
    QueueHandle_t wake_q = wake_get_event_queue();
    uint32_t      irq_val;
    uint8_t       touch_data[6];

    ESP_LOGI(TAG, "Touch task started");

    while (1) {
        /* Block until INT fires */
        if (xQueueReceive(s_int_queue, &irq_val, portMAX_DELAY) != pdTRUE) continue;

        /* Read gesture + position registers */
        if (cst816_read(REG_GESTURE, touch_data, sizeof(touch_data)) != ESP_OK) {
            ESP_LOGW(TAG, "I2C read failed");
            continue;
        }

        uint8_t gesture    = touch_data[0];
        uint8_t finger_num = touch_data[1];

        if (finger_num == 0) continue;  /* Touch released, no gesture */

        ESP_LOGD(TAG, "Gesture=0x%02x fingers=%d", gesture, finger_num);

        if (gesture == GESTURE_LONG_PRESS) {
            s_muted = !s_muted;
            ESP_LOGI(TAG, "Long press — mute %s", s_muted ? "ON" : "OFF");

        } else if (gesture == GESTURE_CLICK || gesture == GESTURE_NONE) {
            /* Single tap -> manual wake trigger */
            if (wake_q) {
                wake_event_t evt = {
                    .type         = WAKE_EVENT_WAKEWORD_DETECTED,
                    .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
                };
                if (xQueueSend(wake_q, &evt, pdMS_TO_TICKS(50)) != pdTRUE) {
                    ESP_LOGW(TAG, "Wake queue full, tap dropped");
                } else {
                    ESP_LOGI(TAG, "Tap — manual wake triggered");
                }
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t touch_init(void)
{
    /* ISR notification queue */
    s_int_queue = xQueueCreate(4, sizeof(uint32_t));
    if (!s_int_queue) return ESP_ERR_NO_MEM;

    /* Configure CST816S interrupt output mode via IRQ_CTL register */
    esp_err_t ret = cst816_write(REG_IRQ_CTL, IRQ_EN_TOUCH | IRQ_EN_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cst816 IRQ_CTL write failed: %s (continuing)", esp_err_to_name(ret));
        /* Non-fatal — chip may still raise INT on touch */
    }

    /* INT pin — falling edge interrupt */
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    /* gpio_install_isr_service may already be called by another component — ignore that */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT_GPIO, touch_isr_handler, NULL));

    /* Touch event task */
    xTaskCreatePinnedToCore(touch_task, "touch", TOUCH_TASK_STACK,
                            NULL, TOUCH_TASK_PRIO, NULL, 0);

    ESP_LOGI(TAG, "CST816S touch initialized (INT=GPIO%d)", TOUCH_INT_GPIO);
    return ESP_OK;
}

bool touch_is_muted(void)
{
    return s_muted;
}
