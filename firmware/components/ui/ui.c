/*
 * herVoice — UI component
 * ST77916 QSPI 360x360 display on Waveshare ESP32-S3-Touch-LCD-1.85C
 *
 * Pin assignments (confirmed from Waveshare schematic + wiki):
 *   DATA0=GPIO46, DATA1=GPIO45, DATA2=GPIO42, DATA3=GPIO41
 *   CLK=GPIO40, CS=GPIO21, TE=GPIO18, BL=GPIO5
 *   RST via TCA9554PWR I2C GPIO expander (EXIO2, addr 0x20, bus GPIO10/11)
 */

#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_st77916.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "UI";

/* ── LCD QSPI pins (Waveshare schematic confirmed) ─────────────────────────── */
#define LCD_HOST        SPI2_HOST
#define LCD_DATA0       GPIO_NUM_46
#define LCD_DATA1       GPIO_NUM_45
#define LCD_DATA2       GPIO_NUM_42
#define LCD_DATA3       GPIO_NUM_41
#define LCD_CLK         GPIO_NUM_40
#define LCD_CS          GPIO_NUM_21
#define LCD_TE          GPIO_NUM_18
#define LCD_BL          GPIO_NUM_5
/* RST is via TCA9554PWR EXIO2 — no direct GPIO */

/* ── TCA9554PWR I2C GPIO expander ───────────────────────────────────────────── */
#define TCA9554_I2C_NUM     I2C_NUM_0
#define TCA9554_I2C_SDA     GPIO_NUM_11
#define TCA9554_I2C_SCL     GPIO_NUM_10
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#define TCA9554_EXIO2_BIT   (1 << 1)  /* P1 = EXIO2; Waveshare: (1<<(pin-1)) with pin=2 */

/* ── LCD geometry ───────────────────────────────────────────────────────────── */
#define LCD_H_RES       360
#define LCD_V_RES       360
#define LCD_COLOR_BITS  16
#define LVGL_BUF_LEN    (LCD_H_RES * LCD_V_RES / 20)  /* 6480 px — matches Waveshare reference */

/* ── Backlight (LEDC PWM) ───────────────────────────────────────────────────── */
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_RES         LEDC_TIMER_13_BIT
#define BL_LEDC_MAX_DUTY    ((1 << BL_LEDC_RES) - 1)

/* ── LVGL task ──────────────────────────────────────────────────────────────── */
#define LVGL_TASK_STACK  6144
#define LVGL_TASK_PRIO   3
#define LVGL_TICK_MS     2

