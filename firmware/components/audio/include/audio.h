#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize I2S microphone and speaker hardware */
esp_err_t audio_init(void);

/* Start the background audio capture task */
void audio_start_capture_task(void);

/**
 * Read captured PCM samples into buf.
 * Blocks until 'samples' frames available or timeout_ticks elapses.
 * Returns actual number of samples copied (may be < requested on timeout).
 */
size_t audio_get_frames(int16_t *buf, size_t samples, TickType_t timeout_ticks);

/**
 * Write PCM samples to the playback ring buffer.
 * Non-blocking; drops frames if playback buffer is full.
 */
void audio_play_frames(const int16_t *buf, size_t samples);

/* Flush pending playback samples (blocks until drained) */
void audio_playback_flush(void);

#ifdef __cplusplus
}
#endif
