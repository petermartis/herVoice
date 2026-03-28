#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_STATE_IDLE      = 0,
    UI_STATE_WAKE,
    UI_STATE_RECORDING,
    UI_STATE_SENDING,
    UI_STATE_PLAYING,
    UI_STATE_ERROR,
    UI_STATE_COUNT
} ui_state_t;

/**
 * Initialize display hardware and LVGL.
 * Must be called before any other ui_* function.
 */
esp_err_t ui_init(void);

/**
 * Transition to a new UI state.
 * Thread-safe — can be called from any FreeRTOS task.
 */
void ui_set_state(ui_state_t state);

/** Returns current state */
ui_state_t ui_get_state(void);

#ifdef __cplusplus
}
#endif
