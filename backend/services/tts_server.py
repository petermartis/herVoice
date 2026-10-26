"""
herVoice TTS service — Kokoro-82M (EN) + Piper sk_SK-lili (SK), CPU.

POST /synthesize   JSON: {"text": "...", "lang": "en"|"sk"}
GET  /health
Returns: WAV bytes (native engine rate; client resamples).
"""
import argparse
import io
import logging
import sys
import threading
import time
import wave
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from aiohttp import web

log = logging.getLogger("TTS")


class TTSService:
    def __init__(self):
        self._lock = threading.Lock()
        self._kokoro = None
        self._piper = None
        self._piper_rate = 22050

    def load(self) -> None:
        # Kokoro (English) — lazy import; heavy
        t0 = time.perf_counter()
        from kokoro import KPipeline
        self._kokoro = KPipeline(lang_code="a")  # American English
        log.info("Kokoro loaded in %.1fs", time.perf_counter() - t0)

        # Piper (Slovak)
        t0 = time.perf_counter()
        import piper
        import os
        voice_path = os.environ.get("PIPER_VOICE_PATH", "/home/foscam/herVoice/piper-voices/sk_SK-lili-medium.onnx")
        self._piper = piper.PiperVoice.load(voice_path)
        self._piper_rate = self._piper.config.sample_rate
        log.info("Piper sk_SK-lili-medium loaded in %.1fs (rate=%d)",
                 time.perf_counter() - t0, self._piper_rate)

    def synth_en(self, text: str) -> tuple[bytes, int]:
        """Kokoro -> (wav_bytes, sample_rate=24000)."""
        audio = self._kokoro(text, voice="af_heart")
        # audio: iterable of torch tensors (24kHz mono)
        parts = [a.cpu().numpy().astype(np.float32) for a in audio]
        samples = np.concatenate(parts) if parts else np.zeros(0, np.float32)
        pcm = (np.clip(samples, -1.0, 1.0) * 32767).astype(np.int16)
        return _wav(pcm, 24000)

    def synth_sk(self, text: str) -> tuple[bytes, int]:
        """Piper -> (wav_bytes, native rate)."""
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wf:
            self._piper.synthesize(text, wf)
        return buf.getvalue(), self._piper_rate

    def synthesize(self, text: str, lang: str) -> bytes:
        with self._lock:  # engines are not thread-safe
            if lang == "sk":
                wav, _ = self.synth_sk(text)
            else:
                wav, _ = self.synth_en(text)
        return wav


def _wav(pcm: np.ndarray, rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm.tobytes())
    return buf.getvalue()


async def handle_synthesize(request: web.Request) -> web.Response:
    svc: TTSService = request.app["svc"]
    try:
        body = await request.json()
    except Exception:
        return web.Response(status=400, text="invalid JSON")
    text = (body.get("text") or "").strip()
    if not text:
        return web.Response(status=400, text="empty text")
    lang = body.get("lang", "en")
    t0 = time.perf_counter()
    loop = web.Application  # noqa
    import asyncio
    try:
        wav = await asyncio.get_running_loop().run_in_executor(
            request.app["pool"], svc.synthesize, text, lang)
    except Exception as e:
        log.exception("TTS failed")
        return web.json_response({"error": str(e)}, status=500)
    log.info("TTS lang=%s text=%r -> %d bytes in %.2fs",
             lang, text[:60], len(wav), time.perf_counter() - t0)
    return web.Response(body=wav, content_type="audio/wav")


async def handle_health(request: web.Request) -> web.Response:
    return web.json_response({"status": "ok"})


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15555)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                        format="%(asctime)s %(levelname)s %(name)s %(message)s")
    app = web.Application()
    app["pool"] = ThreadPoolExecutor(max_workers=2)
    svc = TTSService()
    t0 = time.perf_counter()
    svc.load()
    log.info("TTS engines ready in %.1fs", time.perf_counter() - t0)
    app["svc"] = svc
    app.router.add_post("/synthesize", handle_synthesize)
    app.router.add_get("/health", handle_health)
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
