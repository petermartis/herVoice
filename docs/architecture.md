# herVoice system architecture

This document describes the deployed system represented by the source tree. Source code is authoritative if this document and an implementation detail diverge.

## Design goals

herVoice is the low-latency voice endpoint for Charlie. The system is designed to provide:

- English and Slovak interaction only
- Charlie's Hermes persona, family context, memory, and tools
- Local LLM inference for voice and ordinary Hermes work
- On-device wake word, VAD, touch control, and responsive status UI
- Streamed speech output and wake-free follow-up turns
- A path toward barge-in and full-duplex conversation

## Runtime hosts

### ESP32-S3 endpoint

The endpoint is a Waveshare ESP32-S3-Touch-LCD-1.85C V2. It owns real-time interaction:

- WakeNet detection with `wn9_sophia_tts`
- Microphone capture through ES7210
- Raw-energy VAD and utterance endpointing
- Framed TCP upload and response reception
- ES8311 playback
- ST77916/LVGL status display and audio halo
- CST816S gestures for wake, cancel, and volume

The endpoint has no general-purpose LLM, STT, or TTS model. It sends 16 kHz mono signed PCM to the backend and receives the same audio format.

### LLM host (`192.168.1.35`)

The checked-out deployment is `/home/foscam/herVoice`. Three system services split the backend pipeline:

| Service | Listener | Entry point | Responsibility |
|---|---|---|---|
| `hervoice-bridge` | `0.0.0.0:8765` | `backend/server.py` | Device sessions, history, STT/LLM/TTS orchestration, binary protocol |
| `hervoice-stt` | `0.0.0.0:15556` | `backend/services/stt_server.py` | faster-whisper transcription (GPU, CPU fallback) |
| `hervoice-tts` | `127.0.0.1:15555` | `backend/services/tts_server.py` | Kokoro, ElevenLabs, and optional Piper synthesis |

The same host exposes local Qwen 3.8 Flash through the Strata OpenAI-compatible API on `127.0.0.1:8081`.

### Hermes NAS (`192.168.1.100`)

The bridge's primary LLM path is the Hermes OpenAI-compatible gateway on port `8642`. The request explicitly carries:

- `provider: custom`
- `model: qwen3.8-flash-next-unsloth-ud-iq4_xs-v31`
- the voice system prompt and the newest user message only
- header `X-Hermes-Session-Id: voice-<device id>-<epoch>`
- `stream: true`
- Qwen thinking suppression via `model_options.reasoning.enabled=false` (the direct Strata fallback uses `chat_template_kwargs.enable_thinking=false`)

The session id makes the gateway continue its own stored transcript, which includes tool calls and their results. The bridge must not resend earlier turns itself: a resent history holds only the spoken replies, and the model then copies that pattern and answers home-state questions without calling a tool.

The gateway adds Charlie's persona, persistent memory, user/family profile, skills, and selected tools. Voice is therefore not an anonymous direct-model chatbot.

If the gateway is disabled, has no API key, fails before emitting content, returns an error, or returns an empty reply, the bridge falls back to Strata directly at `127.0.0.1:8081`. That fallback keeps the speaker responsive but does not provide the full Hermes persona/tool layer. A failure after streamed content begins is not retried, because retrying could speak a duplicated answer.

## End-to-end sequence

```text
ESP32                  Bridge              STT             Hermes/Qwen          TTS
  | wake/tap              |                  |                    |                |
  |-- SESSION_START ----->|                  |                    |                |
  |-- PCM_UP × N -------->|                  |                    |                |
  |-- PCM_UP_END -------->|-- WAV/raw audio->|                    |                |
  |                       |<-- transcript ---|                    |                |
  |                       |---------------- voice messages ------>|                |
  |                       |<------------ streamed JSON -----------|                |
  |                       |-------------------------------- sentence + lang ------>|
  |<-- PCM_DOWN × N ------|<---------------- WAV/PCM ------------------------------|
  |<-- REPLY_META --------|                  |                    |                |
  |<-- PCM_DOWN_END ------|                  |                    |                |
  | follow-up if requested, reusing the same TCP socket                            |
```

