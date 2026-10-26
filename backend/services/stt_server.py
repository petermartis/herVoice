"""
herVoice STT service — faster-whisper (large-v3-turbo, CPU int8).

POST /transcribe   multipart form: file=audio (wav/raw), language=auto|en|sk
                    optional: sample_rate (default 16000)
GET  /health
Returns: {"text": "...", "language": "en", "duration": 2.3, "seconds": 1.1}
"""
import argparse
import io
import logging
import sys
import time
import wave

import numpy as np
from aiohttp import web
from faster_whisper import WhisperModel

log = logging.getLogger("STT")

SAMPLE_RATE = 16000


class STTService:
    def __init__(self, model_size: str, device: str, compute_type: str):
        log.info("Loading faster-whisper model %r (%s/%s)...", model_size, device, compute_type)
        t0 = time.perf_counter()
        self.model = WhisperModel(model_size, device=device, compute_type=compute_type, cpu_threads=16)
        log.info("Model loaded in %.1fs", time.perf_counter() - t0)

    def _to_mono_f32(self, audio: bytes, sample_rate: int) -> np.ndarray:
        """Accept WAV bytes or raw s16le mono. Returns float32 mono @ SAMPLE_RATE."""
        data = None
        src_rate = sample_rate
        try:
            with wave.open(io.BytesIO(audio), "rb") as wf:
                src_rate = wf.getframerate()
                n_ch = wf.getnchannels()
                width = wf.getsampwidth()
                raw = wf.readframes(wf.getnframes())
                if width == 2:
                    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                elif width == 4:
                    samples = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2147483648.0
                else:
                    raise ValueError(f"unsupported sample width {width}")
                if n_ch > 1:
                    samples = samples.reshape(-1, n_ch).mean(axis=1)
                data = samples
        except (wave.Error, EOFError, ValueError):
            # raw s16le mono fallback
            data = np.frombuffer(audio, dtype=np.int16).astype(np.float32) / 32768.0
        if src_rate != SAMPLE_RATE:
            idx = np.linspace(0.0, len(data) - 1, int(len(data) * SAMPLE_RATE / src_rate))
            data = np.interp(idx, np.arange(len(data)), data).astype(np.float32)
        return data

    def transcribe(self, audio: bytes, language: str | None, sample_rate: int = SAMPLE_RATE) -> dict:
        audio_f32 = self._to_mono_f32(audio, sample_rate)
        t0 = time.perf_counter()
        segments, info = self.model.transcribe(
            audio_f32,
            language=None if language in (None, "", "auto") else language,
            beam_size=1,
            vad_filter=True,
            vad_parameters={"min_silence_duration_ms": 300},
        )
        text = " ".join(s.text.strip() for s in segments).strip()
        return {
            "text": text,
            "language": info.language,
            "duration": round(info.duration, 2),
            "seconds": round(time.perf_counter() - t0, 2),
        }


async def handle_transcribe(request: web.Request) -> web.Response:
    svc: STTService = request.app["svc"]
    data = await request.read()
    language = request.query.get("language", "auto")
    try:
        sample_rate = int(request.query.get("sample_rate", SAMPLE_RATE))
    except ValueError:
        sample_rate = SAMPLE_RATE
    if len(data) < 1000:
        return web.json_response({"error": "audio too small"}, status=400)
    try:
        result = svc.transcribe(data, language, sample_rate)
    except Exception as e:
        log.exception("transcribe failed")
        return web.json_response({"error": str(e)}, status=500)
    return web.json_response(result)


async def handle_health(request: web.Request) -> web.Response:
    return web.json_response({"status": "ok"})


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=15556)
    ap.add_argument("--model", default="large-v3-turbo")
    ap.add_argument("--device", default="cpu")
    ap.add_argument("--compute", default="int8")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, stream=sys.stdout,
                        format="%(asctime)s %(levelname)s %(name)s %(message)s")
    app = web.Application()
    app["svc"] = STTService(args.model, args.device, args.compute)
    app.router.add_post("/transcribe", handle_transcribe)
    app.router.add_get("/health", handle_health)
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
