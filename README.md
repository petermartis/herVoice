# herVoice

## Overview

herVoice is an ESP32-S3 based voice assistant in the style of Alexa. Wake word detection, voice activity detection (VAD), and a touch-screen UI all run on-device. When a wake word is detected, audio is streamed over TCP to a backend server that runs a full AI pipeline: Whisper for speech-to-text, Qwen 3.5 (via OpenClaw) for language model inference, and Piper for text-to-speech. The synthesised response audio is streamed back to the device and played through the built-in speaker.

---

## Architecture

```
┌─────────────────────────────────────────┐     TCP     ┌────────────────────────────────┐
│   Waveshare ESP32-S3-Touch-LCD-1.85C    │◄───────────►│  Backend Server (Linux/Mac)     │
│   (V2 — ES7210 mic ADC, ES8311 DAC)     │             │                                │
│                                         │  PCM audio  │  • Whisper STT (faster-whisper) │
│  • WakeNet wake word (esp-sr)           │  streaming  │  • Qwen 3.5 LLM (via OpenClaw) │
│  • Energy-based VAD                     │             │  • Piper TTS                   │
│  • LVGL 1.85" QSPI touch display        │             │                                │
│  • ES7210 4-ch mic ADC (I2C + I2S)      │             └────────────────────────────────┘
│  • ES8311 speaker DAC (I2C + I2S)       │
└─────────────────────────────────────────┘
```

---

## Hardware

### Board

**Waveshare ESP32-S3-Touch-LCD-1.85C — V2 revision**

> The V2 board uses codec ICs (ES7210 ADC + ES8311 DAC) for audio, not raw I2S microphones.
> V1 boards used PCM5101A DAC + direct PDM/I2S MEMS mics — the firmware is **not** compatible with V1.

- Product wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C
- ESP32-S3 with 8 MB PSRAM, 16 MB Flash
- 1.85" 360×360 ST77916 QSPI round LCD with CST816 capacitive touch
- TCA9554 I2C GPIO expander (controls LCD backlight/reset)
- 2× MEMS microphones → ES7210 4-channel ADC codec
- ES8311 DAC codec → power amplifier

### GPIO Map

#### I2C Bus (shared — SDA=GPIO11, SCL=GPIO10)

| Signal    | GPIO | Device(s)                                          |
|-----------|------|----------------------------------------------------|
| SDA       | 11   | ES7210 mic ADC, ES8311 DAC, TCA9554 LCD expander   |
| SCL       | 10   | ES7210 mic ADC, ES8311 DAC, TCA9554 LCD expander   |

I2C addresses: ES7210 = `0x40`, ES8311 = `0x18` (ES8311_ADDRRES_0), TCA9554 = `0x20`

#### I2S Bus (full-duplex — one peripheral owns all clock lines)

| Signal | GPIO | Direction | Connected to         |
|--------|------|-----------|----------------------|
| MCLK   | 2    | OUT       | ES7210 + ES8311      |
| BCK    | 48   | OUT       | ES7210 + ES8311      |
| WS     | 38   | OUT       | ES7210 + ES8311      |
| DIN    | 39   | IN        | ES7210 (mic → ESP32) |
| DOUT   | 47   | OUT       | ES8311 (ESP32 → spkr)|

MCLK = 256 × sample_rate (4.096 MHz at 16 kHz). TX channel is enabled before RX so MCLK is stable when the ES7210 starts clocking.

#### Audio Power Amplifier

| Signal | GPIO | Notes                                              |
|--------|------|----------------------------------------------------|
| PA_EN  | 15   | Drive HIGH to unmute the amplifier; default LOW    |

#### LCD / Display

| Signal     | GPIO | Notes                         |
|------------|------|-------------------------------|
| QSPI CS    | TCA9554 | Via I2C expander           |
| QSPI SCLK  | —    | ST77916 QSPI controller       |
| QSPI D0–D3 | —    | ST77916 QSPI 4-bit data bus   |
| Backlight  | TCA9554 | Via I2C expander           |

#### Touch Controller

| Signal | GPIO     | Notes                              |
|--------|----------|------------------------------------|
| INT    | via I2C  | CST816 on shared I2C bus           |
| RST    | TCA9554  | Via I2C expander                   |

---

## Repository Structure

