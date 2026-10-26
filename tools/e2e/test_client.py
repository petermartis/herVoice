"""
herVoice E2E test client — simulates the ESP32 firmware protocol.

Usage: python test_client.py [--server 192.168.1.35:8765] [--audio file.wav|file.pcm] [--save reply.wav]
Sends SESSION_START, streams the audio as PCM-UP frames, waits for PCM-DOWN,
saves the reply and prints timing.
"""
import argparse
import socket
import struct
import sys
import time

import wave


def send_frame(sock, ftype: int, payload: bytes = b""):
    sock.sendall(struct.pack("<IB", len(payload), ftype) + payload)


def recv_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def recv_frame(sock):
    hdr = recv_exact(sock, 5)
    length, ftype = struct.unpack("<IB", hdr)
    payload = b""
    while len(payload) < length:
        payload += recv_exact(sock, length - len(payload))
    return ftype, payload


def load_pcm(path: str) -> tuple[bytes, int]:
    """Returns (s16le mono pcm, sample_rate)."""
    if path.endswith(".wav"):
        with wave.open(path, "rb") as wf:
            assert wf.getnchannels() == 1 and wf.getsampwidth() == 2, "need mono 16-bit WAV"
            return wf.readframes(wf.getnframes()), wf.getframerate()
    with open(path, "rb") as f:
        return f.read(), 16000


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="192.168.1.35:8765")
    ap.add_argument("--audio", required=True)
    ap.add_argument("--device-id", type=int, default=1)
    ap.add_argument("--save", default="reply.wav")
    args = ap.parse_args()

    pcm, rate = load_pcm(args.audio)
    host, port = args.server.split(":")

    t0 = time.perf_counter()
    sock = socket.create_connection((host, int(port)), timeout=30)
    print(f"connected in {time.perf_counter()-t0:.2f}s")

    # SESSION_START (10-byte payload)
    ss = struct.pack("<BIHBBB", 0x01, args.device_id, rate, 16, 1, 0)
    send_frame(sock, 0x00, ss)

    # Stream PCM in 1600-sample (100ms) frames like the firmware
    t_up = time.perf_counter()
    frame = 1600 * 2
    for i in range(0, len(pcm), frame):
        send_frame(sock, 0x01, pcm[i:i + frame])
    send_frame(sock, 0x02)
    print(f"uploaded {len(pcm)//2} samples in {time.perf_counter()-t_up:.2f}s")

    # Receive reply
    t_reply = None
    out = bytearray()
    sock.settimeout(120)
    while True:
        ftype, payload = recv_frame(sock)
        if ftype == 0x10:
            if t_reply is None:
                t_reply = time.perf_counter()
                print(f"first audio after {t_reply - t0:.2f}s total")
            out.extend(payload)
        elif ftype == 0x11:
            break
        elif ftype == 0x20:
            print(f"SERVER ERROR: code={payload[0]} {payload[1:].decode(errors=replace)}")
            sys.exit(1)
        else:
            print(f"unexpected frame 0x{ftype:02x}, skipping {len(payload)} bytes")

    with wave.open(args.save, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(16000)
        wf.writeframes(bytes(out))
    print(f"reply: {len(out)//2/16000:.1f}s audio -> {args.save}, "
          f"end-to-end {time.perf_counter()-t0:.2f}s")


if __name__ == "__main__":
    main()
