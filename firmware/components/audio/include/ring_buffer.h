#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t         *buf;
    size_t           capacity;   /* in samples */
    volatile size_t  head;       /* write pos */
    volatile size_t  tail;       /* read pos */
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t data_avail; /* counting semaphore */
} ring_buf_t;

esp_err_t ring_buf_init(ring_buf_t *rb, size_t capacity_samples);
void      ring_buf_free(ring_buf_t *rb);
size_t    ring_buf_write(ring_buf_t *rb, const int16_t *data, size_t samples);
size_t    ring_buf_read(ring_buf_t *rb, int16_t *data, size_t samples, TickType_t timeout);
size_t    ring_buf_available(ring_buf_t *rb);
void      ring_buf_clear(ring_buf_t *rb);

#ifdef __cplusplus
}
#endif