### Device capture and endpointing

1. WakeNet or a short idle tap posts a wake event.
2. The network task suspends WakeNet before consuming microphone samples. WakeNet and session upload share one capture ring; allowing both readers would split the utterance.
3. Firmware connects to `CONFIG_HERVOICE_SERVER_HOST:CONFIG_HERVOICE_SERVER_PORT`, currently `192.168.1.35:8765`.
4. Firmware sends `SESSION_START` with protocol version, device ID, and audio format.
5. Before speech begins, blocks below the start threshold are discarded.
6. VAD operates on unamplified RMS. Once speech starts, samples are multiplied by 10 with int16 saturation for STT upload.
7. The utterance ends after 600 ms below stop threshold or at the 10-second turn cap.

Current VAD constants are start RMS 1500, stop RMS 700, 600 ms trailing silence, and 10 seconds maximum. `firmware/components/net/net_stream.c` is authoritative; the separate `wake/vad.c` is not the active session endpointing path.

### STT and language normalization

The STT service runs faster-whisper `large-v3-turbo` on the GPU with `int8_float16` compute (about 1.6 GB of VRAM, roughly 0.07 s per clip), `beam_size=1`, and `vad_filter=False`. If the model cannot load on the GPU, or a call fails there, the service reloads on CPU int8 with 16 threads (about 2.9 s per clip with language detection). Firmware already performs endpointing; a second server-side VAD can drop speech and is intentionally disabled.

Language policy:

1. The first turn uses automatic detection, which is a two-way decision made from one language-detection pass.
2. The clip is English only when English is Whisper's first choice; anything else is Slovak. Comparing the `en` score with the `sk` score is wrong: short Slovak clips are often scored as Polish or Czech with Slovak near zero.
3. The clip is decoded once, in the chosen language. The response includes `lang_probs` for diagnosis.
4. Follow-up turns pass the established `en` or `sk` language directly to STT.
5. Session logic treats exactly `en` as English and every other result as Slovak.

The language lock is scoped to the current TCP connection. Per-device conversation text persists across reconnects while the bridge process is alive, but the connection language does not.

### Conversation history

`backend/server.py` stores one history list per `device_id`. The bridge peeks at the first `SESSION_START` to select the list before constructing a session, so the first turn and all follow-ups share the same history.

This local history is used only by the direct Strata fallback. On the primary path the gateway holds the transcript under the conversation's session id. Assistant turns are stored locally in the same JSON form the model is asked to produce.

The local history is process-local and trimmed in place to the system prompt plus the last 20 messages. After `HISTORY_IDLE_RESET_S` (default 180 s) without a turn from a device, its local history is cleared and a new gateway session id is issued, so a new wake does not inherit stale home states. A bridge restart also clears it; Hermes itself may still contribute persistent user memory.

### Household context

`backend/config.py` holds only behaviour rules. Family facts, room and device identifiers, and tool recipes are read at startup from a private file outside the repository (`HERVOICE_CONTEXT_FILE`, default `/home/foscam/herVoice-private/voice_context.md`, mode `0600`) and appended to the voice system prompt. The repository is public: never put names or device identifiers in tracked files. The bridge startup line reports the context size; zero means the file was not found and Charlie has no home knowledge.

### LLM output contract

The voice system prompt asks Charlie to return only:

```json
{"reply": "spoken answer", "followup": false}
```

Requirements include:

- one to three short spoken sentences
- English or Slovak only
- no mid-conversation language switching
- no markdown or stage directions
- execute an allowed tool before claiming a home action or data lookup succeeded
- `followup=true` only when Charlie needs another user response

The parser tolerates code fences, extracts the first JSON object when necessary, and accepts plain-text fallback output. The effective follow-up flag is the model's flag OR a heuristic that recognizes a trailing question mark and several English request patterns. Slovak statement-form questions without `?` remain a known heuristic gap.

