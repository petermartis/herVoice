# herVoice

herVoice is the embedded voice endpoint for **Charlie**, the family-aware Hermes assistant. A Waveshare ESP32-S3 device performs wake-word detection, endpointing, touch handling, UI, audio capture, and playback. A Linux backend performs speech recognition, routes the turn through the Hermes agent, synthesizes speech, and streams audio back to the device.

The production system supports **English and Slovak only**. The language chosen by the first recognized utterance remains fixed for follow-up turns on the same voice conversation.

## Current production system

| Layer | Current implementation |
|---|---|
| Endpoint | Waveshare ESP32-S3-Touch-LCD-1.85C V2 |
| Wake word | ESP-SR WakeNet `wn9_sophia_tts`; tap is the manual trigger |
| Endpointing | Raw-RMS VAD in firmware, before 10× STT upload gain |
| Device transport | Framed TCP PCM, device → backend `192.168.1.35:8765` |
| Speech recognition | faster-whisper `large-v3-turbo`, CPU int8, service on `127.0.0.1:15556` |
| Agent | Hermes gateway on `192.168.1.100:8642` |
| Voice LLM route | Explicit local `qwen3.8-flash-next-unsloth-ud-iq4_xs-v31` through provider `custom` |
| Direct LLM fallback | Local Strata OpenAI-compatible endpoint on `127.0.0.1:8081` |
| English speech | Kokoro `af_heart`, 24 kHz, resampled for the device |
| Slovak speech | ElevenLabs multilingual TTS; optional Piper startup path when ElevenLabs is not configured |
| Conversation behavior | Per-device in-memory history, sentence-streamed TTS, wake-free follow-ups |

The active firmware wake word is **Sophia**, not Charlie. A custom Charlie model remains future work.

## System topology

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Waveshare ESP32-S3-Touch-LCD-1.85C V2                              │
│ WakeNet · raw-RMS VAD · 16 kHz PCM · LVGL · CST816 touch           │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ framed TCP, port 8765
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│ LLM host 192.168.1.35 — /home/foscam/herVoice                      │
│                                                                      │
│ hervoice-bridge       :8765   device protocol and session pipeline   │
│ hervoice-stt        127.0.0.1:15556  faster-whisper                  │
│ hervoice-tts        127.0.0.1:15555  Kokoro / ElevenLabs / Piper     │
│ Strata Qwen         127.0.0.1:8081   direct local fallback           │
└──────────────────────────────┬───────────────────────────────────────┘
                               │ OpenAI-compatible streaming API
                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Hermes NAS 192.168.1.100:8642                                      │
