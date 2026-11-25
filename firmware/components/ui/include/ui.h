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

/**
 * Briefly show "Volume X" on the face (1.2 s) then restore the normal
 * state text. Thread-safe - called from the touch task on volume swipes.
 */
void ui_show_volume(int vol);

#ifdef __cplusplus
}
#endif

/* Kitchen-timer countdown overlay, shown above the state label (bottom of
 * the screen) in every state. Thread-safe: just stores the value; the LVGL
 * task renders it. Pseudo values: UI_TIMER_HIDE, UI_TIMER_DONE. */
#define UI_TIMER_HIDE (-1)
#define UI_TIMER_DONE (-2)
void ui_timer_display(int seconds);
