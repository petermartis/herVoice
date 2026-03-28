"""Binary frame protocol for herVoice device <-> backend communication."""

import struct
import asyncio
from typing import Optional

# Frame types
FRAME_SESSION_START  = 0x00
FRAME_PCM_UP         = 0x01
FRAME_PCM_UP_END     = 0x02
FRAME_PCM_DOWN       = 0x10
FRAME_PCM_DOWN_END   = 0x11
FRAME_ERROR          = 0x20

PROTO_VERSION = 0x01
FRAME_HEADER_SIZE = 5  # 4 bytes length + 1 byte type


class FrameError(Exception):
    pass


async def read_exact(reader: asyncio.StreamReader, n: int) -> bytes:
    """Read exactly n bytes from reader, raising FrameError on short read."""
    data = await reader.readexactly(n)
    return data


async def recv_frame(reader: asyncio.StreamReader) -> tuple[int, bytes]:
    """
    Read one frame from the stream.
    Returns (frame_type, payload_bytes).
    Raises FrameError or asyncio.IncompleteReadError on connection issues.
    """
    header = await read_exact(reader, FRAME_HEADER_SIZE)
    length = struct.unpack_from("<I", header, 0)[0]  # little-endian uint32
    frame_type = header[4]

    if length > 0:
        payload = await read_exact(reader, length)
    else:
        payload = b""

    return frame_type, payload


async def send_frame(writer: asyncio.StreamWriter, frame_type: int, payload: bytes = b"") -> None:
    """Write one frame to the stream."""
    header = struct.pack("<IB", len(payload), frame_type)
    writer.write(header + payload)
    await writer.drain()


async def send_error(writer: asyncio.StreamWriter, message: str, code: int = 1) -> None:
    """Send an error frame."""
    payload = bytes([code]) + message.encode("utf-8")
    await send_frame(writer, FRAME_ERROR, payload)


def parse_session_start(payload: bytes) -> dict:
    """
    Parse Session Start payload (type 0x00).
    Returns dict with: proto_version, device_id, sample_rate, bit_depth, channels, flags
    """
    if len(payload) < 10:
        raise FrameError(f"Session Start payload too short: {len(payload)} bytes")
    proto_version = payload[0]
    device_id     = struct.unpack_from("<I", payload, 1)[0]
    sample_rate   = struct.unpack_from("<H", payload, 5)[0]
    bit_depth     = payload[7]
    channels      = payload[8]
    flags         = payload[9]
    return {
        "proto_version": proto_version,
        "device_id":     device_id,
        "sample_rate":   sample_rate,
        "bit_depth":     bit_depth,
        "channels":      channels,
        "flags":         flags,
    }
