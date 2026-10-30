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

/* Samples currently queued for playback */
size_t audio_playback_available(void);

/* Flush pending playback samples (blocks until drained) */
void audio_playback_flush(void);

/* Play a sine-wave test tone — blocks until samples are queued */
void audio_play_test_tone(uint32_t freq_hz, uint32_t duration_ms);

/* Speaker volume 0-100 — applied immediately, persisted to NVS */
void audio_set_volume(int percent);
int  audio_get_volume(void);

/* Real-time audio signal level for UI visualizations */
void     audio_set_ui_level(uint32_t level);
uint32_t audio_get_ui_level(void);

#ifdef __cplusplus
}
#endif
