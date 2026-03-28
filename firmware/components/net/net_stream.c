#include <string.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio.h"
#include "wake.h"
#include "net.h"
#include "ui.h"
#include "protocol.h"
#include "sdkconfig.h"

static const char *TAG = "NET";

#define SERVER_HOST        CONFIG_HERVOICE_SERVER_HOST
#define SERVER_PORT        CONFIG_HERVOICE_SERVER_PORT
#define DEVICE_ID          CONFIG_HERVOICE_DEVICE_ID
#define SAMPLE_RATE        CONFIG_HERVOICE_SAMPLE_RATE
#define FRAME_SAMPLES      CONFIG_HERVOICE_AUDIO_FRAME_SAMPLES
#define TCP_RETRIES        CONFIG_HERVOICE_TCP_RETRY_COUNT
#define MAX_SESSION_MS     CONFIG_HERVOICE_MAX_SESSION_DURATION_MS
#define VAD_SILENCE_MS     CONFIG_HERVOICE_VAD_SILENCE_TIMEOUT_MS
#define VAD_START_THR      CONFIG_HERVOICE_VAD_START_THRESHOLD
#define VAD_STOP_THR       CONFIG_HERVOICE_VAD_STOP_THRESHOLD

#define NET_TASK_STACK     8192
#define NET_TASK_PRIO      5

static TaskHandle_t s_net_task = NULL;

/* ---------- helpers ---------- */

esp_err_t proto_send_frame(int sock, uint8_t type, const void *data, uint32_t length)
{
    frame_header_t hdr;
    hdr.length_le = length;  /* already little-endian on ESP32 (little-endian arch) */
    hdr.type = type;
    int sent = send(sock, &hdr, FRAME_HEADER_SIZE, 0);
    if (sent != FRAME_HEADER_SIZE) return ESP_FAIL;
    if (length > 0 && data) {
        sent = send(sock, data, length, 0);
        if ((uint32_t)sent != length) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t proto_recv_exact(int sock, void *buf, size_t len)
{
    size_t received = 0;
    uint8_t *ptr = (uint8_t *)buf;
    while (received < len) {
        int r = recv(sock, ptr + received, len - received, 0);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }
    return ESP_OK;
}

esp_err_t proto_recv_header(int sock, frame_header_t *header)
{
    esp_err_t ret = proto_recv_exact(sock, header, FRAME_HEADER_SIZE);
    /* length_le is already little-endian, ESP32 is LE so no swap needed */
    return ret;
}

/* ---------- connection ---------- */

static int connect_to_server(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", SERVER_PORT);

    int err = getaddrinfo(SERVER_HOST, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "getaddrinfo failed: %d", err);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        freeaddrinfo(res);
        return -1;
    }

    /* Set receive timeout */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect() failed: %d", errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    ESP_LOGI(TAG, "Connected to %s:%d", SERVER_HOST, SERVER_PORT);
    return sock;
}

/* ---------- session ---------- */

static void run_session(int sock)
{
    /* Send Session Start */
    session_start_payload_t ss = {
        .proto_version = PROTO_VERSION,
        .device_id     = DEVICE_ID,
        .sample_rate   = SAMPLE_RATE,
        .bit_depth     = 16,
        .channels      = 1,
        .flags         = 0,
    };
    if (proto_send_frame(sock, FRAME_SESSION_START, &ss, sizeof(ss)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send Session Start");
        return;
    }

    ui_set_state(UI_STATE_RECORDING);
    ESP_LOGI(TAG, "Session started, streaming audio...");

    int16_t  pcm_buf[FRAME_SAMPLES];
    uint32_t session_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool     speech_started   = false;
    bool     speech_ended     = false;
    uint32_t silence_start_ms = 0;

    /* --- Upload phase --- */
    while (!speech_ended) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - session_start_ms) > (uint32_t)MAX_SESSION_MS) {
            ESP_LOGW(TAG, "Max session duration reached");
            break;
        }

        size_t got = audio_get_frames(pcm_buf, FRAME_SAMPLES, pdMS_TO_TICKS(200));
        if (got == 0) continue;

        /* Simple energy-based VAD */
        uint64_t energy = 0;
        for (size_t i = 0; i < got; i++) {
            int32_t s = pcm_buf[i];
            energy += (uint64_t)(s * s);
        }
        uint32_t rms = (uint32_t)sqrtf((float)(energy / got));

        if (!speech_started) {
            if (rms >= VAD_START_THR) {
                speech_started = true;
                silence_start_ms = 0;
                ESP_LOGI(TAG, "VAD: speech start");
            } else {
                continue; /* Don't send pre-speech audio */
            }
        }

        /* Send PCM-UP frame */
        if (proto_send_frame(sock, FRAME_PCM_UP, pcm_buf, got * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "Send PCM-UP failed");
            return;
        }

        /* Check for end of speech */
        if (rms < VAD_STOP_THR) {
            if (silence_start_ms == 0) silence_start_ms = now_ms;
            else if ((now_ms - silence_start_ms) >= (uint32_t)VAD_SILENCE_MS) {
                speech_ended = true;
                ESP_LOGI(TAG, "VAD: speech end (silence timeout)");
            }
        } else {
            silence_start_ms = 0;
        }
    }

    /* Send PCM-UP End */
    proto_send_frame(sock, FRAME_PCM_UP_END, NULL, 0);
    ui_set_state(UI_STATE_SENDING);
    ESP_LOGI(TAG, "Audio upload done, waiting for response...");

    /* --- Playback phase --- */
    ui_set_state(UI_STATE_PLAYING);
    while (1) {
        frame_header_t hdr;
        if (proto_recv_header(sock, &hdr) != ESP_OK) {
            ESP_LOGW(TAG, "Connection lost during playback");
            break;
        }

        uint32_t payload_len = hdr.length_le;

        if (hdr.type == FRAME_PCM_DOWN) {
            /* Stream PCM to playback buffer in chunks */
            uint8_t  chunk[512];
            uint32_t remaining = payload_len;
            while (remaining > 0) {
                uint32_t to_read = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
                if (proto_recv_exact(sock, chunk, to_read) != ESP_OK) {
                    ESP_LOGW(TAG, "PCM-DOWN recv error");
                    goto session_done;
                }
                audio_play_frames((int16_t *)chunk, to_read / sizeof(int16_t));
                remaining -= to_read;
            }
        } else if (hdr.type == FRAME_PCM_DOWN_END) {
            ESP_LOGI(TAG, "Session complete (PCM-DOWN End)");
            audio_playback_flush();
            break;
        } else if (hdr.type == FRAME_ERROR) {
            uint8_t err_buf[256];
            uint32_t to_read = payload_len < sizeof(err_buf) - 1 ? payload_len : sizeof(err_buf) - 1;
            proto_recv_exact(sock, err_buf, to_read);
            err_buf[to_read] = '\0';
            ESP_LOGW(TAG, "Server error: %s", err_buf);
            break;
        } else {
            /* Unknown type: skip payload */
            uint8_t discard[64];
            uint32_t remaining = payload_len;
            while (remaining > 0) {
                uint32_t to_read = remaining < sizeof(discard) ? remaining : sizeof(discard);
                if (proto_recv_exact(sock, discard, to_read) != ESP_OK) goto session_done;
                remaining -= to_read;
            }
        }
    }

