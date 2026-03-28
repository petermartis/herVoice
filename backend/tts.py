"""
TTS module — calls Piper via subprocess, streams raw PCM output.
Piper writes 16-bit LE mono PCM to stdout.
"""
import asyncio
import logging
import shutil
from typing import AsyncIterator

log = logging.getLogger("TTS")

import config

CHUNK_BYTES = 1024  # ~32ms @ 16kHz 16-bit mono


class PiperTTS:
    def __init__(self):
        self._piper_path = shutil.which(config.PIPER_BINARY) or config.PIPER_BINARY

    async def synthesize(self, text: str, output_sample_rate: int = 16000) -> AsyncIterator[bytes]:
        """
        Synthesize text to speech using Piper.
        Yields raw PCM chunks (16-bit LE mono) suitable for sending to the device.
        """
        if not text.strip():
            return

        cmd = [
            self._piper_path,
            "--model",       config.PIPER_MODEL,
            "--output-raw",
            "--length-scale", "1.0",
            "--sentence-silence", "0.1",
        ]

        log.debug("Piper cmd: %s", " ".join(cmd))
        log.debug("TTS text: %r", text[:80])

        try:
            proc = await asyncio.create_subprocess_exec(
                *cmd,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
        except FileNotFoundError:
            log.error("Piper binary not found at: %s", self._piper_path)
            yield _stub_pcm(text, output_sample_rate)
            return

        # Feed text to piper stdin
        stdin_data = (text + "\n").encode("utf-8")
        proc.stdin.write(stdin_data)
        await proc.stdin.drain()
        proc.stdin.close()

        # Stream stdout in chunks
        try:
            while True:
                chunk = await proc.stdout.read(CHUNK_BYTES)
                if not chunk:
                    break
                # Resample if piper output rate != device rate
                if config.PIPER_SAMPLE_RATE != output_sample_rate:
                    chunk = _resample_pcm(chunk, config.PIPER_SAMPLE_RATE, output_sample_rate)
                if chunk:
                    yield chunk
        except asyncio.CancelledError:
            proc.kill()
            raise

        await proc.wait()
        stderr_out = await proc.stderr.read()
        if stderr_out:
            log.debug("Piper stderr: %s", stderr_out.decode(errors="replace")[:200])

        if proc.returncode != 0:
            log.warning("Piper exited with code %d", proc.returncode)


def _resample_pcm(pcm_bytes: bytes, src_rate: int, dst_rate: int) -> bytes:
    """Simple linear resampling of 16-bit LE mono PCM."""
    try:
        import numpy as np
        import scipy.signal
        samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32)
        resampled = scipy.signal.resample_poly(samples, dst_rate, src_rate)
        return resampled.astype(np.int16).tobytes()
    except Exception as e:
        log.warning("Resample failed: %s, returning original", e)
        return pcm_bytes


def _stub_pcm(text: str, sample_rate: int) -> bytes:
    """Return a short beep tone as PCM when Piper is unavailable."""
    import math
    import struct
    duration_s = min(len(text) * 0.05, 3.0)
    n_samples = int(sample_rate * duration_s)
    freq = 440.0
    samples = [
        int(16000 * math.sin(2 * math.pi * freq * i / sample_rate))
        for i in range(n_samples)
    ]
    return struct.pack(f"<{n_samples}h", *samples)
