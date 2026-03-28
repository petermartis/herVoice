"""
STT module — POSTs WAV audio to whisper-server (faster-whisper medium, CUDA float16).
POST /transcribe  multipart: file=audio.wav, language=auto|sk|en
Returns: {"text":"...","language":"sk","duration":2.3}
"""
import asyncio
import io
import logging
import struct
import wave
from typing import Optional, Tuple

log = logging.getLogger("STT")

try:
    import aiohttp
    _HAS_AIOHTTP = True
except ImportError:
    _HAS_AIOHTTP = False
    log.warning("aiohttp not installed, STT will be a stub")

import config


def _pcm_to_wav(pcm_bytes: bytes, sample_rate: int = 16000,
                channels: int = 1, sampwidth: int = 2) -> bytes:
    """Wrap raw PCM bytes in a WAV container."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sampwidth)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


class WhisperSTT:
    def __init__(self):
        self._session: Optional["aiohttp.ClientSession"] = None

    def _get_session(self) -> "aiohttp.ClientSession":
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session

    async def load(self) -> None:
        """No-op: whisper-server runs as a separate process."""
        log.info("STT: using whisper-server at %s", config.WHISPER_SERVER_URL)

    async def transcribe(self, audio_bytes: bytes,
                         sample_rate: int = 16000) -> Tuple[str, str]:
        """
        Transcribe raw PCM bytes (16-bit LE mono) to text via whisper-server.
        Returns (text, language) — language is 'sk', 'en', etc.
        Returns ("", "sk") on failure or empty audio.
        """
        if len(audio_bytes) < 100:
            return ("", "sk")

        if not _HAS_AIOHTTP:
            log.warning("aiohttp unavailable, returning stub transcript")
            return ("hello", "sk")

        wav_bytes = _pcm_to_wav(audio_bytes, sample_rate)

        url = f"{config.WHISPER_SERVER_URL.rstrip('/')}/transcribe"
        form = aiohttp.FormData()
        form.add_field("file", wav_bytes,
                       filename="audio.wav",
                       content_type="audio/wav")
        form.add_field("language", "auto")

        session = self._get_session()
        try:
            async with session.post(url, data=form,
                                    timeout=aiohttp.ClientTimeout(total=60)) as resp:
                if resp.status != 200:
                    body = await resp.text()
                    log.error("STT request failed: HTTP %d — %s", resp.status, body[:200])
                    return ("", "sk")
                data = await resp.json()
                text = data.get("text", "").strip()
                lang = data.get("language", "sk")
                duration = data.get("duration", 0)
                log.info("STT: %r  lang=%s  duration=%.1fs", text[:80], lang, duration)
                return (text, lang)
        except aiohttp.ClientError as e:
            log.exception("STT HTTP error: %s", e)
            return ("", "sk")
        except (KeyError, ValueError) as e:
            log.exception("STT response parse error: %s", e)
            return ("", "sk")

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()
