#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "ui.h"

static const char *TAG = "UI";

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85C — confirmed from schematic + wiki:
 * Display: ST77916, QSPI interface (4 data lines, no DC pin, 32-bit cmd format)
 *
 * LCD_DATA0 (SDA0/MOSI) = GPIO46
 * LCD_DATA1 (SDA1)      = GPIO45
 * LCD_DATA2 (SDA2)      = GPIO42
 * LCD_DATA3 (SDA3)      = GPIO41
 * LCD_CLK               = GPIO40
 * LCD_CS                = GPIO21
 * LCD_TE  (tear effect)  = GPIO18
 * LCD_RST               = via TCA9554 EXIO2 (I2C GPIO expander, not a direct GPIO → pass -1)
 * LCD_BL  (backlight)   = GPIO5
 */
#define LCD_HOST        SPI2_HOST
#define LCD_DATA0       GPIO_NUM_46
#define LCD_DATA1       GPIO_NUM_45
#define LCD_DATA2       GPIO_NUM_42
#define LCD_DATA3       GPIO_NUM_41
#define LCD_CLK         GPIO_NUM_40
#define LCD_CS          GPIO_NUM_21
#define LCD_TE          GPIO_NUM_18
#define LCD_BL          GPIO_NUM_5
/* LCD_RST is controlled via TCA9554 I2C expander EXIO2; use -1 for direct GPIO */
#define LCD_RST         (-1)
#define LCD_H_RES       360
#define LCD_V_RES       360
#define LCD_BUF_LINES   40

#define LVGL_TASK_STACK  6144
#define LVGL_TASK_PRIO   3
#define LVGL_TICK_MS     2

static lv_disp_t         *s_disp          = NULL;
static lv_obj_t          *s_screen        = NULL;
static lv_obj_t          *s_state_label   = NULL;
static lv_obj_t          *s_sub_label     = NULL;
static SemaphoreHandle_t  s_lvgl_mutex    = NULL;
static volatile ui_state_t s_current_state = UI_STATE_IDLE;
static volatile ui_state_t s_pending_state = UI_STATE_IDLE;
static volatile bool       s_state_changed = false;

static esp_lcd_panel_handle_t s_panel = NULL;

/* ---------- LCD flush callback ---------- */
static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              (void *)color_p);
    lv_disp_flush_ready(drv);
}

/* ---------- LVGL tick timer ---------- */
static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

/* ---------- UI update (called from LVGL task) ---------- */
static const char *state_labels[] = {
    "Listening...",
    "Wake!",
    "Recording...",
    "Processing...",
    "Speaking...",
    "Error",
};

static lv_color_t state_colors[] = {
    /* IDLE */      { .full = 0x1F51 },   /* dark green-ish */
    /* WAKE */      { .full = 0xFFE0 },   /* yellow */
    /* RECORDING */ { .full = 0xF800 },   /* red */
    /* SENDING */   { .full = 0x001F },   /* blue */
    /* PLAYING */   { .full = 0x07FF },   /* cyan */
    /* ERROR */     { .full = 0xF810 },   /* bright red */
};

static void apply_state(ui_state_t state)
{
    if (!s_screen || !s_state_label) return;

    lv_obj_set_style_bg_color(s_screen,
                              lv_color_make(
                                  (state_colors[state].full >> 11) & 0x1F,
                                  (state_colors[state].full >> 5) & 0x3F,
                                  state_colors[state].full & 0x1F),
                              LV_PART_MAIN);
    lv_label_set_text(s_state_label, state_labels[state]);
    s_current_state = state;
}

/* ---------- LVGL handler task ---------- */
static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    while (1) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (s_state_changed) {
                apply_state(s_pending_state);
                s_state_changed = false;
            }
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------- public API ---------- */

esp_err_t ui_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) return ESP_ERR_NO_MEM;

    /*
     * QSPI SPI bus — ST77916 uses 4 data lines.
     * mosi_io_num  = DATA0
     * miso_io_num  = DATA1  (output in QSPI write mode)
     * quadwp_io_num = DATA2
     * quadhd_io_num = DATA3
     */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_DATA0,
        .miso_io_num   = LCD_DATA1,
        .sclk_io_num   = LCD_CLK,
        .quadwp_io_num = LCD_DATA2,
        .quadhd_io_num = LCD_DATA3,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /*
     * ST77916 QSPI panel IO.
     * No DC pin (dc_gpio_num = -1); command/address packed in 32-bit opcode.
     * lcd_cmd_bits = 32 per ST77916 QSPI protocol.
     * flags.quad_mode = 1 to enable quad-SPI writes.
     */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = -1,
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = 80 * 1000 * 1000,
        .lcd_cmd_bits      = 32,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        .flags = {
            .quad_mode = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_cfg, &io_handle));

    /*
     * ST77916 panel driver.
     * NOTE: ESP-IDF has no built-in ST77916 driver. Add the Waveshare component
     * (or espressif/esp_lcd_st77916 from component registry) to idf_component.yml
     * and replace esp_lcd_new_panel_st7789 with esp_lcd_new_panel_st77916.
     * The init sequence differs; this ST7789 call is a compile placeholder.
     */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = LCD_RST,   /* -1: reset via TCA9554 EXIO2 separately */
        .rgb_endian      = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* TE (tearing effect) pin — configure as input for optional vsync sync */
    gpio_set_direction(LCD_TE, GPIO_MODE_INPUT);

    /* Backlight */
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);

    /* LVGL init */
    lv_init();

    /* Draw buffer */
    static lv_color_t draw_buf1[LCD_H_RES * LCD_BUF_LINES];
    static lv_disp_draw_buf_t draw_buf_desc;
    lv_disp_draw_buf_init(&draw_buf_desc, draw_buf1, NULL,
                          LCD_H_RES * LCD_BUF_LINES);

    /* Display driver */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_H_RES;
    disp_drv.ver_res    = LCD_V_RES;
    disp_drv.flush_cb   = lcd_flush_cb;
    disp_drv.draw_buf   = &draw_buf_desc;
    s_disp = lv_disp_drv_register(&disp_drv);

    /* Create screen */
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);

    /* State label (centered) */
    s_state_label = lv_label_create(s_screen);
    lv_label_set_text(s_state_label, "Initializing...");
    lv_obj_set_style_text_color(s_state_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 0);

    /* Sub label */
    s_sub_label = lv_label_create(s_screen);
    lv_label_set_text(s_sub_label, "herVoice");
    lv_obj_set_style_text_color(s_sub_label, lv_color_make(180, 180, 180), LV_PART_MAIN);
    lv_obj_align(s_sub_label, LV_ALIGN_CENTER, 0, 30);

    /* LVGL tick timer (2ms) */
    esp_timer_handle_t tick_timer;
    esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    /* LVGL handler task */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK,
                            NULL, LVGL_TASK_PRIO, NULL, 0);

    ESP_LOGI(TAG, "UI initialized (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void ui_set_state(ui_state_t state)
{
    if (state >= UI_STATE_COUNT) return;
    s_pending_state = state;
    s_state_changed = true;
    ESP_LOGD(TAG, "State -> %s", state_labels[state]);
}

ui_state_t ui_get_state(void)
{
    return s_current_state;
}
