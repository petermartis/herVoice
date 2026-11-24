# herVoice development and operations guide

This guide covers repository ownership, firmware builds, backend deployment, runtime verification, debugging, and known development priorities.

## Sources of truth

| Item | Authoritative location |
|---|---|
| Canonical development repository | Mac mini `/Users/pmartis/herVoice` |
| Shared remote | `github.com/petermartis/herVoice`, branch `main` |
| NAS mirror | `/volume1/homes/hermes/herVoice` |
| Running backend checkout | LLM host `/home/foscam/herVoice` |
| Firmware behavior | Source under `firmware/`, especially `main.c` and component implementations |
| Backend behavior | Source under `backend/` and installed systemd units |
| Runtime secrets | LLM host `/home/foscam/herVoice/.env`, mode `0600` |

The deployed checkout contains intentionally untracked runtime state such as `.env`, `venv/`, datasets, generated audio, and local backups. Synchronize tracked files by commit without deleting those assets.

Before development, require all tracked copies to agree:

```bash
# Run in each checkout
git fetch origin
git rev-parse HEAD
git rev-parse origin/main
git status --porcelain=v1
git rev-list --left-right --count HEAD...origin/main
```

A synchronized clean checkout reports identical commit IDs and `0 0`. The deployed checkout may list approved untracked runtime files but should have no modified tracked files.

## Firmware build environment

The canonical build and flash host is the Mac mini. The project uses ESP-IDF 5.3.x-era APIs and the installed Espressif environment on that machine.

The esp-sr `movemodel.py` helper invokes `python` literally. `firmware/.local_bin/python` is a compatibility link to Python 3 and should be at the front of `PATH` when needed.

Example:

```bash
cd /Users/pmartis/herVoice/firmware
export IDF_PATH="$HOME/esp/esp-idf"
export PATH="$PWD/.local_bin:$IDF_PATH/tools:$PATH"
. "$IDF_PATH/export.sh"
idf.py build
```

Do not treat command exit alone as a successful build. Require the explicit `Project build complete` line and all release artifacts.

### Durable build invariants

`firmware/sdkconfig.defaults` must retain:

- ESP32-S3 target
- 240 MHz CPU
- 16 MB flash
- OCT/OPI PSRAM mode at 80 MHz
- 16 KB main-task stack
- 1 kHz FreeRTOS tick
- four static Wi-Fi RX buffers
- custom partition table

QUAD PSRAM is not valid for this V2 board. The generic boot phrase `Spiram mode: SPI` is not evidence that QUAD should be selected.

### Firmware credentials

Store Wi-Fi values only in gitignored `firmware/sdkconfig`:

```text
CONFIG_HERVOICE_WIFI_SSID="..."
CONFIG_HERVOICE_WIFI_PASSWORD="..."
```

Never add those values to `sdkconfig.defaults`, documentation, shell history, or commits.

## Release images and partition map

| Partition | Offset | Size | Image |
|---|---:|---:|---|
| Bootloader | `0x0` | generated | `bootloader.bin` |
| Partition table | `0x8000` | generated | `partition-table.bin` |
| OTA metadata | `0xD000` | `0x2000` | `ota_data_initial.bin` |
| Factory application | `0x10000` | `0x300000` | `hervoice.bin` |
| Wake models, SPIFFS | `0x310000` | `0x500000` | `srmodels.bin` |
| Storage, SPIFFS | `0x810000` | `0x7F0000` | runtime data |

Before relaying or flashing a release, calculate and compare SHA-256 hashes for all five images at the source and destination.

Example flash operation:

```bash
idf.py -p /dev/cu.usbmodem31101 flash
```

Every esptool image must report `Hash of data verified`.

### ROM recovery and monitor behavior

If normal flashing cannot connect:

1. Hold **BOOT**.
2. Tap **RST**.
3. Release **BOOT**.
4. Flash.
5. If the serial output remains at `waiting for download`, tap **RST** once without BOOT.

Opening the ESP32-S3 USB Serial/JTAG port can reset the device into download mode. Start the monitor, then press RST once to obtain a fresh application boot.

After any esptool hard reset, physically power-cycle USB before testing touch; the CST816S may otherwise remain unresponsive.

## Post-flash acceptance gate

Do not accept a firmware build merely because it flashed. A clean boot must show:

