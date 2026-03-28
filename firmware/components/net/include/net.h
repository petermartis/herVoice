#pragma once
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

#ifdef __cplusplus
}
#endif
