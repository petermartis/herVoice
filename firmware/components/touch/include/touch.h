#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize CST816 touch controller and start touch event task.
 * Must be called after I2C bus is already initialized (ui_init() does this).
 */
esp_err_t touch_init(void);

/** Returns true if microphone is muted (toggled by long-press). */
bool touch_is_muted(void);

#ifdef __cplusplus
}
#endif