### Sentence-streamed TTS

The bridge incrementally extracts the `reply` field from partial JSON and divides it at sentence boundaries. A producer/consumer pipeline synthesizes complete sentences while the LLM continues generating later text. The final sentence is queued the moment the reply string's closing quote arrives, not when the stream ends, because the gateway keeps the stream open for about a second after the text is complete. Text outside the JSON reply is never spoken mid-stream. This reduces time to first audio.

The TTS client sends `{text, lang}` to `127.0.0.1:15555/synthesize`, reads a WAV response, resamples it to the device sample rate with `scipy.signal.resample_poly`, and sends 50 ms `PCM_DOWN` chunks.

Engine selection:

- English: Kokoro `af_heart`, normally 24 kHz before resampling
- Slovak: ElevenLabs multilingual synthesis when its key and voice ID are configured
- Piper Slovak: loaded only when ElevenLabs is not configured at service startup

Reply-text diacritics override an incorrectly detected English session for TTS voice selection. If a sentence contains Slovak diacritics, it uses Slovak synthesis.

When ElevenLabs is configured, Piper is not preloaded. Consequently, an ElevenLabs runtime failure currently returns an error instead of providing an effective Piper fallback.

## Follow-up turns

The bridge sends `REPLY_META` with one byte immediately before `PCM_DOWN_END`:

- `0`: final answer
- `1`: Charlie expects another user response

When the value is `1`, firmware waits for the playback tail, sends another `SESSION_START` on the same socket, and records the next utterance without requiring the wake word. The device permits at most five consecutive follow-ups. A silent follow-up times out, receives an empty end response, closes the socket, and returns to wake mode.

## Cancellation

Cancellation is local and has no dedicated protocol frame. A short tap during an active session sets a cancellation flag. Receive polling notices within approximately 200 ms, the endpoint returns to Ready, and the socket closes. The backend observes EOF or reset.

Current `audio_playback_flush()` drains queued audio rather than clearing it. Code comments that describe immediate queued-audio discard are stale.

## Firmware subsystem invariants

### Initialization order

The main boot order is:

1. NVS
2. display/UI
3. idle UI state
4. `esp_srmodel_init("model")`
5. `wake_init()` and WakeNet model creation
6. `audio_init()`
7. 440 Hz, 600 ms speaker test tone
8. capture task
9. WakeNet task
10. touch
11. non-blocking Wi-Fi initialization
12. network stream task

WakeNet model creation must remain before `audio_init()` and before I2S DMA. Reversing this order has caused intermittent model corruption and `LoadProhibited` failures.

### Memory ownership

- Capture ring: 8000 mono samples, approximately 500 ms, internal RAM; overwrite-oldest behavior
- Preferred playback ring: 32768 mono samples, approximately 2.048 seconds, PSRAM
- Playback fallback allocations: 16384 PSRAM, 16384 internal, then 8192 internal samples
- LCD draw buffers: two 6480-pixel RGB565 buffers in DMA-capable internal RAM, not PSRAM
- PSRAM mode: OCT/OPI at 80 MHz; QUAD is wrong for this V2 board

Playback copies PSRAM data through an internal buffer before I2S DMA. Writes use semaphore pacing with a five-second timeout rather than intentionally dropping full-ring audio.

### Wi-Fi

Wi-Fi startup is non-blocking. Disconnect sets Error state and schedules reconnect through a helper task. Retries never stop. `WIFI_PS_NONE` avoids modem-sleep burst delivery that breaks streaming audio. Static RX buffers are reduced to four to retain internal RAM headroom.

## UI and touch

| State | Label | Color |
|---|---|---|
| Idle / wake transition | Ready | dark green `(0,80,0)` |
| Active speech capture | Listening... | dark red `(160,0,0)` |
| Uploaded, awaiting response | Processing... | blue `(0,0,140)` |
| Receiving/playing audio | Speaking... | teal `(0,120,120)` |
| Network fault | Error | bright red `(200,0,0)` |

