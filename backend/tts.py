"""
TTS module — POSTs text to piper-server, receives WAV, yields raw PCM chunks.
POST /synthesize  JSON: {"text":"...","voice":"lili"|"libritts","speaker_id":886}
Returns: WAV bytes (22050 Hz for lili, 22050 Hz for libritts)
Device expects: 16000 Hz 16-bit mono PCM
"""
import io
import logging
import wave
from typing import AsyncIterator, Optional

log = logging.getLogger("TTS")

try:
    import aiohttp
    _HAS_AIOHTTP = True
except ImportError:
    _HAS_AIOHTTP = False
    log.warning("aiohttp not installed, TTS will be a stub")

import config

CHUNK_SAMPLES = 800   # 50ms @ 16kHz
CHUNK_BYTES   = CHUNK_SAMPLES * 2  # 16-bit


class PiperTTS:
    def __init__(self):
        self._session: Optional["aiohttp.ClientSession"] = None

    def _get_session(self) -> "aiohttp.ClientSession":
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session

    async def synthesize(self, text: str, lang: str = "sk",
                         output_sample_rate: int = 16000) -> AsyncIterator[bytes]:
        """
        Synthesize text via piper-server.
        Yields raw PCM chunks (16-bit LE mono, output_sample_rate Hz).
        lang: 'sk' → voice=lili, otherwise → voice=libritts + speaker_id
        """
        if not text.strip():
            return

        if not _HAS_AIOHTTP:
            log.warning("aiohttp unavailable, yielding stub PCM")
            yield _stub_pcm(text, output_sample_rate)
            return

        if lang == "sk":
            payload = {"text": text, "voice": config.PIPER_VOICE_SK}
        else:
            payload = {
                "text":       text,
                "voice":      config.PIPER_VOICE_EN,
                "speaker_id": config.PIPER_SPEAKER_ID_EN,
            }

        url = f"{config.PIPER_SERVER_URL.rstrip('/')}/synthesize"
        log.debug("TTS POST %s  voice=%s  text=%r", url, payload["voice"], text[:80])

        session = self._get_session()
        try:
            async with session.post(url, json=payload,
                                    timeout=aiohttp.ClientTimeout(total=30)) as resp:
                if resp.status != 200:
                    body = await resp.text()
                    log.error("TTS request failed: HTTP %d — %s", resp.status, body[:200])
                    return
                wav_bytes = await resp.read()
        except aiohttp.ClientError as e:
            log.exception("TTS HTTP error: %s", e)
            return

        # Parse WAV header to get the native sample rate
        try:
            with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
                src_rate    = wf.getframerate()
                n_channels  = wf.getnchannels()
                sampwidth   = wf.getsampwidth()
                pcm_bytes   = wf.readframes(wf.getnframes())
        except Exception as e:
            log.error("TTS WAV parse error: %s", e)
            return

        log.debug("TTS WAV: rate=%d ch=%d width=%d pcm_bytes=%d",
                  src_rate, n_channels, sampwidth, len(pcm_bytes))

        # Convert to mono 16-bit if needed
        if n_channels > 1 or sampwidth != 2:
            try:
                import numpy as np
                samples = np.frombuffer(pcm_bytes, dtype=np.int16)
                if n_channels > 1:
                    samples = samples.reshape(-1, n_channels).mean(axis=1).astype(np.int16)
                pcm_bytes = samples.tobytes()
            except Exception as e:
                log.warning("TTS channel/depth conversion failed: %s", e)

        # Resample to device rate if needed
        if src_rate != output_sample_rate:
            pcm_bytes = _resample_pcm(pcm_bytes, src_rate, output_sample_rate)

        # Yield in fixed-size chunks
        offset = 0
        while offset < len(pcm_bytes):
            chunk = pcm_bytes[offset:offset + CHUNK_BYTES]
            if chunk:
                yield chunk
            offset += CHUNK_BYTES

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()


def _resample_pcm(pcm_bytes: bytes, src_rate: int, dst_rate: int) -> bytes:
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
