# herVoice binary protocol

## Scope

herVoice uses a small framed protocol over TCP between the ESP32-S3 endpoint and `hervoice-bridge`. One TCP connection represents one wake-word conversation. The connection can carry multiple turns when Charlie requests a follow-up, but it closes when the conversation ends or is cancelled; it is not a permanently connected device channel.

The protocol assumes a trusted home network. It provides no encryption, endpoint authentication, replay protection, or integrity layer beyond TCP.

## Framing

Every frame consists of a five-byte header followed by the declared payload:

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 4 | payload length | unsigned 32-bit little-endian |
| 4 | 1 | frame type | unsigned byte |
| 5 | N | payload | frame-specific bytes |

Zero-length payloads are valid. Implementations must read exactly five header bytes and then exactly the declared payload length.

```text
+--------------------------+------+-------------------+
| payload_length_le (4 B)  | type | payload (N bytes) |
+--------------------------+------+-------------------+
```

Protocol version is currently `0x01`.

## Frame types

| Value | Symbol | Direction | Payload | Purpose |
|---:|---|---|---|---|
| `0x00` | `FRAME_SESSION_START` | device → server | 10 bytes | Start one turn and declare audio format |
| `0x01` | `FRAME_PCM_UP` | device → server | PCM bytes | Microphone audio |
| `0x02` | `FRAME_PCM_UP_END` | device → server | empty | End microphone upload |
| `0x10` | `FRAME_PCM_DOWN` | server → device | PCM bytes | Synthesized response audio |
| `0x11` | `FRAME_PCM_DOWN_END` | server → device | empty | Definitive end of response |
| `0x12` | `FRAME_REPLY_META` | server → device | exactly 1 byte | Whether another user reply is expected |
| `0x20` | `FRAME_ERROR` | primarily server → device | code + optional UTF-8 | Abort/error report |

## Session Start (`0x00`)

The payload is packed and always 10 bytes:

| Offset | Size | Field | Encoding/current value |
|---:|---:|---|---|
| 0 | 1 | `proto_version` | `0x01` |
| 1 | 4 | `device_id` | uint32 little-endian |
| 5 | 2 | `sample_rate` | uint16 little-endian; currently 16000 |
| 7 | 1 | `bit_depth` | currently 16 |
| 8 | 1 | `channels` | currently 1 |
| 9 | 1 | `flags` | currently 0 |

Firmware sends a new Session Start at the beginning of every turn, including wake-free follow-up turns on the existing socket.

### Current validation limitation

At the audited implementation, version, payload length, and declared format are not comprehensively rejected when invalid. Treat the 10-byte version-1, 16 kHz, mono, 16-bit format as mandatory. Future hardening should enforce it before allocating or consuming audio.

## Uplink audio (`0x01`, `0x02`)

`FRAME_PCM_UP` carries raw signed 16-bit little-endian mono PCM without a WAV header. The device normally sends 800 samples, or 1600 bytes, per frame; shorter even-sized frames can occur after short ring reads.

The device sends `FRAME_PCM_UP_END` with an empty payload when VAD ends the utterance or the turn reaches its maximum duration.

The bridge currently caps accumulated upload bytes at 320000, equivalent to 10 seconds at 16 kHz mono s16le. This cap is fixed and is not recomputed from the format declared in Session Start.

## Downlink audio (`0x10`, `0x11`)

`FRAME_PCM_DOWN` carries raw signed 16-bit little-endian mono PCM. The firmware consumes a downlink frame in chunks of at most 512 bytes and places samples into the playback ring; the audio task duplicates mono to stereo for I2S output.

Payload size must be even. The current implementation relies on a trusted server rather than rejecting every malformed size.

`FRAME_PCM_DOWN_END` has an empty payload and is the definitive response terminator. It follows all downlink audio and any Reply Meta frame.

The endpoint drains the playback ring and DMA tail before completing a normal turn.

## Reply Meta (`0x12`)

Reply Meta has exactly one payload byte:

| Value | Meaning |
|---:|---|
| `0` | Final answer; close this conversation after playback |
| `1` | Charlie expects another user utterance |

Reply Meta is sent after the final `PCM_DOWN` frame and before `PCM_DOWN_END`.

The payload must remain exactly one byte. Current firmware reads the first byte only and does not consume surplus bytes; a longer payload would desynchronize framing.

## Error (`0x20`)

The backend encodes Error as:

```text
byte 0     error code
bytes 1..  optional UTF-8 message
```

