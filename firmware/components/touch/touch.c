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
#include "audio.h"  /* volume + beep feedback */
#include "ui.h"    /* on-screen volume flash */
#include "net.h"   /* tap cancels an active session */

static const char *TAG = "TOUCH";

/* CST816S — confirmed from Waveshare schematic */
#define TOUCH_I2C_NUM       I2C_NUM_0        /* shared with TCA9554/RTC/IMU */
#define TOUCH_I2C_ADDR      0x15
#define TOUCH_INT_GPIO      GPIO_NUM_4

/* CST816S data register map — matches Waveshare ESP-IDF demo read_data,
 * which reads DATA_LEN bytes from DATA_START_REG:
 * 0x02 num, 0x03 XH(4b), 0x04 XL, 0x05 YH(4b), 0x06 YL */
#define DATA_START_REG      0x02
#define DATA_LEN            5
#define REG_CHIP_ID         0xA7   /* product ID — Waveshare demo reads this at init */
#define REG_AUTOSLEEP       0xFE   /* 1 = auto-sleep disabled */
#define REG_GESTURE         0x01   /* native gesture: 1=UP 2=DOWN 3=L 4=R 5=click 0x0B=dbl 0x0C=long */
#define REG_TP_VERSION      0x15   /* touchpad version */
#define REG_PROJ_ID         0xA8   /* project ID */
#define REG_FW_VERSION      0xA9   /* firmware version */
#define REG_AUTOSLEEP_TIME  0xF9
#define REG_IRQ_CTL         0xFA
#define IRQ_EN_TOUCH        0x40
#define IRQ_EN_CHANGE       0x20

#define LONG_PRESS_MS       500
#define TOUCH_TASK_STACK    3072
#define TOUCH_TASK_PRIO     4
#define TOUCH_POLL_MS       100    /* polling fallback in case INT line misbehaves */

/* Swipe = release after vertical travel >= this many px (screen coords: +y down) */
#define SWIPE_PX            40
#define VOL_STEP            5   /* fine steps: double the increments */

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

/* ── TCA9554PWR (0x20, I2C0) — EXIO1 drives the CST816S RST line (active-low).
 *    EXIO2 (display RST) must stay HIGH — never toggle it here. ───────────── */
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUT     0x01
#define TCA9554_EXIO1_BIT   0x01
#define TCA9554_EXIO2_BIT   0x02   /* display RST — keep deasserted */

static void tca_set_exio1(bool level)
{
    uint8_t out = TCA9554_EXIO2_BIT | (level ? TCA9554_EXIO1_BIT : 0x00);
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, TCA9554_REG_OUT, true);
    i2c_master_write_byte(cmd, out, true);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(TOUCH_I2C_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
}

/* ── GPIO ISR ────────────────────────────────────────────────────────────── */

static void IRAM_ATTR touch_isr_handler(void *arg)
{
    uint32_t val = 1;
    xQueueSendFromISR(s_int_queue, &val, NULL);
}

/* ── Touch task ──────────────────────────────────────────────────────────── */

/* Short beep whose pitch encodes the new volume (300-1200 Hz).
 * Skipped while live TTS is queued in the playback ring — there the volume
 * change is audible in the speech itself, and the tone would be queued
 * behind (or clobber) it. */
static void volume_beep(void)
{
    if (audio_playback_available() > 256) return;
    int v = audio_get_volume();
    if (v <= 0) return;  /* at mute level there is nothing to hear anyway */
    audio_play_test_tone(300 + (uint32_t)v * 9, 100);
}

