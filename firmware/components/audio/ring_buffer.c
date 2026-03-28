#include "ring_buffer.h"
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "RING_BUF";

esp_err_t ring_buf_init(ring_buf_t *rb, size_t capacity_samples)
{
    rb->buf = (int16_t *)malloc(capacity_samples * sizeof(int16_t));
    if (!rb->buf) {
        ESP_LOGE(TAG, "malloc failed for %zu samples", capacity_samples);
        return ESP_ERR_NO_MEM;
    }
    rb->capacity = capacity_samples;
    rb->head = 0;
    rb->tail = 0;
    rb->mutex = xSemaphoreCreateMutex();
    rb->data_avail = xSemaphoreCreateCounting(capacity_samples, 0);
    if (!rb->mutex || !rb->data_avail) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ring_buf_free(ring_buf_t *rb)
{
    free(rb->buf);
    vSemaphoreDelete(rb->mutex);
    vSemaphoreDelete(rb->data_avail);
}

size_t ring_buf_write(ring_buf_t *rb, const int16_t *data, size_t samples)
{
    xSemaphoreTake(rb->mutex, portMAX_DELAY);
    size_t written = 0;
    for (size_t i = 0; i < samples; i++) {
        size_t next_head = (rb->head + 1) % rb->capacity;
        if (next_head == rb->tail) {
            /* Buffer full: overwrite oldest sample (drop tail) */
            rb->tail = (rb->tail + 1) % rb->capacity;
            /* Adjust semaphore count - take one away since we dropped */
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

size_t ring_buf_read(ring_buf_t *rb, int16_t *data, size_t samples, TickType_t timeout)
{
    size_t read = 0;
    for (size_t i = 0; i < samples; i++) {
        if (xSemaphoreTake(rb->data_avail, (i == 0) ? timeout : 0) != pdTRUE) {
            break;
        }
        xSemaphoreTake(rb->mutex, portMAX_DELAY);
        data[read++] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % rb->capacity;
        xSemaphoreGive(rb->mutex);
    }
    return read;
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
    /* Drain semaphore */
    while (xSemaphoreTake(rb->data_avail, 0) == pdTRUE) {}
    xSemaphoreGive(rb->mutex);
}