```
herVoice/
├── firmware/                           # ESP-IDF v5.3.2 project
│   ├── main/
│   │   ├── main.c                      # Startup — init order, task launch
│   │   ├── idf_component.yml           # Managed component dependencies
│   │   └── Kconfig.projbuild           # Wi-Fi, server, audio Kconfig options
│   ├── components/
│   │   ├── audio/                      # I2S + codec init, capture/playback tasks
│   │   │   ├── audio_capture.c         # ES7210 init, full-duplex I2S, RMS logging
│   │   │   ├── audio_playback.c        # ES8311 init, PA_EN, test tone, mono→stereo
│   │   │   ├── ring_buffer.c           # Lock-free single-producer/consumer ring buf
│   │   │   └── include/audio.h
│   │   ├── esp_lcd_st77916/            # Vendored ST77916 driver v2.0.2 (see note below)
│   │   ├── net/                        # Wi-Fi init, TCP stream task
│   │   ├── touch/                      # CST816 capacitive touch driver
│   │   ├── ui/                         # LVGL display, state machine
│   │   └── wake/                       # WakeNet / esp-sr integration
│   ├── .local_bin/
│   │   └── python -> python3           # Symlink: workaround for esp-sr build script
│   └── CMakeLists.txt
├── backend/                            # Python asyncio TCP server
│   ├── server.py                       # Main entry point
│   ├── stt.py                          # Whisper STT (faster-whisper)
│   ├── llm.py                          # OpenClaw / OpenAI-compatible LLM client
│   ├── tts.py                          # Piper TTS subprocess wrapper
│   ├── protocol.py                     # Frame encode/decode helpers
│   └── requirements.txt
├── tools/
│   └── mic_test/                       # Standalone ESP-IDF project — mic diagnostics
│       └── main/main.c                 # I2S-only capture; prints RMS every 500 ms
└── docs/
    └── protocol.md                     # Binary framing protocol specification
```

---

## Audio Subsystem

### Codec Overview

| Codec  | Role       | I2C Addr | IDF Component           |
|--------|------------|----------|-------------------------|
| ES7210 | Mic ADC    | `0x40`   | `espressif/es7210 ^1.0` |
| ES8311 | Speaker DAC| `0x18`   | `espressif/es8311 ^1.0` |

### Initialisation Sequence (`audio_init`)

1. **Ring buffers** allocated (capture: 500 ms; playback: ~500 ms)
2. **ES7210** configured over I2C — 16 kHz, MCLK ratio 256, I2S standard format, 16-bit, MIC_BIAS 2.87 V, gain 36 dB
3. **Full-duplex I2S channel** created with `i2s_new_channel(&cfg, &tx, &rx)` so a single peripheral owns MCLK=GPIO2; no clock conflict
4. Both TX and RX initialised in Philips/standard I2S mode, stereo, 16-bit
5. **TX enabled first** so MCLK is running when ES7210 begins clocking data
6. RX enabled; ES8311 configured over I2C (same MCLK source)
7. **PA_EN** (GPIO15) driven HIGH to unmute the power amplifier
8. **440 Hz boot beep** played as a speaker confidence check

### Capture Task (`audio_capture.c`)

- Reads stereo frames from DMA (L = MIC1, R = MIC2)
- Averages L+R → mono 16-bit PCM, writes to capture ring buffer
- Logs RMS every second:
  - `RMS = 0` → silent (check ES7210 wiring)
  - `RMS < 50` → very quiet / background noise
  - `RMS 50–500` → OK, ambient
  - `RMS > 500` → loud / speech

Typical ambient RMS with the board on a desk: **150–300**. Speech at normal distance: **500–1000+**.

### Playback Task (`audio_playback.c`)

- Reads mono PCM from playback ring buffer
- Duplicates L=R → stereo before writing to I2S DMA
- `audio_play_frames(buf, n)` — non-blocking write to ring buffer
- `audio_play_test_tone(freq_hz, ms)` — synchronous sine tone (blocks until queued + drains)
- `audio_playback_flush()` — blocks until ring buffer drains

---

## Build

### Prerequisites

- **ESP-IDF v5.3.2** installed at `~/esp/esp-idf`
- Xtensa toolchain at `~/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20240530/`
- Python env at `~/.espressif/python_env/idf5.3_py3.14_env/`

### Build Command

The `esp-sr` component's `movemodel.py` script calls `python` (not `python3`). A symlink in `.local_bin/` fixes this; prepend it to PATH before building:

```bash
export IDF_PATH=~/esp/esp-idf
export PATH="/path/to/firmware/.local_bin:$IDF_PATH/tools:$HOME/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20240530/xtensa-esp-elf/bin:$PATH"
export PYTHON="$HOME/.espressif/python_env/idf5.3_py3.14_env/bin/python3"
"$PYTHON" "$IDF_PATH/tools/idf.py" build
```

### Flash

```bash
"$PYTHON" "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodem31101 flash
```

### Monitor (USB Serial/JTAG quirk)

Opening the serial port on an ESP32-S3 USB Serial/JTAG interface resets the chip into download mode (`boot:0x2`). Use this workflow:

```bash
# 1. Start monitor in the background via `script` to avoid immediate download-mode reset
script -q /tmp/hvlog.txt "$PYTHON" "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodem31101 monitor

# 2. Wait for "waiting for download" to appear, then press the physical RESET button on the board
# 3. The device boots normally and log output flows
# 4. Press Ctrl+] to exit; read the log at /tmp/hvlog.txt
```

---

## Configuration

### Firmware Kconfig (`idf.py menuconfig` → "herVoice Configuration")