- PSRAM initialized in the expected OCT/OPI mode
- preferred two-second playback ring allocated in PSRAM
- Wake engine started: microWakeWord `charlie` model loaded from the app binary (or WakeNet `wn9_sophia_tts` created when built with `CONFIG_HERVOICE_WAKE_ENGINE_WAKENET`)
- audio capture and playback initialized
- boot tone audible
- touch controller identified after a real power cycle
- Wi-Fi `GOT_IP`
- network, capture, wake detection, touch, and UI tasks running
- stable Ready state
- no abort, reset loop, panic, watchdog, or repeated allocation failure

Then test:

1. Idle wake phrase activates exactly once.
2. Tap activates manually.
3. Normal speech transitions Ready → Listening → Processing → Speaking → Ready.
4. English and Slovak replies use the correct language and voice.
5. A Charlie question opens a wake-free follow-up turn.
6. Tap during a session cancels within the receive-poll interval.
7. Swipe volume survives reboot.
8. TTS remains smooth while the 20 FPS halo is active.

## Critical firmware invariants

### Boot order

Keep WakeNet model loading before I2S initialization (WakeNet build; the microWakeWord model is embedded and has no runtime load step):

```text
NVS → UI → esp_srmodel_init → wake_init → audio_init → test tone
    → capture task → wake detection task → touch → Wi-Fi → network task
```

Model creation after I2S DMA has caused intermittent model-parser corruption and `LoadProhibited` resets.

### Shared microphone ring

The wake engine and session upload read the same capture ring. Suspend wake detection before a voice session and resume it only after the TTS tail and a short delay. If both readers run concurrently, each receives fragments of the utterance. The microWakeWord engine additionally ignores the first ~7.4 s of features after each start (`MIN_SLICES_BEFORE_DETECTION`), so detection benches must wait before the first playback after a reset.

### Wake engine build toggle and retraining

The wake engine is a Kconfig choice in `components/wake/Kconfig`:
`CONFIG_HERVOICE_WAKE_ENGINE_MWW` (default) builds the microWakeWord engine
(`wakeword_mww.cpp`, vendored `components/micro_wake_word/`), and
`CONFIG_HERVOICE_WAKE_ENGINE_WAKENET` builds the original ESP-SR WakeNet path.
The active model binary lives at `components/wake/model/charlie_ss_quant.tflite`
and is embedded via `target_add_binary_data` in the wake component CMakeLists;
it must be registered after `idf_component_register`.

Retraining runs offline (LLM box, CPU-only). Pipeline: ElevenLabs + piper
synthesis, playback through a speaker, re-record through the puck mic array,
clean clips, `microwakeword` feature generation and training, export of
`stream_state_internal_quant.tflite`, replace the model file, rebuild and flash.
Model input is `(1, 3, 40)` int8 features, output `(1, 1)` uint8 probability;
the firmware feeds 3 new feature frames per inference at a 10 ms step.

Two traps found the hard way:

1. Call `set_features_step_size()` **before** `setup()` — the frontend config
   captures the step at setup time, and a step of 0 leaves the window buffer
   frozen: features flatten to -128 and probability pins at 0.
2. `model_train_eval` TFLite export asserts representative-spectrogram length
   divisible by the stride; the local repo copy slices instead of asserting.

Benching: play held-out clips through the room speaker at moderate volume —
over-loud corpus playback trains the model off the real acoustic range; the v3
corpus was rescaled to 80%. Parse serial live for the wake event, and wait
about 12 s after each detection before the next clip so the engine re-arms.
Record both recall and a 3-minute idle false-positive run at the target cutoff.

### VAD before gain

Compute VAD RMS on raw samples. Apply the 10× gain only after speech starts and only for STT upload. Running VAD on amplified ambient noise prevents silence termination and forces every turn to the 10-second cap.

### Display traffic versus audio

Keep QSPI at 80 MHz, transfer queue depth at 1, synchronous flush-ready signaling, DMA-capable internal LVGL buffers, 20 FPS halo pacing, and dirty-checking. Aggressive redraws or hidden full-screen GIF decoding can starve PSRAM/audio traffic and produce choppy TTS.

### Playback memory

Allocate the large playback ring in PSRAM, then copy each chunk through internal RAM before I2S DMA. Capability flags are requirements, not alternatives: asking for both `MALLOC_CAP_SPIRAM` and `MALLOC_CAP_INTERNAL` does not mean “either.”

### Wi-Fi