/* ── ST77916 vendor init (Waveshare V2: 0x28 unlock, full GOA, VCOM set) ───── */
static const st77916_lcd_init_cmd_t s_vendor_init[] = {
    /* Page 0x28 unlock */
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    /* Page 0x01 — power/timing */
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x49}, 1, 0},
    {0xB1, (uint8_t[]){0x4A}, 1, 0},
    {0xB2, (uint8_t[]){0x1F}, 1, 0},
    {0xB4, (uint8_t[]){0x46}, 1, 0},
    {0xB5, (uint8_t[]){0x34}, 1, 0},
    {0xB6, (uint8_t[]){0xD5}, 1, 0},
    {0xB7, (uint8_t[]){0x30}, 1, 0},
    {0xB8, (uint8_t[]){0x04}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x51}, 1, 0},  /* 0x01→0x51: match default charge pump level */
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x51}, 1, 0},  /* 0x01→0x51: match default charge pump level */
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    /* Page 0x02 — gamma */
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0x70,0x09,0x12,0x0C,0x0B,0x27,0x38,0x54,0x4E,0x19,0x15,0x15,0x2C,0x2F}, 14, 0},
    {0xE1, (uint8_t[]){0x70,0x08,0x11,0x0C,0x0B,0x27,0x38,0x43,0x4C,0x18,0x14,0x14,0x2B,0x2D}, 14, 0},
    /* Page 0x10 — GOA timing + signal mapping */
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x0A}, 1, 0},  /* 0x08→0x0A: GOA timing (match component default) */
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x0B}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x00}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x82}, 1, 0},
    {0xEA, (uint8_t[]){0xDF}, 1, 0},
    {0xEB, (uint8_t[]){0x89}, 1, 0},
    {0xEC, (uint8_t[]){0x20}, 1, 0},
    {0xED, (uint8_t[]){0x14}, 1, 0},
    {0xEE, (uint8_t[]){0xFF}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0xFF}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x30}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x42}, 1, 0},
    {0x61, (uint8_t[]){0xE0}, 1, 0},
    {0x62, (uint8_t[]){0x40}, 1, 0},
    {0x63, (uint8_t[]){0x40}, 1, 0},
    {0x64, (uint8_t[]){0x02}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x40}, 1, 0},
    {0x67, (uint8_t[]){0x03}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x42}, 1, 0},
    {0x71, (uint8_t[]){0xE0}, 1, 0},
    {0x72, (uint8_t[]){0x40}, 1, 0},
    {0x73, (uint8_t[]){0x40}, 1, 0},
    {0x74, (uint8_t[]){0x02}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x40}, 1, 0},
    {0x77, (uint8_t[]){0x03}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x38}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x04}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xDC}, 1, 0},
    {0x85, (uint8_t[]){0x00}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x38}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x06}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xDE}, 1, 0},
    {0x8D, (uint8_t[]){0x00}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x38}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x08}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xE0}, 1, 0},
    {0x95, (uint8_t[]){0x00}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x38}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0A}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xE2}, 1, 0},
    {0x9D, (uint8_t[]){0x00}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x38}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x03}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xDB}, 1, 0},
    {0xA5, (uint8_t[]){0x00}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x38}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x05}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xDD}, 1, 0},
    {0xAD, (uint8_t[]){0x00}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x38}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xDF}, 1, 0},
    {0xB5, (uint8_t[]){0x00}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x38}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x09}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xE1}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x22}, 1, 0},
    {0xC1, (uint8_t[]){0xAA}, 1, 0},
    {0xC2, (uint8_t[]){0x65}, 1, 0},
    {0xC3, (uint8_t[]){0x74}, 1, 0},
    {0xC4, (uint8_t[]){0x47}, 1, 0},
    {0xC5, (uint8_t[]){0x56}, 1, 0},
    {0xC6, (uint8_t[]){0x00}, 1, 0},
    {0xC7, (uint8_t[]){0x88}, 1, 0},
    {0xC8, (uint8_t[]){0x99}, 1, 0},
    {0xC9, (uint8_t[]){0x33}, 1, 0},
    {0xD0, (uint8_t[]){0x11}, 1, 0},
    {0xD1, (uint8_t[]){0xAA}, 1, 0},
    {0xD2, (uint8_t[]){0x65}, 1, 0},
    {0xD3, (uint8_t[]){0x74}, 1, 0},
    {0xD4, (uint8_t[]){0x47}, 1, 0},
    {0xD5, (uint8_t[]){0x56}, 1, 0},
    {0xD6, (uint8_t[]){0x00}, 1, 0},
    {0xD7, (uint8_t[]){0x88}, 1, 0},
    {0xD8, (uint8_t[]){0x99}, 1, 0},
    {0xD9, (uint8_t[]){0x33}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    /* INVON (0x21) omitted — panel is normally-black; INVON would invert all colours */
    {0x11, (uint8_t[]){0x00}, 1, 120},  /* SLPOUT */
};

/* ── Module state ───────────────────────────────────────────────────────────── */
static esp_lcd_panel_io_handle_t s_io_handle   = NULL;
static esp_lcd_panel_handle_t  s_panel         = NULL;
static lv_disp_t              *s_disp          = NULL;
static lv_obj_t               *s_screen        = NULL;
static lv_obj_t               *s_state_label   = NULL;
static lv_obj_t               *s_sub_label     = NULL;
static SemaphoreHandle_t       s_lvgl_mutex    = NULL;
static volatile ui_state_t     s_current_state = UI_STATE_IDLE;
static volatile ui_state_t     s_pending_state = UI_STATE_IDLE;
static volatile bool           s_state_changed = false;

static lv_disp_drv_t           s_disp_drv;
static lv_disp_draw_buf_t      s_draw_buf_desc;
/* Two PSRAM buffers — double-buffering lets flush_ready be called synchronously (Waveshare pattern) */
static lv_color_t             *s_draw_buf1     = NULL;
static lv_color_t             *s_draw_buf2     = NULL;

/* ── TCA9554 helper ─────────────────────────────────────────────────────────── */

static esp_err_t tca9554_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TCA9554_I2C_SDA,
        .scl_io_num       = TCA9554_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t ret = i2c_param_config(TCA9554_I2C_NUM, &conf);
    if (ret != ESP_OK) return ret;
    ret = i2c_driver_install(TCA9554_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* Already installed by another component — not an error */
        ret = ESP_OK;
    }
    return ret;
}

