#include <string.h>
#include <errno.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <sys/ioctl.h>
#include <sys/select.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "audio.h"
#include "wake.h"
#include "net.h"
#include "ui.h"
#include "protocol.h"
#include "sdkconfig.h"

static const char *TAG = "NET";
static uint32_t s_last_up_ms = 0;
static volatile bool s_session_active   = false;
static volatile bool s_cancel_requested = false;
static volatile bool s_expects_reply    = false;  /* set by FRAME_REPLY_META */
#define MAX_FOLLOWUP_TURNS  5   /* safety cap on consecutive follow-up turns */
static uint32_t s_rx_samples, s_rx_frames;
static int32_t s_rx_min, s_rx_max;

/* RX pacing diagnostics — one RXSTAT line per second during playback */
static uint32_t s_rst_frames, s_rst_recv_us, s_rst_write_us;
static uint32_t s_rst_max_recv_us, s_rst_max_write_us;
static uint32_t s_rst_av_min, s_rst_av_max;
static int64_t  s_rst_last_ms;

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
    const uint8_t *parts[] = {(const uint8_t *)&hdr, (const uint8_t *)data};
    const size_t lengths[] = {FRAME_HEADER_SIZE, length};
    for (int part = 0; part < 2; part++) {
        if (lengths[part] == 0 || parts[part] == NULL) continue;
        size_t sent_total = 0;
        while (sent_total < lengths[part]) {
            int sent = send(sock, parts[part] + sent_total,
                            lengths[part] - sent_total, 0);
            if (sent <= 0) return ESP_FAIL;
            sent_total += (size_t)sent;
        }
    }
    return ESP_OK;
}

esp_err_t proto_recv_exact(int sock, void *buf, size_t len)
{
    size_t received = 0;
    uint8_t *ptr = (uint8_t *)buf;
    while (received < len) {
        if (s_cancel_requested) return ESP_FAIL;  /* user cancelled */
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

    /* Bound both directions: a dead bridge must never wedge the net task on
     * send() and leave the UI permanently on Recording. */
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval send_tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));

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

/* Interruptible wait for socket readability: true = data ready,
 * false = cancel requested (or socket error). Bounds cancel latency
 * during the long STT/LLM processing gap (SO_RCVTIMEO is 30 s). */