Keep power saving disabled and reconnect forever. Do not delay inside an event callback; use a helper task. Do not restore large static RX-buffer counts without rechecking internal-RAM headroom.

## Backend environment

Create a virtual environment under the deployed checkout and install both bridge and service dependencies. Dependency files currently use minimum versions rather than a lockfile, so record resolved versions for reproducible releases.

The root `.env` is loaded by the bridge unit and should be loaded by the installed TTS unit. It must remain `0600`, owned by the service account, and untracked.

Representative names only—never place real values in documentation:

```dotenv
HERVOICE_HOST=0.0.0.0
HERVOICE_PORT=8765
WHISPER_SERVER_URL=http://127.0.0.1:15556
PIPER_SERVER_URL=http://127.0.0.1:15555

HERMES_API_ENABLED=1
HERMES_API_URL=http://192.168.1.100:8642/v1/chat/completions
HERMES_API_KEY=<protected>
HERMES_LLM_PROVIDER=custom
HERMES_LLM_MODEL=qwen3.8-flash-next-unsloth-ud-iq4_xs-v31
HERMES_DISABLE_THINKING=1
HERVOICE_CONTEXT_FILE=/home/foscam/herVoice-private/voice_context.md
HISTORY_IDLE_RESET_S=180

LLM_SERVER_URL=http://127.0.0.1:8081
LLM_MODEL=qwen3.8-flash-next-unsloth-ud-iq4_xs-v31
LLM_API_KEY=<protected>

ELEVENLABS_API_KEY=<protected>
ELEVENLABS_SK_VOICE_ID=<protected account voice id>
ELEVENLABS_MODEL=eleven_multilingual_v2
```

The checked-in environment example is historical and must not be treated as authoritative until it is aligned with `backend/config.py` and the service implementations.

## Systemd deployment

The active deployment consists of:

- `hervoice-stt.service`
- `hervoice-tts.service`
- `hervoice-bridge.service`

The units execute `/home/foscam/herVoice/venv/bin/python` from `/home/foscam/herVoice`. The bridge starts after STT and TTS but does not currently declare hard `Requires=` dependencies.

After installing or changing a unit:

```bash
sudo systemctl daemon-reload
sudo systemctl restart hervoice-stt hervoice-tts hervoice-bridge
systemctl is-active hervoice-stt hervoice-tts hervoice-bridge
systemctl cat hervoice-tts
```

The installed STT unit runs on the GPU through the drop-in `deploy/hervoice-stt-gpu.conf`, installed as `/etc/systemd/system/hervoice-stt.service.d/zz-gpu.conf`. It needs cuDNN 9 on `LD_LIBRARY_PATH` (here `/home/foscam/hervoice-gpu-libs`) and cuBLAS from the system CUDA install. Remove the drop-in and run `daemon-reload` to return to CPU. The model shares the GPU with Qwen; measure free VRAM with `nvidia-smi` before adding anything else to it.

Verify the installed unit, not just the repository template. In particular, confirm that TTS has an `EnvironmentFile` entry for the root `.env` before expecting ElevenLabs variables to exist.

## Health and smoke checks

### STT

The STT service contract is:

```text
POST http://127.0.0.1:15556/transcribe?language=auto&sample_rate=16000
Body: complete WAV bytes or raw s16le PCM,
      or a multipart form with fields file and language (what backend/stt.py sends)
```

`language` is `auto`, `en` or `sk`. `auto` returns English only when English is Whisper's first choice, otherwise Slovak, and adds `lang_probs` to the response.

`GET /health` returns the status and the device in use (`cuda` or `cpu`). Requests under 1000 bytes are rejected.

### TTS

```text
POST http://127.0.0.1:15555/synthesize
Content-Type: application/json
{"text":"Hello","lang":"en"}
```

Validate the returned data as WAV, not only HTTP 200. Check channel count, sample width, sample rate, duration, peak/RMS, and that the waveform does not end at a clipped DC rail.

ElevenLabs may return MP3 despite a PCM request. The service detects compressed audio and uses ffmpeg to decode it to 16 kHz mono s16le before wrapping WAV.

### Local model

Strata health is available on the local model service. The expected loaded model is `qwen3.8-flash-next-unsloth-ud-iq4_xs-v31`, with authentication enabled. Do not print the key while probing.

### Bridge

Confirm port 8765 is listening and the startup journal reports the Hermes agent API backend and a non-zero household context size. A startup line naming direct local LLM means the Hermes key/configuration was unavailable.