Recording and playback show a centered level halo. LVGL size updates are paced to 20 FPS and dirty-checked to prevent display traffic from starving audio.

Touch gestures:

- short tap while idle: manual wake
- short tap during a session: cancel
- swipe up/down: volume +5/-5, persisted in NVS
- long press: toggles an internal mute flag, but mute is not currently applied to the codec, amplifier, capture, or network path

After an esptool hard reset, CST816S can remain unresponsive until USB power is physically cycled.

## Configuration

### Firmware

Wi-Fi credentials live only in gitignored `firmware/sdkconfig`. Important Kconfig values include server host/port, sample rate, device ID, and wake model. The active detector keeps the WakeNet model's own calibrated threshold (0.647 for `wn9_sophia_tts`); forcing the engine minimum of 0.4 let ambient sound open sessions several times an hour. The `CONFIG_HERVOICE_WAKE_THRESHOLD` setting is not wired into the current detector.

### Bridge environment

The system unit loads `/home/foscam/herVoice/.env`. Values must never be committed or printed. Variables read by the bridge include:

- `HERVOICE_HOST`, `HERVOICE_PORT`
- `WHISPER_SERVER_URL`
- `LLM_SERVER_URL`, `LLM_MODEL`, `LLM_API_KEY`
- `LLM_NO_THINK`, `LLM_MAX_TOKENS`, `LLM_TEMPERATURE`, `LLM_SYSTEM_PROMPT`
- `HERMES_API_ENABLED`, `HERMES_API_URL`, `HERMES_API_KEY`
- `HERMES_TIMEOUT_S`, `HERMES_LLM_PROVIDER`, `HERMES_LLM_MODEL`
- `HERMES_DISABLE_THINKING`
- `HERVOICE_CONTEXT_FILE`, `HISTORY_IDLE_RESET_S`
- `PIPER_SERVER_URL`

The TTS service additionally reads `ELEVENLABS_API_KEY`, `ELEVENLABS_SK_VOICE_ID`, `ELEVENLABS_MODEL`, and `PIPER_VOICE_PATH`. Verify the installed TTS systemd unit actually loads the protected environment file; the checked-in unit currently does not declare it.

## Dataset capture mode

When `/tmp/hervoice_record_state.json` marks collection active, the bridge bypasses STT, LLM, and TTS. It plays start/end beeps and saves a 16 kHz mono WAV under `/home/foscam/herVoice/dataset/raw`. This path supports custom wake-word corpus collection.

## Latency instrumentation

`backend/session.py` records stage marks and emits one `LATENCY` log line per real turn. Important fields include audio upload, STT duration, first LLM content, total LLM duration, first TTS audio, first-audio time, total time, tail, and language. Use bridge journal logs for performance work rather than estimating from UI transitions.

## Trust and security boundary

The device protocol has no encryption or peer authentication. It assumes a trusted routed home network. The API gateway and Strata endpoints require bearer credentials, stored only in protected environment files. Do not expose device TCP port 8765 or local HTTP model services to untrusted networks.

## Known implementation gaps

At commit `e8848ff`, developers should account for these concrete gaps:

1. The STT service has no automated contract test; it accepts both the bridge's multipart form and a raw audio body.
2. The checked-in TTS unit does not load the root `.env`, although ElevenLabs configuration normally lives there.
3. Piper is not an effective runtime fallback after an already-configured ElevenLabs engine fails.
4. Slovak statement-form follow-up questions without a question mark may not trigger the heuristic when the model's flag is wrong.
5. Protocol version, declared format, exact session payload length, maximum incoming frame size, and PCM even-byte length are not comprehensively validated.
6. The manual E2E client skips `REPLY_META` and cannot exercise a second follow-up turn.
7. Touch mute is not functionally enforced.
8. The bridge's fallback history is process-local; the primary transcript lives in the gateway.

See [development.md](development.md) for verification procedures and prioritized development work.