static esp_err_t tca9554_set_exio2(bool level)
{
    /* Set EXIO2 (bit 2) as output in config register, then write level */
    uint8_t buf[2];

    /* Config register: 0 = output, read-modify-write */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    buf[0] = TCA9554_REG_CONFIG;
    buf[1] = 0x00; /* all outputs — safe since we own this bus init */
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TCA9554_I2C_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;

    /* Output register */
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TCA9554_ADDR << 1) | I2C_MASTER_WRITE, true);
    buf[0] = TCA9554_REG_OUTPUT;
    buf[1] = level ? TCA9554_EXIO2_BIT : 0x00;
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(TCA9554_I2C_NUM, cmd, pdMS_TO_TICKS(10));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void lcd_hardware_reset(void)
{
    esp_err_t r;
    r = tca9554_set_exio2(false);
    ESP_LOGI(TAG, "DBG rst_assert  ret=%s  bit=0x%02x", esp_err_to_name(r), TCA9554_EXIO2_BIT);
    vTaskDelay(pdMS_TO_TICKS(10));
    r = tca9554_set_exio2(true);
    ESP_LOGI(TAG, "DBG rst_deassert ret=%s", esp_err_to_name(r));
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ── Backlight (LEDC PWM) ───────────────────────────────────────────────────── */

static void backlight_init(uint8_t brightness_percent)
{
    ledc_timer_config_t timer = {
        .duty_resolution = BL_LEDC_RES,
        .freq_hz         = 5000,
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .channel    = BL_LEDC_CHANNEL,
        .duty       = 0,
        .gpio_num   = LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .timer_sel  = BL_LEDC_TIMER,
    };
    ledc_channel_config(&ch);
    ledc_fade_func_install(0);

    if (brightness_percent > 100) brightness_percent = 100;
    uint32_t duty = (BL_LEDC_MAX_DUTY * brightness_percent) / 100;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

/* ── LCD flush callback ─────────────────────────────────────────────────────── */

static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    static int s_flush_n = 0;
    s_flush_n++;
    if (s_flush_n <= 25) {  /* log first 25 flushes */
        const uint16_t *p = (const uint16_t *)color_p;
        ESP_LOGI(TAG, "DBG flush#%02d (%d,%d)-(%d,%d) px[0..3]=%04x %04x %04x %04x",
                 s_flush_n, area->x1, area->y1, area->x2, area->y2,
                 p[0], p[1], p[2], p[3]);
    }
    esp_err_t r = esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              (void *)color_p);
    if (r != ESP_OK) ESP_LOGE(TAG, "DBG draw_bitmap err: %s", esp_err_to_name(r));
    lv_disp_flush_ready(drv);
}

/* ── LVGL tick callback ─────────────────────────────────────────────────────── */

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

/* ── State rendering ────────────────────────────────────────────────────────── */

static const char *s_state_labels[UI_STATE_COUNT] = {
    "Listening...",
    "Wake!",
    "Recording...",
    "Processing...",
    "Speaking...",
    "Error",
};

/* RGB888 background colors per state */
static const struct { uint8_t r, g, b; } s_state_colors[UI_STATE_COUNT] = {
    {  0,  80,  0  },  /* IDLE     — dark green  */
    {180, 160,  0  },  /* WAKE     — amber       */
    {160,   0,  0  },  /* RECORDING— dark red    */
    {  0,   0, 140 },  /* SENDING  — blue        */
    {  0, 120, 120 },  /* PLAYING  — teal        */
    {200,   0,  0  },  /* ERROR    — bright red  */
};

static void apply_state(ui_state_t state)
{
    if (!s_screen || !s_state_label) return;
    lv_obj_set_style_bg_color(s_screen,
        lv_color_make(s_state_colors[state].r,
                      s_state_colors[state].g,
                      s_state_colors[state].b),
        LV_PART_MAIN);
    lv_label_set_text(s_state_label, s_state_labels[state]);
    s_current_state = state;
}

/* ── LVGL handler task ──────────────────────────────────────────────────────── */

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    int tick = 0;
    while (1) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (s_state_changed) {
                apply_state(s_pending_state);
                s_state_changed = false;
            }
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        tick++;
        if (tick <= 5) ESP_LOGI(TAG, "DBG lvgl_task tick %d", tick);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

