#include "ring_buffer.h"
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "RING_BUF";

esp_err_t ring_buf_init_cap(ring_buf_t *rb, size_t capacity_samples, uint32_t cap_mask)
{
    if (!rb || capacity_samples < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    rb->buf = (int16_t *)heap_caps_malloc(capacity_samples * sizeof(int16_t), cap_mask);
    if (!rb->buf) {
        ESP_LOGE(TAG, "heap_caps_malloc failed for %lu samples (mask=0x%04lX)",
                 (unsigned long)capacity_samples, (unsigned long)cap_mask);
        return ESP_ERR_NO_MEM;
    }
    rb->capacity = capacity_samples;
    rb->head = 0;
    rb->tail = 0;
    rb->mutex = xSemaphoreCreateMutex();
    rb->data_avail = xSemaphoreCreateCounting(capacity_samples, 0);
    /* Free-slot tokens: writers reserve them BEFORE writing, readers release
     * them after reading. data_avail + space_avail == capacity - 1 always.
     * Gives exact backpressure without any polling. */
    rb->space_avail = xSemaphoreCreateCounting(capacity_samples - 1, capacity_samples - 1);
    if (!rb->mutex || !rb->data_avail || !rb->space_avail) {
        if (rb->mutex) vSemaphoreDelete(rb->mutex);
        if (rb->data_avail) vSemaphoreDelete(rb->data_avail);
        if (rb->space_avail) vSemaphoreDelete(rb->space_avail);
        heap_caps_free(rb->buf);
        rb->buf = NULL; rb->mutex = NULL;
        rb->data_avail = NULL; rb->space_avail = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ring_buf_init(ring_buf_t *rb, size_t capacity_samples)
{
    return ring_buf_init_cap(rb, capacity_samples, MALLOC_CAP_INTERNAL);
}

void ring_buf_free(ring_buf_t *rb)
{
    heap_caps_free(rb->buf);
    vSemaphoreDelete(rb->mutex);
    vSemaphoreDelete(rb->data_avail);
    if (rb->space_avail) {
        vSemaphoreDelete(rb->space_avail);
        rb->space_avail = NULL;
    }
}

size_t ring_buf_write(ring_buf_t *rb, const int16_t *data, size_t samples)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t written = 0;
    for (size_t i = 0; i < samples; i++) {
        size_t next_head = (rb->head + 1) % rb->capacity;
        if (next_head == rb->tail) {
            /* Buffer full: overwrite oldest sample (drop tail). Fullness is
             * an index property — checked here under the mutex we already
             * hold. NOT a space_avail probe: that semaphore saturates at
             * max count when full (reader gives clamp), so take(0) succeeds
             * at full and lets head lap tail (build25 wake-word death).
             * Overwrite-mode writers never reserve space tokens. */
            rb->tail = (rb->tail + 1) % rb->capacity;
            /* The dropped sample was still counted available to readers —
             * undo that count. */
            xSemaphoreTake(rb->data_avail, 0);
        }
        rb->buf[rb->head] = data[i];
        rb->head = next_head;
        xSemaphoreGive(rb->data_avail);
        written++;
    }
    xSemaphoreGive(rb->mutex);
    return written;
}

/* Paced write: reserves free-slot tokens (blocking) BEFORE writing, so the
 * caller is throttled exactly by the reader's consumption rate. The task
 * sleeps on the semaphore with zero CPU while the ring drains — no polling,
 * no lost throughput. Used by TTS streaming (TCP backpressure); no audio is
 * discarded. Returns samples actually written; gives up after timeout_ms. */
size_t ring_buf_write_paced(ring_buf_t *rb, const int16_t *data, size_t samples,
                            uint32_t timeout_ms)
{
    size_t written = 0;
    TickType_t start = xTaskGetTickCount();
    while (written < samples) {
        uint32_t elapsed_ms = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        if (elapsed_ms >= timeout_ms) break;
        /* Reserve as many free slots as we can (blocking on the first). */
        size_t reserved = 0;
        while (reserved < samples - written) {
            TickType_t t = (reserved == 0) ? pdMS_TO_TICKS(timeout_ms - elapsed_ms) : 0;
            if (xSemaphoreTake(rb->space_avail, t) != pdTRUE) break;
            reserved++;
        }
        if (reserved == 0) break;
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
        for (size_t i = 0; i < reserved; i++) {
            rb->buf[rb->head] = data[written + i];
            rb->head = (rb->head + 1) % rb->capacity;
            xSemaphoreGive(rb->data_avail);
        }
        xSemaphoreGive(rb->mutex);
        written += reserved;
    }
    return written;
}

size_t ring_buf_read(ring_buf_t *rb, int16_t *data, size_t samples, TickType_t timeout)
{
    /* Grab availability tokens first (bounded wait on the first one), then
     * bulk-read under a SINGLE mutex hold. The previous per-sample take/give
     * made ~1000 critical sections per call; if the wake task was suspended
     * mid-read (wake_detection_suspend), the ring mutex was left owned by a
     * suspended task and every other ring user blocked on portMAX_DELAY
     * forever (device stuck on "Recording"). */
    size_t tokens = 0;
    while (tokens < samples) {
        if (xSemaphoreTake(rb->data_avail, tokens == 0 ? timeout : 0) != pdTRUE) {
            break;
        }
        tokens++;
    }
    if (tokens == 0) {
        return 0;
    }
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    for (size_t i = 0; i < tokens; i++) {
        data[i] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % rb->capacity;
    }
    xSemaphoreGive(rb->mutex);
    for (size_t i = 0; i < tokens; i++) {
        xSemaphoreGive(rb->space_avail);
    }
    return tokens;
}

size_t ring_buf_available(ring_buf_t *rb)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t avail = (rb->head - rb->tail + rb->capacity) % rb->capacity;
    xSemaphoreGive(rb->mutex);
    return avail;
}

void ring_buf_clear(ring_buf_t *rb)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    rb->head = rb->tail = 0;
    xSemaphoreGive(rb->mutex);
    /* Drain both semaphores, then restore the free-slot count to capacity-1 */
    while (xSemaphoreTake(rb->data_avail, 0) == pdTRUE) {}
    while (xSemaphoreTake(rb->space_avail, 0) == pdTRUE) {}
    for (size_t i = 0; i + 1 < rb->capacity; i++) {
        xSemaphoreGive(rb->space_avail);
    }
}
