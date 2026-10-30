#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize Wi-Fi and TCP subsystem.
 * Blocks until Wi-Fi is connected or returns ESP_ERR_TIMEOUT.
 */
esp_err_t net_init(void);

/** Start the net_stream_task FreeRTOS task */
void net_start_stream_task(void);

/** True while a session (upload or playback phase) is running. */
bool net_session_active(void);

/**
 * Ask the running session to be aborted (tap while listening/speaking).
 * Thread-safe - callable from the touch task. Takes effect within ~200 ms.
 */
void net_cancel_session(void);

#ifdef __cplusplus
}
#endif