static bool wait_sock_readable(int sock)
{
    fd_set rfds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    while (!s_cancel_requested) {
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        int r = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
    return false;
}

static void run_session(int sock, bool is_followup)
{
    s_cancel_requested = false;
    s_session_active   = true;
    s_expects_reply    = false;

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
        s_session_active = false;
        return;
    }

    ui_set_state(is_followup ? UI_STATE_IDLE : UI_STATE_RECORDING);
    ESP_LOGI(TAG, "Session started, streaming audio... (followup=%d)", (int)is_followup);

    int16_t  pcm_buf[FRAME_SAMPLES];
    uint32_t session_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool     speech_started   = false;
    bool     speech_ended     = false;
    uint32_t silence_start_ms = 0;

    /* --- Upload phase --- */
    while (!speech_ended) {
        if (s_cancel_requested) goto session_cancelled;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_ms - session_start_ms) > (uint32_t)MAX_SESSION_MS) {
            ESP_LOGW(TAG, "Max session duration reached");
            break;
        }

        size_t got = audio_get_frames(pcm_buf, FRAME_SAMPLES, pdMS_TO_TICKS(200));
        /* DEBUG build21: upload status every 2 s */
        if (now_ms - s_last_up_ms >= 2000) {
            s_last_up_ms = now_ms;
            if (got == 0) {
                ESP_LOGW(TAG, "UP: RING EMPTY ms=%lu sp=%d",
                         (unsigned long)(now_ms - session_start_ms), speech_started);
            } else {
                uint64_t e2 = 0;
                for (size_t i = 0; i < got; i++) { int32_t s = pcm_buf[i]; e2 += (uint64_t)(s * s); }
                ESP_LOGI(TAG, "UP: got=%lu rms=%lu ms=%lu sp=%d",
                         (unsigned long)got, (unsigned long)sqrtf((float)(e2 / got)),
                         (unsigned long)(now_ms - session_start_ms), speech_started);
            }
        }
        if (got == 0) continue;

        /* Simple energy-based VAD on RAW microphone samples. The configured
         * 1500/700 thresholds are raw-domain values. Applying the 10x upload
         * gain before VAD turns normal room noise (~100-170 RMS) into
         * 1000-1700 RMS, so silence never reaches the stop threshold and each
         * turn remains on Recording until the 10-second hard cap. */
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
                if (is_followup) ui_set_state(UI_STATE_RECORDING);
                ESP_LOGI(TAG, "VAD: speech start");
            } else {
                continue; /* Don't send pre-speech audio */
            }
        }

        /* Normalize only the audio sent to STT; keep VAD in the raw domain. */
        for (size_t i = 0; i < got; i++) {
            int32_t v = (int32_t)pcm_buf[i] * 10;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            pcm_buf[i] = (int16_t)v;
        }

        /* Send PCM-UP frame */
        if (proto_send_frame(sock, FRAME_PCM_UP, pcm_buf, got * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "Send PCM-UP failed");
            goto session_done;
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
    s_rx_samples = 0; s_rx_frames = 0; s_rx_min = 32767; s_rx_max = -32768;
    s_rst_frames = s_rst_recv_us = s_rst_write_us = 0;
    s_rst_max_recv_us = s_rst_max_write_us = 0;
    s_rst_av_min = 0xFFFFFFFF;
    s_rst_av_max = 0;
    s_rst_last_ms = 0;

    /* --- Playback phase --- */
    while (1) {
        if (s_cancel_requested) goto session_cancelled;
        frame_header_t hdr;
        if (!wait_sock_readable(sock)) goto session_cancelled;
        if (proto_recv_header(sock, &hdr) != ESP_OK) {
            if (s_cancel_requested) goto session_cancelled;
            ESP_LOGW(TAG, "Connection lost during playback");
            break;
        }

        uint32_t payload_len = hdr.length_le;

        if (hdr.type == FRAME_PCM_DOWN) {
            /* Switch to "Speaking" only when TTS audio actually arrives;
             * while STT/LLM are running the face stays on "Processing". */
            ui_set_state(UI_STATE_PLAYING);
            /* Stream PCM to playback buffer in chunks */
            uint8_t  chunk[512];
            uint32_t remaining = payload_len;
            while (remaining > 0) {
                uint32_t to_read = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
                int64_t t0 = esp_timer_get_time();
                if (proto_recv_exact(sock, chunk, to_read) != ESP_OK) {
                    if (s_cancel_requested) goto session_cancelled;
                    ESP_LOGW(TAG, "PCM-DOWN recv error");
                    goto session_done;
                }
                uint32_t recv_us = (uint32_t)(esp_timer_get_time() - t0);
                {
                    const int16_t *pcm = (const int16_t *)chunk;
                    uint32_t n = to_read / sizeof(int16_t);
                    int32_t mn = 32767, mx = -32768;
                    for (uint32_t i = 0; i < n; i++) {
                        if (pcm[i] < mn) mn = pcm[i];
                        if (pcm[i] > mx) mx = pcm[i];
                    }
                    s_rx_samples += n;
                    if (mn < s_rx_min) s_rx_min = mn;
                    if (mx > s_rx_max) s_rx_max = mx;
                    if ((s_rx_frames++ % 40) == 0) {
                        ESP_LOGI(TAG, "RX chunk: %lu B min=%ld max=%ld (total %lu smp)",
                                 (unsigned long)to_read, (long)mn, (long)mx, (unsigned long)s_rx_samples);
                    }
                }
                size_t av = audio_playback_available();
                int64_t t1 = esp_timer_get_time();
                audio_play_frames((int16_t *)chunk, to_read / sizeof(int16_t));
                uint32_t write_us = (uint32_t)(esp_timer_get_time() - t1);

                s_rst_frames++;
                s_rst_recv_us += recv_us;
                s_rst_write_us += write_us;
                if (recv_us > s_rst_max_recv_us) s_rst_max_recv_us = recv_us;
                if (write_us > s_rst_max_write_us) s_rst_max_write_us = write_us;
                if (av < s_rst_av_min) s_rst_av_min = (uint32_t)av;
                if (av > s_rst_av_max) s_rst_av_max = (uint32_t)av;
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - s_rst_last_ms >= 1000) {
                    int pending = 0;
                    ioctl(sock, FIONREAD, &pending);
                    ESP_LOGI(TAG, "RXSTAT: %lu ch, recv %lums (max %lums), write %lums (max %lums), pend=%d, ring %lu..%lu",
                             (unsigned long)s_rst_frames,
                             (unsigned long)(s_rst_recv_us / 1000),
                             (unsigned long)(s_rst_max_recv_us / 1000),
                             (unsigned long)(s_rst_write_us / 1000),
                             (unsigned long)(s_rst_max_write_us / 1000),
                             pending,
                             (unsigned long)s_rst_av_min,
                             (unsigned long)s_rst_av_max);
                    s_rst_frames = s_rst_recv_us = s_rst_write_us = 0;
                    s_rst_max_recv_us = s_rst_max_write_us = 0;
                    s_rst_av_min = 0xFFFFFFFF;
                    s_rst_av_max = 0;
                    s_rst_last_ms = now_ms;
                }
                remaining -= to_read;
            }
        } else if (hdr.type == FRAME_REPLY_META) {
            uint8_t meta = 0;
            if (payload_len >= 1 && proto_recv_exact(sock, &meta, 1) == ESP_OK) {
                s_expects_reply = (meta == 1);
            }
            ESP_LOGI(TAG, "Reply meta: expects_reply=%d", (int)s_expects_reply);
        } else if (hdr.type == FRAME_PCM_DOWN_END) {
            ESP_LOGI(TAG, "Session complete (PCM-DOWN End)");
            ESP_LOGI(TAG, "RX audio total: %lu samples min=%ld max=%ld", (unsigned long)s_rx_samples, (long)s_rx_min, (long)s_rx_max);
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

session_cancelled:
    audio_playback_flush();   /* drop queued TTS tail */
    ESP_LOGW(TAG, "Session cancelled by user");

session_done:
    s_session_active = false;
    ui_set_state(UI_STATE_IDLE);
}

/* ---------- diagnostic task: dump all task states every 30 s ---------- */
static void diag_task(void *arg)
{
    static const char *names[] = {"net_stream", "audio_cap", "audio_play",
                                  "wake_detect", "touch", "lvgl", "main"};
    ESP_LOGI(TAG, "DIAG: task started (30 s state dumps)");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            TaskHandle_t h = xTaskGetHandle(names[i]);
            if (h) {
                ESP_LOGI(TAG, "DIAG: %-12s state=%d wm=%u", names[i],
                         (int)eTaskGetState(h),
                         (unsigned)uxTaskGetStackHighWaterMark(h));
            } else {
                ESP_LOGW(TAG, "DIAG: %-12s MISSING", names[i]);
            }
        }
    }
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
        wake_detection_suspend(); /* session owns the mic ring */

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

        /* Tapped cancel during the connect phase */
        if (s_cancel_requested) {
            s_cancel_requested = false;
            ESP_LOGW(TAG, "Session cancelled during connect");
            close(sock);
            sock = -1;
            ui_set_state(UI_STATE_IDLE);
            wake_detection_resume();
            continue;
        }

        if (sock < 0) {
            ESP_LOGE(TAG, "Could not connect after %d retries", TCP_RETRIES);
            ui_set_state(UI_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(3000));
            ui_set_state(UI_STATE_IDLE);
            wake_detection_resume();
            continue;
        }

        run_session(sock, false);

        /* Follow-up loop: when the LLM asked the user a question, keep the
         * line open and go straight back to listening - no wake word needed.
         * A silent follow-up window (no speech in MAX_SESSION_MS) yields an
         * empty transcript, no meta frame, and the loop exits. */
        int followups = 0;
        while (s_expects_reply && !s_cancel_requested && followups < MAX_FOLLOWUP_TURNS) {
            /* Let the TTS tail finish before arming the mic (max 3 s) */
            for (int i = 0; i < 30 && audio_playback_available() > 256; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            followups++;
            ESP_LOGI(TAG, "Follow-up turn %d - LLM expects reply, staying connected", followups);
            run_session(sock, true);
        }

        /* Drain stale wake events queued while the session was running
         * (double detections) so we don't re-enter a session immediately */
        while (xQueueReceive(wake_q, &evt, 0) == pdTRUE) {}

        /* Let the TTS tail settle, then re-arm the wake word (avoids
         * self-triggering on the reply audio) */
        vTaskDelay(pdMS_TO_TICKS(500));
        wake_detection_resume();

        /* Close the per-session socket. The bridge drops its side on the
         * 120 s idle timeout; a zero-byte keepalive can't detect a half-dead
         * TCP, and reusing that socket breaks the next session. */
        close(sock);
        sock = -1;
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
    /* diag task dropped: creation failed at boot (internal heap exhausted
     * after WiFi). RXSTAT lines in the RX loop cover the same diagnostics. */
}

bool net_session_active(void)
{
    return s_session_active;
}

void net_cancel_session(void)
{
    if (s_session_active) {
        s_cancel_requested = true;
        ESP_LOGI(TAG, "Cancel requested");
    }
}