│ Charlie persona · memory · family context · skills · Homey/email     │
│ Explicit provider=custom, model=Qwen 3.8 Flash                       │
└──────────────────────────────────────────────────────────────────────┘
```

See [docs/architecture.md](docs/architecture.md) for the complete runtime design and [docs/protocol.md](docs/protocol.md) for the wire format.

## Voice-turn flow

1. WakeNet detects Sophia, or the user taps the display.
2. Firmware suspends WakeNet so it cannot consume samples from the same microphone ring during a session.
3. Firmware opens a TCP connection to the bridge and sends `SESSION_START`.
4. Raw-RMS VAD waits for speech, then sends amplified 16 kHz mono s16le audio in `PCM_UP` frames.
5. After 600 ms of trailing silence, or at the 10-second cap, firmware sends `PCM_UP_END`.
6. The bridge wraps audio as WAV and calls the STT service.
7. The bridge normalizes the conversation language to English or Slovak and sends the turn to the Hermes gateway.
8. Hermes runs Charlie with persona, memory, and allowed tools using local Qwen 3.8 Flash.
9. The bridge parses Charlie's JSON reply, starts TTS as complete sentences become available, and streams `PCM_DOWN` frames.
10. `REPLY_META` declares whether the assistant expects an answer; `PCM_DOWN_END` terminates playback.
11. If a reply is expected, the device keeps the same socket and records the next turn without another wake word. Follow-ups are capped at five.

## Hardware

### Board

- Waveshare ESP32-S3-Touch-LCD-1.85C **V2**
- ESP32-S3, 16 MB flash, 8 MB OCT/OPI PSRAM
- 360×360 ST77916 QSPI round LCD
- CST816S capacitive touch controller
- ES7210 microphone ADC and ES8311 speaker DAC
- TCA9554PWR GPIO expander

The V1 board has a different audio design and is not supported by this firmware.

### Authoritative pin map

| Function | Pin/address |
|---|---|
| I2C0 SDA / SCL | GPIO11 / GPIO10, 400 kHz |
| ES7210 / ES8311 / TCA9554 / CST816S | `0x40` / `0x18` / `0x20` / `0x15` |
| I2S MCLK / BCLK / WS | GPIO2 / GPIO48 / GPIO38 |
| I2S microphone DIN / speaker DOUT | GPIO39 / GPIO47 |
| Speaker amplifier enable | GPIO15 |
| LCD CLK / D0 / D1 / D2 / D3 | GPIO40 / 46 / 45 / 42 / 41 |
| LCD CS / TE / backlight | GPIO21 / GPIO18 / GPIO5 |
| LCD reset | TCA9554 EXIO2 |
| Touch interrupt / reset | GPIO4 / TCA9554 EXIO1 |

Important hardware and initialization invariants are documented in [docs/development.md](docs/development.md) and [docs/display_bringup.md](docs/display_bringup.md).

## Repository layout

```text
herVoice/
├── firmware/                    ESP-IDF endpoint firmware
│   ├── main/                    boot order and project configuration
│   └── components/
│       ├── audio/               codecs, I2S, capture/playback rings
│       ├── net/                 Wi-Fi, VAD, framed TCP conversation loop
│       ├── touch/               CST816S initialization and gestures
│       ├── ui/                  ST77916/LVGL state UI and audio halo
│       └── wake/                WakeNet integration
├── backend/
│   ├── server.py                asyncio TCP bridge and per-device history
│   ├── session.py               STT → LLM → sentence TTS pipeline
│   ├── protocol.py              frame encoding and decoding
│   ├── stt.py                   STT HTTP client
│   ├── llm.py                   Hermes streaming client + local fallback
│   ├── tts.py                   TTS HTTP client and resampling
│   ├── config.py                runtime configuration
│   └── services/
│       ├── stt_server.py        faster-whisper HTTP service
│       └── tts_server.py        Kokoro / ElevenLabs / Piper HTTP service
├── deploy/                      systemd unit templates
├── tools/
│   ├── e2e/test_client.py       manual protocol/latency client
│   └── mic_test/                standalone microphone diagnostic firmware
└── docs/
    ├── architecture.md          complete system and runtime overview
    ├── development.md           build, deploy, verify, and known issues
    ├── protocol.md              binary protocol specification
    └── display_bringup.md       verified display implementation
```

## Deployment locations

| Purpose | Location |
|---|---|
| Canonical development checkout | Mac mini: `/Users/pmartis/herVoice` |
| GitHub | `github.com/petermartis/herVoice`, branch `main` |
| NAS mirror | `/volume1/homes/hermes/herVoice` |
| Deployed backend | LLM host: `/home/foscam/herVoice` |
| Runtime secrets | `/home/foscam/herVoice/.env`, mode `0600`, never committed |

The Mac mini checkout is the canonical firmware build tree. Keep GitHub, the NAS mirror, and the deployed tracked files on the same commit. Runtime datasets, the virtual environment, `.env`, and local backups on the LLM host are intentionally untracked.

## Build and operations

The firmware currently targets ESP-IDF 5.3.x-era APIs. A release consists of five images:

| Offset | Image |
|---:|---|
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0xD000` | `ota_data_initial.bin` |
| `0x10000` | `hervoice.bin` |
| `0x310000` | `srmodels.bin` |

Build, flash, service deployment, environment configuration, health checks, and acceptance gates are in [docs/development.md](docs/development.md).

## Current status

Implemented and deployed:

- Audio codec bring-up and full-duplex I2S channel ownership
- WakeNet model loading and Sophia wake detection
- Raw-domain VAD and utterance endpointing
- Framed TCP upload and streamed playback
- Stable ST77916 display and CST816S touch
- Manual wake, local cancellation, volume gestures, and persistent volume
- English/Slovak STT normalization and conversation language locking
- Charlie persona through Hermes with local Qwen 3.8 Flash
- Sentence-streamed TTS and follow-up turns without a second wake word
- Kokoro English and ElevenLabs Slovak speech
- Per-stage latency logging and manual E2E client

Outstanding work and known defects are tracked explicitly in [docs/development.md](docs/development.md#known-issues-and-development-priorities). Important current items include STT client/server contract verification, true mute enforcement, custom Charlie wake-word deployment, protocol hardening, automated language/follow-up tests, and removal of temporary display diagnostics.

## Security

- Never commit Wi-Fi passwords, API keys, bearer tokens, cloned-voice identifiers tied to private accounts, or `.env` files.
- Firmware Wi-Fi credentials belong only in gitignored `firmware/sdkconfig`.
- Backend secrets belong only in `/home/foscam/herVoice/.env` with mode `0600`.
- Logs and documentation must name environment variables, never their values.
- The device protocol currently assumes a trusted network and has no transport encryption or authentication; do not expose port 8765 to an untrusted network.

## License

MIT