## Manual E2E client

`tools/e2e/test_client.py` sends a 16-bit mono WAV or raw PCM to the bridge, measures first-audio and total latency, and saves the returned 16 kHz mono WAV.

Use it to verify transport and audio production, but understand its limits:

- no transcript assertion
- no provider/model assertion
- no language assertion
- no response-content assertion
- no WAV quality assertion
- skips `REPLY_META`
- cannot record a second follow-up turn on the same socket

These limitations should be closed with automated integration tests.

## Latency debugging

Read bridge journal `LATENCY` lines. Diagnose the longest stage rather than retuning unrelated components:

| Field/stage | Likely subsystem |
|---|---|
| audio upload | device VAD, trailing silence, network upload |
| STT | faster-whisper service/model; about 0.07 s on the GPU, seconds if it fell back to CPU |
| LLM first content | Hermes gateway (its SQLite commit setting, see below), model queue, tool loop, prompt size |
| LLM total | response length and model generation |
| TTS first audio | sentence boundary and selected TTS engine |
| tail | playback ring and DMA drain |
| first-audio time | end-to-end user-perceived latency |

Reference figures from bridge-injected clips on 2026-11-24: knowledge questions 0.7-1.3 s to first audio, home questions with one tool call 1.8-2.8 s. The first turn after a pause or a gateway restart is several seconds slower. These depend on the Hermes NAS running `database.synchronous: NORMAL`; at the SQLite default each turn spends an extra 1-2 s in database commits on that host's spinning disks.

The Speaking UI state is entered only when the first `PCM_DOWN` arrives; a visual state alone is not proof that useful audio was queued or played.

## Dataset collection

Production-microphone collection mode is driven by `/tmp/hervoice_record_state.json`. Active mode bypasses STT/LLM/TTS, plays cue tones, and writes WAV files to `dataset/raw/`. Treat voice samples as private data and keep them untracked unless the user explicitly approves publication.

## Known issues and development priorities

### Correctness and reliability

1. **Keep the first turn after a pause warm.** It is several seconds slower than later turns.
2. **Load TTS secrets in the checked-in unit.** Add and verify `EnvironmentFile=-/home/foscam/herVoice/.env`.
3. **Implement a real Slovak TTS failure fallback** or explicitly document speech loss when ElevenLabs is unavailable.
4. **Harden frame validation.** Bound payload lengths; validate protocol version, exact session payload length, declared format, and even PCM byte counts; always consume or reject surplus bytes safely.
5. **Make mute functional.** Apply the touch mute state to playback/amplifier behavior and define whether capture continues.
6. **Language is connection-scoped.** Conversation state resets deliberately after 180 s of silence; the language lock still ends with the TCP connection.

### Testing

1. Add STT contract integration tests (raw body, multipart, language decision).
2. Add English/Slovak normalization and language-lock tests.
3. Assert voice requests use `provider=custom`, Qwen 3.8 Flash and a conversation session id, and carry no resent history.
4. Exercise `REPLY_META`, a second turn on the same socket, silence timeout, and five-turn cap.
5. Validate generated WAV format and signal sanity.
6. Add cancellation and truncated/disconnected-socket tests.
7. Pin Python dependencies or generate a reproducible lock/constraints file.

### Product work

1. ~~Train and deploy a custom **Charlie** wake-word model~~ — done: microWakeWord `charlie` (v3 corpus, 7/8 recall / 0 FP per 3 min idle at cutoff 0.97); Sophia and tap remain as evaluation controls.
2. Retain disputed-turn audio with bounded, privacy-aware retention to diagnose Whisper hallucinations.
3. Remove temporary full-screen display diagnostics and debug logs after confirming no remaining dependency on them.
4. Improve Slovak follow-up detection for statement-form questions without punctuation.
5. Instrument firmware-side wake, VAD-start, VAD-end, connect, and upload timestamps.
6. Implement VAD-driven barge-in and then full-duplex capture using the V2 hardware's AEC path.

## Documentation maintenance rule

When behavior changes, update documentation in the same commit:

- topology or model routing → `README.md` and `docs/architecture.md`
- frame or session behavior → `docs/protocol.md`
- display pins/init/timing → `docs/display_bringup.md`
- build, deploy, tests, or known defects → this file

Do not leave historical experiments phrased as current invariants. When preserving a failed approach is useful, label it clearly as historical and state the current source-of-truth implementation.
