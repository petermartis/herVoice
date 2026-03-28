"""
STT module using faster-whisper.
Supports Slovak and English auto-detection.
"""
import asyncio
import io
import logging
import struct
import numpy as np
from typing import Optional

log = logging.getLogger("STT")

try:
    from faster_whisper import WhisperModel
    _HAS_FASTER_WHISPER = True
except ImportError:
    _HAS_FASTER_WHISPER = False
    log.warning("faster-whisper not installed, STT will be a stub")

import config


class WhisperSTT:
    def __init__(self):
        self._model: Optional["WhisperModel"] = None
        self._lock = asyncio.Lock()

    async def load(self) -> None:
        """Load Whisper model (blocking, runs in thread pool)."""
        if not _HAS_FASTER_WHISPER:
            log.warning("faster-whisper unavailable, using stub STT")
            return
        loop = asyncio.get_running_loop()
        self._model = await loop.run_in_executor(
            None,
            lambda: WhisperModel(
                config.WHISPER_MODEL,
                device=config.WHISPER_DEVICE,
                compute_type=config.WHISPER_COMPUTE,
            ),
        )
        log.info("Whisper model '%s' loaded on %s (%s)",
                 config.WHISPER_MODEL, config.WHISPER_DEVICE, config.WHISPER_COMPUTE)

    async def transcribe(self, audio_bytes: bytes, sample_rate: int = 16000) -> str:
        """
        Transcribe raw PCM bytes (16-bit little-endian mono) to text.
        Returns transcribed string (empty string on failure).
        """
        if not _HAS_FASTER_WHISPER or self._model is None:
            log.warning("STT model not loaded, returning stub transcript")
            return "hello"

        if len(audio_bytes) < 100:
            return ""

        loop = asyncio.get_running_loop()
        async with self._lock:
            transcript = await loop.run_in_executor(
                None,
                lambda: self._do_transcribe(audio_bytes, sample_rate),
            )
        return transcript

    def _do_transcribe(self, audio_bytes: bytes, sample_rate: int) -> str:
        # Convert PCM int16 LE bytes to float32 numpy array normalised to [-1, 1]
        samples = np.frombuffer(audio_bytes, dtype=np.int16).astype(np.float32) / 32768.0

        # Resample to 16000 Hz if needed (faster-whisper expects 16kHz)
        if sample_rate != 16000:
            import scipy.signal
            samples = scipy.signal.resample_poly(
                samples, 16000, sample_rate
            ).astype(np.float32)

        segments, info = self._model.transcribe(
            samples,
            language=config.WHISPER_LANGUAGE,   # None = auto-detect
            initial_prompt=config.WHISPER_INITIAL_PROMPT or None,
            vad_filter=True,
            vad_parameters={"min_silence_duration_ms": 300},
        )

        log.info("STT detected language: %s (prob=%.2f)", info.language, info.language_probability)

        text = " ".join(seg.text.strip() for seg in segments)
        return text.strip()