Current backend paths generally use code `1` for a generic error. The older conceptual taxonomy of separate STT, LLM, and TTS codes is not consistently implemented and must not be treated as a stable API.

Firmware does not normally send Error frames. User cancellation is represented by closing the TCP connection, not by an Error or Cancel frame.

Keep Error payloads at or below 255 bytes with the current firmware. Its receiver truncates its local read and does not safely drain oversized surplus data.

## Normal one-turn flow

```text
Device                                      Bridge
  |                                            |
  |--- SESSION_START ------------------------->|
  |--- PCM_UP × N ---------------------------->|
  |--- PCM_UP_END ---------------------------->|
  |                                            |  STT → Hermes/Qwen → TTS
  |<-- PCM_DOWN × N ---------------------------|
  |<-- REPLY_META {0} -------------------------|
  |<-- PCM_DOWN_END ---------------------------|
  |                                            |
  +---------------- close socket --------------+
```

UI sequence is normally Ready → Listening → Processing → Speaking → Ready. Speaking begins only on receipt of the first PCM Down frame.

## Follow-up flow

```text
Device                                      Bridge
  |--- SESSION_START (turn 1) ---------------->|
  |--- PCM_UP × N / PCM_UP_END -------------->|
  |<-- PCM_DOWN × N ---------------------------|
  |<-- REPLY_META {1} -------------------------|
  |<-- PCM_DOWN_END ---------------------------|
  |       wait for playback tail               |
  |--- SESSION_START (turn 2, same socket) ---->|
  |--- PCM_UP × N / PCM_UP_END -------------->|
  |<-- PCM_DOWN × N ---------------------------|
  |<-- REPLY_META {0} -------------------------|
  |<-- PCM_DOWN_END ---------------------------|
  +---------------- close socket --------------+
```

Firmware permits at most five consecutive follow-up turns. Follow-up capture starts without WakeNet. Before VAD sees speech, the display presents Ready; it changes to Listening when speech begins.

If the user remains silent, the normal turn timeout produces an empty upload. The server returns an end response without another positive Reply Meta; firmware exits follow-up mode and closes the socket.

## Cancellation

There is no cancel frame. A short tap during a conversation sets a local cancellation flag. Receive waits poll at approximately 200 ms, then the endpoint returns to Ready and closes the TCP socket. The bridge observes EOF, connection reset, or broken pipe and abandons the session.

A future explicit cancel frame would need to define interaction with queued TTS, tool calls, and backend history before it can replace this behavior.

## Audio format

| Parameter | Current value |
|---|---|
| Encoding | signed PCM, 16-bit little-endian |
| Channels | mono on wire |
| Sample rate | 16000 Hz |
| Uplink gain | 10× with saturation after raw-domain VAD starts |
| Downlink chunking | bridge sends approximately 50 ms chunks |

No RIFF/WAV header appears in PCM Up or PCM Down payloads. The bridge adds WAV framing only when calling HTTP audio services.

## Connection and timeout behavior

- One TCP connection per wake-word conversation
- Same socket reused for follow-up turns only
- Device send timeout: 2 seconds
- Device receive timeout: 30 seconds
- Active response receive loop checks cancellation approximately every 200 ms
- Device turn cap: 10 seconds
- Bridge upload cap: 320000 bytes
- Follow-up cap: five turns

## Parser safety requirements

Protocol work should preserve these requirements:

1. Bound incoming payload sizes before allocation.
2. Validate Session Start length, version, sample rate, depth, and channels.
3. Require even PCM payload lengths.
4. Require Reply Meta length exactly one.
5. Consume or reject an entire Error payload; never truncate without draining.
6. Treat premature EOF as a connection close and break the server's per-connection loop.
7. Do not swallow `IncompleteReadError`, `ConnectionResetError`, or `BrokenPipeError`; doing so can create an infinite empty-session loop.
8. Keep PCM Down End as the definitive response terminator even when Reply Meta is present.

## Source locations

| Side | File |
|---|---|
| Shared firmware constants and structs | `firmware/components/net/include/protocol.h` |
| Firmware session state machine | `firmware/components/net/net_stream.c` |
| Backend frame codec | `backend/protocol.py` |
| Backend turn processing | `backend/session.py` |
| Backend connection/history loop | `backend/server.py` |
| Manual protocol client | `tools/e2e/test_client.py` |

The E2E client currently skips Reply Meta and does not submit a second turn. Extend it before using it as full follow-up protocol coverage.