session_done:
    ui_set_state(UI_STATE_IDLE);
}

/* ---------- main task ---------- */

static void net_stream_task(void *arg)
{
    QueueHandle_t wake_q = wake_get_event_queue();
    int           sock   = -1;

    ESP_LOGI(TAG, "Net stream task started");

    while (1) {
        /* Wait for wake event */
        wake_event_t evt;
        if (xQueueReceive(wake_q, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.type != WAKE_EVENT_WAKEWORD_DETECTED) continue;

        ESP_LOGI(TAG, "Wake event received, opening session");
        ui_set_state(UI_STATE_WAKE);

        /* Connect with exponential backoff */
        if (sock < 0) {
            uint32_t delay_ms = 500;
            for (int i = 0; i < TCP_RETRIES; i++) {
                sock = connect_to_server();
                if (sock >= 0) break;
                ESP_LOGW(TAG, "Connect failed, retry %d/%d in %lums", i+1, TCP_RETRIES, delay_ms);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                delay_ms = delay_ms < 8000 ? delay_ms * 2 : 8000;
            }
        }

        if (sock < 0) {
            ESP_LOGE(TAG, "Could not connect after %d retries", TCP_RETRIES);
            ui_set_state(UI_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(3000));
            ui_set_state(UI_STATE_IDLE);
            continue;
        }

        run_session(sock);

        /* Keep connection alive for next session; if it was dropped, mark as closed */
        /* Simple keepalive: try a zero-byte send to detect dead socket */
        int test = send(sock, NULL, 0, MSG_DONTWAIT);
        if (test < 0 && errno != EAGAIN) {
            close(sock);
            sock = -1;
        }
    }
}

esp_err_t net_init(void)
{
    extern esp_err_t wifi_init_sta(void);
    return wifi_init_sta();
}

void net_start_stream_task(void)
{
    xTaskCreatePinnedToCore(net_stream_task, "net_stream", NET_TASK_STACK,
                            NULL, NET_TASK_PRIO, &s_net_task, 0);
}