static void touch_task(void *arg)
{
    QueueHandle_t wake_q = wake_get_event_queue();
    uint32_t      irq_val;
    uint8_t       d[DATA_LEN];

    static int64_t s_down_ms    = 0;
    static int16_t s_down_x     = 0, s_down_y = 0;
    static int16_t s_last_x     = 0, s_last_y = 0;
    static bool    s_in_contact = false;
    static int     s_idle_polls = 0;

    ESP_LOGI(TAG, "Touch task started");

    while (1) {
        /* Block until INT fires; 100 ms timeout doubles as a polling
         * fallback if the INT line ever stops edge-triggering. */
        if (xQueueReceive(s_int_queue, &irq_val, pdMS_TO_TICKS(TOUCH_POLL_MS)) != pdTRUE) {
            /* queue timeout — poll below */
        }

        if (cst816_read(DATA_START_REG, d, sizeof(d)) != ESP_OK) {
            if (++s_idle_polls % 10 == 0) ESP_LOGW(TAG, "poll read FAILED (%d)", s_idle_polls);
            continue;
        }

        uint8_t num = d[0];
        if (num == 0xFF || num > 2) num = 0;   /* invalid / no touch */
        int16_t x = (int16_t)(((d[1] & 0x0F) << 8) | d[2]);
        int16_t y = (int16_t)(((d[3] & 0x0F) << 8) | d[4]);

        if (num > 0) {
            /* Finger down — track the contact; decide on release. */
            if (!s_in_contact) {
                s_in_contact = true;
                s_down_ms = esp_timer_get_time() / 1000;
                s_down_x  = x;
                s_down_y  = y;
                ESP_LOGI(TAG, "Down: x=%d y=%d", (int)x, (int)y);
            }
            s_last_x = x;
            s_last_y = y;
            continue;
        }

        if (!s_in_contact) {
            /* Idle: watch for a stuck-LOW INT line (stale pending event) */
            if (++s_idle_polls % 50 == 0 && gpio_get_level(TOUCH_INT_GPIO) == 0) {
                ESP_LOGW(TAG, "INT pin LOW while idle — clearing stale event");
                cst816_read(DATA_START_REG, d, sizeof(d));
            }
            continue;
        }

        /* Release — decide the gesture from travel/duration */
        s_in_contact = false;
        int64_t dur = esp_timer_get_time() / 1000 - s_down_ms;
        int16_t dy  = s_last_y - s_down_y;
        int16_t dx  = s_last_x - s_down_x;

        ESP_LOGI(TAG, "Release: dur=%ld ms dx=%d dy=%d", (long)dur, (int)dx, (int)dy);

        if (dy <= -SWIPE_PX) {
            audio_set_volume(audio_get_volume() + VOL_STEP);
            ESP_LOGI(TAG, "Swipe up — volume %d", audio_get_volume());
            ui_show_volume(audio_get_volume());
            volume_beep();

        } else if (dy >= SWIPE_PX) {
            audio_set_volume(audio_get_volume() - VOL_STEP);
            ESP_LOGI(TAG, "Swipe down — volume %d", audio_get_volume());
            ui_show_volume(audio_get_volume());
            volume_beep();

        } else if (dur >= LONG_PRESS_MS) {
            s_muted = !s_muted;
            ESP_LOGI(TAG, "Long press — mute %s", s_muted ? "ON" : "OFF");

        } else if (net_session_active()) {
            net_cancel_session();
            ESP_LOGI(TAG, "Tap - session cancelled");

        } else {
            /* Tap -> manual wake (same path as WakeNet) */
            wake_event_t evt = {
                .type         = WAKE_EVENT_WAKEWORD_DETECTED,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
            };
            if (wake_q) {
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

    /* 1. Reset (board demo sequence) */
    tca_set_exio1(false);
    vTaskDelay(pdMS_TO_TICKS(10));
    tca_set_exio1(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Read identification block (CST816_Read_cfg in the board demo) */
    uint8_t chip_id = 0, tp_ver = 0, proj_id = 0, fw_ver = 0;
    if (cst816_read(REG_CHIP_ID, &chip_id, 1) != ESP_OK) {
        ESP_LOGE(TAG, "CST816S chip ID read failed - touch will NOT work");
    } else {
        cst816_read(REG_TP_VERSION, &tp_ver, 1);
        cst816_read(REG_PROJ_ID, &proj_id, 1);
        cst816_read(REG_FW_VERSION, &fw_ver, 1);
        ESP_LOGI(TAG, "CST816: ChipID=0x%02x ProjID=0x%02x FwVer=0x%02x TPVer=0x%02x",
                 chip_id, proj_id, fw_ver, tp_ver);
    }

    /* 3. Board demo resets AGAIN before disabling auto-sleep, then writes
     *    the value 10 (not 1) to 0xFE */
    tca_set_exio1(false);
    vTaskDelay(pdMS_TO_TICKS(10));
    tca_set_exio1(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (cst816_write(REG_AUTOSLEEP, 10) != ESP_OK) {
        ESP_LOGW(TAG, "CST816S auto-sleep disable failed (continuing)");
    }

    /* 5. NO IRQ_CTL write — Waveshare demo does not write 0xFA. On the
     *    CST816S (different register map from CST816) 0xFA may control
     *    sensor scanning, and writing it may disable sensing. */

    /* 5. INT pin — falling edge interrupt (CST816S INT is active-low) */
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << TOUCH_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
    /* gpio_install_isr_service may already be called by another component */
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT_GPIO, touch_isr_handler, NULL));

    /* 6. Clear any stale touch data so the INT line starts idle-HIGH.
     *    A pending power-up event would otherwise hold INT low and no new
     *    falling edge would ever arrive. */
    uint8_t stale[DATA_LEN];
    cst816_read(DATA_START_REG, stale, sizeof(stale));
    vTaskDelay(pdMS_TO_TICKS(20));
    int lvl = gpio_get_level(TOUCH_INT_GPIO);
    ESP_LOGI(TAG, "INT pin idle level = %d (expect 1)", lvl);
    if (lvl == 0) {
        ESP_LOGW(TAG, "INT stuck LOW after clear — relying on poll fallback");
    }


    /* 8. Touch event task */
    xTaskCreatePinnedToCore(touch_task, "touch", TOUCH_TASK_STACK,
                            NULL, TOUCH_TASK_PRIO, NULL, 0);

    ESP_LOGI(TAG, "CST816S touch initialized (INT=GPIO%d)", TOUCH_INT_GPIO);
    return ESP_OK;
}

bool touch_is_muted(void)
{
    return s_muted;
}
