# herVoice

## Overview

herVoice is an ESP32-S3 based voice assistant in the style of Alexa. Wake word detection, voice activity detection (VAD), and a touch-screen UI all run on-device. When a wake word is detected, audio is streamed over TCP to a backend server that runs a full AI pipeline: Whisper for speech-to-text, Qwen 3.5 (via OpenClaw) for language model inference, and Piper for text-to-speech. The synthesised response audio is streamed back to the device and played through the built-in speaker.

---

## Architecture

```
┌──────────────────────────────────────┐     TCP     ┌────────────────────────────────┐
│  Waveshare ESP32-S3-Touch-LCD-1.85C  │◄───────────►│  Backend Server (Linux/Mac)     │
│                                      │             │                                │
│  • WakeNet wake word                 │  PCM audio  │  • Whisper STT (faster-whisper) │
│  • Energy-based VAD                  │  streaming  │  • Qwen 3.5 LLM (via OpenClaw) │
│  • LVGL 1.85" touch display          │             │  • Piper TTS                   │
│  • I2S mic + speaker                 │             │                                │
└──────────────────────────────────────┘             └────────────────────────────────┘
```

---

## Repository Structure

```
herVoice/
├── firmware/                   # ESP-IDF project for the ESP32-S3
│   ├── main/                   # Application source (wake word, VAD, TCP client, UI)
│   ├── components/             # Optional local components
│   ├── Kconfig.projbuild       # Project-level Kconfig options (Wi-Fi, server, audio)
│   └── CMakeLists.txt
├── backend/                    # Python asyncio TCP server
│   ├── server.py               # Main entry point; accepts device connections
│   ├── stt.py                  # Whisper STT wrapper (faster-whisper)
│   ├── llm.py                  # OpenClaw / OpenAI-compatible LLM client
│   ├── tts.py                  # Piper TTS subprocess wrapper
│   ├── protocol.py             # Frame encode/decode helpers
│   └── requirements.txt        # Python dependencies
└── docs/
    └── protocol.md             # Binary protocol specification
```

---

## Hardware Requirements

- **Waveshare ESP32-S3-Touch-LCD-1.85C** — ESP32-S3 with 1.85" capacitive touch LCD, onboard microphone, and speaker
- Product wiki and schematic: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C

---

## Prerequisites

### Firmware

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
- `espressif/esp-sr` component — install via the ESP Component Registry:
  ```bash
  idf.py add-dependency "espressif/esp-sr"
  ```
- LVGL component (`lvgl/lvgl` from the component registry or as a managed component)

### Backend

- Python 3.11+
- [faster-whisper](https://github.com/SYSTRAN/faster-whisper) — Whisper inference library
- [Piper TTS](https://github.com/rhasspy/piper) binary + a compatible `.onnx` voice model
- [OpenClaw](https://github.com/peterm/openclaw) running locally and serving Qwen 3.5 on the OpenAI-compatible API

---

## Quick Start

### Backend

```bash
cd backend
pip install -r requirements.txt

# Required environment variables
export PIPER_BINARY=/path/to/piper
export PIPER_MODEL=/path/to/voice.onnx
export OPENCLAW_BASE_URL=http://localhost:11434/v1
export OPENCLAW_MODEL=qwen2.5:3b

python server.py
```

The server listens on `0.0.0.0:9876` by default (override with `HERVOICE_HOST` / `HERVOICE_PORT`).

### Firmware

```bash
cd firmware

# Configure Wi-Fi credentials, server IP/port, and audio settings
idf.py menuconfig   # Navigate to "herVoice Configuration"

idf.py build flash monitor
```

Say the configured wake word. The device UI transitions to the listening state, streams audio to the backend, and plays back the synthesised response.

---

## Configuration

### Backend Environment Variables

| Variable              | Default                       | Description                                         |
|-----------------------|-------------------------------|-----------------------------------------------------|
| `HERVOICE_HOST`       | `0.0.0.0`                     | Interface the TCP server binds to                   |
| `HERVOICE_PORT`       | `9876`                        | TCP port the server listens on                      |
| `WHISPER_MODEL`       | `base`                        | faster-whisper model size (tiny/base/small/medium)  |
| `WHISPER_DEVICE`      | `cpu`                         | Inference device (`cpu` or `cuda`)                  |
| `OPENCLAW_BASE_URL`   | `http://localhost:11434/v1`   | Base URL of the OpenClaw / OpenAI-compatible API    |
| `OPENCLAW_MODEL`      | `qwen2.5:3b`                  | Model name to use for LLM inference                 |
| `PIPER_BINARY`        | *(required)*                  | Absolute path to the `piper` executable             |
| `PIPER_MODEL`         | *(required)*                  | Absolute path to the `.onnx` voice model file       |

### Firmware Kconfig Options

These options are exposed under **"herVoice Configuration"** in `idf.py menuconfig` and are defined in `firmware/Kconfig.projbuild`.

| Kconfig Symbol                  | Description                                              |
|---------------------------------|----------------------------------------------------------|
| `CONFIG_HERVOICE_WIFI_SSID`     | Wi-Fi network name                                       |
| `CONFIG_HERVOICE_WIFI_PASSWORD` | Wi-Fi password                                           |
| `CONFIG_HERVOICE_SERVER_HOST`   | Backend server IP address or hostname                    |
| `CONFIG_HERVOICE_SERVER_PORT`   | Backend server TCP port (default 9876)                   |
| `CONFIG_HERVOICE_SAMPLE_RATE`   | Microphone sample rate in Hz (default 16000)             |
| `CONFIG_HERVOICE_WAKE_WORD`     | WakeNet model slot to use (e.g. `hilexin`, `hiesp`)      |

---

## Protocol

Device-to-server and server-to-device communication uses a custom binary framing protocol over a persistent TCP connection. Each frame has a 5-byte header (`uint32 LE` length + `uint8` type) followed by a variable-length payload.

Full specification: [docs/protocol.md](docs/protocol.md)

---

## Development Roadmap

| Phase | Description                                                   | Status      |
|-------|---------------------------------------------------------------|-------------|
| 1     | Hardware bring-up — I2S microphone, speaker, and LVGL display | In progress |
| 2     | ESP-SR / WakeNet integration and wake word detection          | Planned     |
| 3     | Energy-based VAD and utterance segmentation                   | Planned     |
| 4     | TCP streaming client and PCM-DOWN playback                    | Planned     |
| 5     | Real backend integration — Whisper → Qwen → Piper             | Planned     |
| 6     | UX hardening, threshold tuning, and reliability testing       | Planned     |

---

## License

MIT
