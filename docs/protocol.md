# herVoice Binary Protocol Specification

## Overview

herVoice uses a lightweight binary framing protocol over a persistent TCP connection between the ESP32-S3 device and the backend server. Each message is a self-delimited frame consisting of a fixed 5-byte header followed by a variable-length payload. The protocol is designed for low-overhead, real-time audio streaming in both directions and carries session control, raw PCM audio, and error signalling.

---

## Frame Structure

Every frame on the wire has the following layout:

| Field       | Size    | Type                    | Description                          |
|-------------|---------|-------------------------|--------------------------------------|
| `length_le` | 4 bytes | uint32, little-endian   | Length of the payload in bytes       |
| `type`      | 1 byte  | uint8                   | Frame type identifier (see below)    |
| `payload`   | N bytes | type-dependent          | Frame payload; N = `length_le`       |

The header is always exactly 5 bytes. A `length_le` value of `0` is valid and indicates a frame with no payload (used for End frames).

---

## Frame Types

| Value  | Name            | Direction          | Description                                        |
|--------|-----------------|--------------------|----------------------------------------------------|
| `0x00` | Session Start   | Device → Server    | Opens a new voice session and negotiates parameters |
| `0x01` | PCM-UP          | Device → Server    | Raw PCM audio chunk captured from the microphone   |
| `0x02` | PCM-UP End      | Device → Server    | Signals the end of the user's utterance            |
| `0x10` | PCM-DOWN        | Server → Device    | Raw PCM audio chunk to be played back on speaker   |
| `0x11` | PCM-DOWN End    | Server → Device    | Signals the end of the assistant's audio response  |
| `0x20` | Error           | Bidirectional      | Error notification; payload contains an error code |

---

## Session Start Payload (type `0x00`)

The Session Start frame payload carries audio format negotiation and device identification. All multi-byte integers are little-endian.

| Field           | Size   | Type              | Description                                      |
|-----------------|--------|-------------------|--------------------------------------------------|
| `proto_version` | 1 byte | uint8             | Protocol version; currently `0x01`               |
| `device_id`     | 4 bytes| uint32, LE        | Unique identifier for the device                 |
| `sample_rate`   | 2 bytes| uint16, LE        | Audio sample rate in Hz (e.g. `0x3E80` = 16000)  |
| `bit_depth`     | 1 byte | uint8             | Bits per sample (e.g. `16`)                      |
| `channels`      | 1 byte | uint8             | Number of audio channels (e.g. `1` for mono)     |
| `flags`         | 1 byte | uint8             | Reserved for future use; set to `0x00`           |

Total payload size for Session Start: **10 bytes**.

---

## Session Flow

```
Device                          Server
  |                               |
  |---[Session Start 0x00]------->|
  |---[PCM-UP 0x01]-------------->|  (repeated)
  |---[PCM-UP End 0x02]---------->|
  |                               |--- Whisper STT
  |                               |--- Qwen LLM
  |                               |--- Piper TTS
  |<--[PCM-DOWN 0x10]-------------|  (repeated)
  |<--[PCM-DOWN End 0x11]---------|
  |                               |
```

1. The device detects the wake word and sends a **Session Start** frame.
2. The device streams microphone audio as a series of **PCM-UP** frames.
3. When VAD detects end-of-speech, the device sends a **PCM-UP End** frame.
4. The server transcribes the audio (Whisper), generates a response (Qwen), and synthesises speech (Piper).
5. The server streams the synthesised audio back as a series of **PCM-DOWN** frames.
6. The server sends **PCM-DOWN End** when the response audio is complete.

Either party may send an **Error** frame at any point to abort the session.

---

## Audio Format

| Parameter   | Value                                |
|-------------|--------------------------------------|
| Encoding    | 16-bit signed PCM, little-endian     |
| Channels    | Mono (1 channel)                     |
| Sample rate | 16000 Hz (default; set in Session Start) |
| Chunk size  | Implementation-defined; typically 512–4096 samples per PCM frame |

Both PCM-UP and PCM-DOWN frames carry raw interleaved samples with no additional framing or headers within the payload.

---

## Error Codes

The **Error** frame (`0x20`) carries a 1-byte payload containing one of the following error codes:

| Code   | Name            | Description                                      |
|--------|-----------------|--------------------------------------------------|
| `0x00` | Success         | No error (informational)                         |
| `0x01` | Generic Error   | Unspecified or unknown error                     |
| `0x02` | STT Failure     | Whisper speech-to-text transcription failed      |
| `0x03` | LLM Failure     | Language model inference failed                  |
| `0x04` | TTS Failure     | Piper text-to-speech synthesis failed            |

On receiving an Error frame, both sides should discard any buffered audio and return to the idle/listening state.
