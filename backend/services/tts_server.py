"""
herVoice TTS service — Kokoro-82M (EN) + Piper sk_SK-lili (SK), CPU.

POST /synthesize   JSON: {"text": "...", "lang": "en"|"sk"}
GET  /health
Returns: WAV bytes (native engine rate; client resamples).
"""
import argparse
import io
import logging
import os
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
        self._el_key      = os.environ.get("ELEVENLABS_API_KEY", "")
        self._el_voice_id = os.environ.get("ELEVENLABS_SK_VOICE_ID", "")
        self._el_model    = os.environ.get("ELEVENLABS_MODEL",
                                           "eleven_multilingual_v2")

    def load(self) -> None:
        # Kokoro (English) — lazy import; heavy
        t0 = time.perf_counter()
        from kokoro import KPipeline
        self._kokoro = KPipeline(lang_code="a")  # American English
        log.info("Kokoro loaded in %.1fs", time.perf_counter() - t0)

        # Slovak: ElevenLabs (cloud, "Suzie") is primary; Piper only as an
        # offline fallback when the key/voice are not configured.
        if self._el_key and self._el_voice_id:
            log.info("Slovak TTS: ElevenLabs voice=%s model=%s",
                     self._el_voice_id, self._el_model)
        else:
            log.warning("ELEVENLABS_API_KEY / ELEVENLABS_SK_VOICE_ID not set; "
                        "loading Piper sk_SK-lili as Slovak fallback")
            self._load_piper()

    def _load_piper(self) -> None:
        t0 = time.perf_counter()
        import piper
        voice_path = os.environ.get("PIPER_VOICE_PATH",
                                    "/home/foscam/herVoice/piper-voices/sk_SK-lili-medium.onnx")
        self._piper = piper.PiperVoice.load(voice_path)
        self._piper_rate = self._piper.config.sample_rate
        log.info("Piper sk_SK-lili-medium loaded in %.1fs (rate=%d)",
                 time.perf_counter() - t0, self._piper_rate)

    def synth_en(self, text: str) -> tuple[bytes, int]:
        """Kokoro -> (wav_bytes, sample_rate=24000)."""
        # kokoro 0.9.x yields Result objects; .audio is a torch tensor (24kHz mono)
        parts = [r.audio.cpu().numpy().astype(np.float32) for r in self._kokoro(text, voice="af_heart")]
        samples = np.concatenate(parts) if parts else np.zeros(0, np.float32)
        pcm = (np.clip(samples, -1.0, 1.0) * 32767).astype(np.int16)
        return _wav(pcm, 24000), 24000

    def synth_sk(self, text: str) -> tuple[bytes, int]:
        """Slovak -> (wav_bytes, rate). ElevenLabs primary, Piper fallback."""
        if self._el_key and self._el_voice_id:
            try:
                return self._synth_sk_elevenlabs(text)
            except Exception as e:
                log.warning("ElevenLabs SK failed (%s); Piper fallback", e)
                if self._piper is None:
                    raise
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wf:
            self._piper.synthesize_wav(text, wf)
        return buf.getvalue(), self._piper_rate

    def _synth_sk_elevenlabs(self, text: str) -> tuple[bytes, int]:
        """Call the ElevenLabs TTS API; return 16 kHz mono WAV bytes.
        Retries once and validates the response: ElevenLabs sometimes
        returns a non-PCM error body (or truncated PCM) on an HTTP 200,
        which used to crash np.frombuffer and drop the Slovak reply."""
        import urllib.request
        import json as _json
        import time as _time
        import subprocess
        url = ("https://api.elevenlabs.io/v1/text-to-speech/"
               f"{self._el_voice_id}")
        body = _json.dumps({
            "text": text,
            "model_id": self._el_model,
            "voice_settings": {"stability": 0.5, "similarity_boost": 0.75},
            "output_format": "pcm_16000",
        }).encode()
        last_err = None
        for attempt in (1, 2):
            try:
                req = urllib.request.Request(
                    url, data=body, method="POST",
                    headers={"xi-api-key": self._el_key,
                             "Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=20) as r:
                    if r.status != 200:
                        raise RuntimeError("ElevenLabs HTTP %d" % r.status)
                    raw = r.read()
                if len(raw) < 500:
                    raise RuntimeError(
                        "ElevenLabs returned only %d bytes: %r"
                        % (len(raw), raw[:120]))
                # eleven_multilingual_v2 ignores output_format and returns
                # MP3 (audio/mpeg, ID3/Lavf) even for pcm_16000/pcm_24000.
                # Reading the compressed bytes as raw int16 gave a truncated,
                # noisy clip with a DC rail. Detect the format; decode MP3 ->
                # 16 kHz mono s16le with ffmpeg (always present on the box).
                is_mp3 = raw[:3] == b"ID3" or (raw[0] & 0xFF) == 0xFF
                if is_mp3:
                    p2 = subprocess.run(
                        ["ffmpeg", "-hide_banner", "-loglevel", "error",
                         "-i", "pipe:0", "-ar", "16000", "-ac", "1",
                         "-f", "s16le", "pipe:1"],
                        input=raw, capture_output=True, timeout=30)
                    if p2.returncode != 0 or not p2.stdout:
                        raise RuntimeError("ffmpeg decode failed rc=%s: %s"
                                           % (p2.returncode, p2.stderr[:200]))
                    pcm_bytes = p2.stdout
                else:
                    pcm_bytes = raw
                    if len(pcm_bytes) % 2:
                        pcm_bytes = pcm_bytes[:-1]
                pcm = np.frombuffer(pcm_bytes, dtype=np.int16)
                if len(pcm) < 800:
                    raise RuntimeError("decoded PCM too short: %d samples" % len(pcm))
                return _wav(pcm, 16000), 16000
            except Exception as e:
                last_err = e
                log.warning("ElevenLabs SK attempt %d failed: %s", attempt, e)
                _time.sleep(0.6 * attempt)
        raise RuntimeError("ElevenLabs SK TTS failed: %s" % last_err)

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