| Symbol                          | Default      | Description                                   |
|---------------------------------|--------------|-----------------------------------------------|
| `CONFIG_HERVOICE_WIFI_SSID`     | `"MyNetwork"`| Wi-Fi SSID                                    |
| `CONFIG_HERVOICE_WIFI_PASSWORD` | `""`         | Wi-Fi password                                |
| `CONFIG_HERVOICE_SERVER_HOST`   | `""`         | Backend server IP or hostname                 |
| `CONFIG_HERVOICE_SERVER_PORT`   | `9876`       | Backend server TCP port                       |
| `CONFIG_HERVOICE_SAMPLE_RATE`   | `16000`      | Audio sample rate in Hz                       |
| `CONFIG_HERVOICE_WAKE_WORD`     | —            | WakeNet model slot (e.g. `wn9_sophia_tts`)    |

### Backend Environment Variables

| Variable            | Default                     | Description                                        |
|---------------------|-----------------------------|----------------------------------------------------|
| `HERVOICE_HOST`     | `0.0.0.0`                   | Interface the TCP server binds to                  |
| `HERVOICE_PORT`     | `9876`                      | TCP port                                           |
| `WHISPER_MODEL`     | `base`                      | faster-whisper model size (tiny/base/small/medium) |
| `WHISPER_DEVICE`    | `cpu`                       | Inference device (`cpu` or `cuda`)                 |
| `OPENCLAW_BASE_URL` | `http://localhost:11434/v1` | OpenClaw / OpenAI-compatible API base URL          |
| `OPENCLAW_MODEL`    | `qwen2.5:3b`                | LLM model name                                     |
| `PIPER_BINARY`      | *(required)*                | Absolute path to the `piper` binary                |
| `PIPER_MODEL`       | *(required)*                | Absolute path to the `.onnx` voice model           |

### Backend Quick Start

```bash
cd backend
pip install -r requirements.txt

export PIPER_BINARY=/path/to/piper
export PIPER_MODEL=/path/to/voice.onnx
export OPENCLAW_BASE_URL=http://localhost:11434/v1
export OPENCLAW_MODEL=qwen2.5:3b

python server.py
```

---

## Protocol

Device-to-server and server-to-device communication uses a custom binary framing protocol over a persistent TCP connection. Each frame has a 5-byte header (`uint32 LE` length + `uint8` type) followed by a variable-length payload.

Full specification: [docs/protocol.md](docs/protocol.md)

---

## Implementation Notes

### Vendored `esp_lcd_st77916`

The managed component `espressif/esp_lcd_st77916` has two incompatible codebases:
- **v1.x** (from esp-bsp) — works with IDF 5.3; wrong init sequence for this board → blank screen
- **v2.x** (from esp-iot-solution) — correct init sequence; requires IDF ≥ 5.4

`firmware/components/esp_lcd_st77916/` vendors v2.0.2 from esp-iot-solution with two modifications:
1. `include(package_manager)` / `cu_pkg_define_version()` removed from `CMakeLists.txt` (not available outside the component manager)
2. Version macros (`ESP_LCD_ST77916_VER_*`) injected via `target_compile_definitions` instead

`espressif/esp_lcd_st77916` is intentionally absent from `idf_component.yml` and `dependencies.lock`.

### `esp-sr` Python Wrapper

`esp-sr`'s `movemodel.py` calls `python` literally. On systems where only `python3` exists, the build fails. The fix is a symlink:

```bash
mkdir -p firmware/.local_bin
ln -s $(which python3) firmware/.local_bin/python
```

Prepend `firmware/.local_bin` to `PATH` before every build (shown in the build command above).

### Wake Word Model

The `wn9_sophia_tts` model binary must be flashed to the `model` partition at offset `0x310000`. If the partition is empty, `wake_init()` logs a warning and skips detection — the rest of the firmware continues normally.

```bash
# Flash just the model partition after a full flash
"$PYTHON" -m esptool --chip esp32s3 -p /dev/cu.usbmodem31101 \
  write_flash 0x310000 build/srmodels/srmodels.bin
```

---

## Microphone Diagnostic Tool

`tools/mic_test/` is a minimal standalone ESP-IDF project that initialises only the I2S microphone (no speaker, no codecs, no display) and prints audio statistics every 500 ms. Use it to confirm raw I2S data before bringing up the full firmware.

```bash
cd tools/mic_test
"$PYTHON" "$IDF_PATH/tools/idf.py" build flash monitor
```

Expected output when working:
```
I (500) MIC_TEST: RMS=187  min=-1024  max=+1102  (512 frames)
```

Expected output when wiring is broken:
```
I (500) MIC_TEST: RMS=0  min=0  max=0  — ALL ZEROS (check wiring)
```

---

## Development Roadmap

| Phase | Description                                                             | Status         |
|-------|-------------------------------------------------------------------------|----------------|
| 1     | Hardware bring-up — ES7210/ES8311 codecs, I2S, LVGL display, touch      | **Complete**   |
| 2     | ESP-SR / WakeNet integration and wake word detection                    | In progress    |
| 3     | Energy-based VAD and utterance segmentation                             | Planned        |
| 4     | TCP streaming client and PCM playback of server response                | Planned        |
| 5     | Full backend integration — Whisper → Qwen → Piper                       | Planned        |
| 6     | UX hardening, threshold tuning, and reliability testing                 | Planned        |

---

## License

MIT