esp_err_t ui_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) return ESP_ERR_NO_MEM;

    /* 1. I2C bus for TCA9554 + reset */
    ESP_ERROR_CHECK(tca9554_init());
    lcd_hardware_reset();

    /* 2. QSPI bus — use data0/1/2/3_io_num (not mosi/miso/quadwp/quadhd) */
    spi_bus_config_t buscfg = {
        .data0_io_num    = LCD_DATA0,
        .data1_io_num    = LCD_DATA1,
        .sclk_io_num     = LCD_CLK,
        .data2_io_num    = LCD_DATA2,
        .data3_io_num    = LCD_DATA3,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
        .max_transfer_sz = LVGL_BUF_LEN * LCD_COLOR_BITS / 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 3. Panel IO — QSPI, 32-bit cmd, no DC pin */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = LCD_CS,
        .dc_gpio_num         = -1,
        .spi_mode            = 0,
        .pclk_hz             = 40 * 1000 * 1000,
        .trans_queue_depth   = 10,
        .on_color_trans_done = NULL,
        .user_ctx            = NULL,
        .lcd_cmd_bits        = 32,
        .lcd_param_bits      = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io_handle));

    /* 4. Waveshare V2 vendor init (0x28 unlock key, full GOA, VCOM set) */
    st77916_vendor_config_t vendor_cfg = {
        .init_cmds      = s_vendor_init,
        .init_cmds_size = sizeof(s_vendor_init) / sizeof(s_vendor_init[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,   /* Reset done via TCA9554 above */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_COLOR_BITS,
        .vendor_config  = &vendor_cfg,
    };
    esp_err_t panel_ret;
    panel_ret = esp_lcd_new_panel_st77916(s_io_handle, &panel_cfg, &s_panel);
    ESP_LOGI(TAG, "DBG new_panel:   %s", esp_err_to_name(panel_ret));
    ESP_ERROR_CHECK(panel_ret);

    panel_ret = esp_lcd_panel_reset(s_panel);
    ESP_LOGI(TAG, "DBG panel_reset: %s", esp_err_to_name(panel_ret));
    ESP_ERROR_CHECK(panel_ret);

    panel_ret = esp_lcd_panel_init(s_panel);
    ESP_LOGI(TAG, "DBG panel_init:  %s", esp_err_to_name(panel_ret));
    ESP_ERROR_CHECK(panel_ret);

    panel_ret = esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "DBG disp_on:     %s", esp_err_to_name(panel_ret));
    ESP_ERROR_CHECK(panel_ret);

    /* DBG: fill entire GRAM with 0x0000 (black on NB panel, white on NW panel) for 2s.
     * Tells us: (a) GRAM writes work, (b) panel polarity without INVON. */
    {
        static uint16_t fill_line[LCD_H_RES];
        memset(fill_line, 0x00, sizeof(fill_line));
        for (int y = 0; y < LCD_V_RES; y++) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, fill_line);
        }
        ESP_LOGI(TAG, "DBG 0x0000 fill done — 2s hold (NB=black, NW=white)");
        vTaskDelay(pdMS_TO_TICKS(2000));
        memset(fill_line, 0xFF, sizeof(fill_line));
        for (int y = 0; y < LCD_V_RES; y++) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, fill_line);
        }
        ESP_LOGI(TAG, "DBG 0xFFFF fill done — 2s hold (NB=white, NW=black)");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* 5. TE pin (tearing effect sync input) */
    gpio_set_direction(LCD_TE, GPIO_MODE_INPUT);

    /* 6. Backlight at 80% via LEDC PWM */
    backlight_init(80);

    /* 7. LVGL */
    lv_init();

    /* Allocate from internal DMA-capable DRAM — PSRAM buffers are not SPI-DMA safe in IDF 5.3 */
    s_draw_buf1 = heap_caps_malloc(LVGL_BUF_LEN * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_draw_buf2 = heap_caps_malloc(LVGL_BUF_LEN * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_draw_buf1 || !s_draw_buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers in internal DRAM");
        return ESP_ERR_NO_MEM;
    }
    memset(s_draw_buf1, 0, LVGL_BUF_LEN * sizeof(lv_color_t));
    memset(s_draw_buf2, 0, LVGL_BUF_LEN * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&s_draw_buf_desc, s_draw_buf1, s_draw_buf2, LVGL_BUF_LEN);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = LCD_H_RES;
    s_disp_drv.ver_res  = LCD_V_RES;
    s_disp_drv.flush_cb = lcd_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf_desc;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    s_screen = lv_scr_act();
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);

    s_state_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_state_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, -15);

    s_sub_label = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_sub_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(s_sub_label, LV_ALIGN_CENTER, 0, 20);

    /* Render IDLE state immediately */
    apply_state(UI_STATE_IDLE);

    /* 8. LVGL tick timer */
    esp_timer_handle_t tick_timer;
    esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    /* 9. LVGL handler task */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK,
                            NULL, LVGL_TASK_PRIO, NULL, 0);

    ESP_LOGI(TAG, "UI initialized (%dx%d, ST77916 QSPI)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void ui_set_state(ui_state_t state)
{
    if (state >= UI_STATE_COUNT) return;
    s_pending_state = state;
    s_state_changed = true;
    ESP_LOGD(TAG, "State -> %s", s_state_labels[state]);
}

ui_state_t ui_get_state(void)
{
    return s_current_state;
}
