#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame types */
#define FRAME_SESSION_START   0x00
#define FRAME_PCM_UP          0x01
#define FRAME_PCM_UP_END      0x02
#define FRAME_PCM_DOWN        0x10
#define FRAME_PCM_DOWN_END    0x11
#define FRAME_ERROR           0x20

/* Protocol version */
#define PROTO_VERSION         0x01

/* Frame header: 5 bytes total (4 len + 1 type) */
#define FRAME_HEADER_SIZE     5

typedef struct __attribute__((packed)) {
    uint32_t length_le;   /* payload length, little-endian */
    uint8_t  type;
} frame_header_t;

/* Session start payload */
typedef struct __attribute__((packed)) {
    uint8_t  proto_version;
    uint32_t device_id;
    uint16_t sample_rate;
    uint8_t  bit_depth;
    uint8_t  channels;
    uint8_t  flags;
} session_start_payload_t;

/**
 * Send a frame over a socket.
 * @param sock   BSD socket fd
 * @param type   Frame type byte
 * @param data   Payload (may be NULL if length==0)
 * @param length Payload length in bytes
 */
esp_err_t proto_send_frame(int sock, uint8_t type, const void *data, uint32_t length);

/**
 * Read exactly 'len' bytes from socket into buf.
 * Returns ESP_OK or ESP_FAIL on error/disconnect.
 */
esp_err_t proto_recv_exact(int sock, void *buf, size_t len);

/**
 * Read one frame header from socket.
 * Fills header->length_le (host byte order after conversion) and header->type.
 */
esp_err_t proto_recv_header(int sock, frame_header_t *header);

#ifdef __cplusplus
}
#endif
