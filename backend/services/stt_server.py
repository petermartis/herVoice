"""
herVoice STT service — faster-whisper (large-v3-turbo), GPU with CPU fallback.

POST /transcribe?language=auto|en|sk[&sample_rate=16000]
     body: the audio itself (WAV or raw s16le mono), or a multipart form
     with a `file` part and optional `language` / `sample_rate` fields.
GET  /health
Returns: {"text": "...", "language": "en"|"sk", "duration": 2.3, "seconds": 0.1,
          "lang_probs": {"en": 0.93, "sk": 0.01}, "device": "cuda"}

Language policy: the device is only ever spoken to in English or Slovak, so
"auto" is a two-way decision. Whisper scores all ~99 languages once: if its
first choice is English the clip is English, anything else is Slovak. The
transcript is then decoded in that language. Other languages are never returned.
"""
import argparse
import asyncio
import io
import logging
import sys
import threading
import time
import wave

import numpy as np
from aiohttp import web
from faster_whisper import WhisperModel

log = logging.getLogger("STT")

SAMPLE_RATE = 16000
LANGS = ("en", "sk")

# Style/vocabulary bias for Whisper decoding (initial_prompt). Not a hard
# dictionary - it strongly steers short noisy clips toward the words this
# home actually says instead of phonetic guesses from other languages.
_DOMAIN_PROMPTS = {
    # Must NOT end with a short standalone phrase: Whisper hallucinates the
    # prompt tail verbatim onto weak audio ("...Dakujem." became the literal
    # transcript for a gate question). Start and end with content-dense
    # multi-word sentences.
    "sk": ("Skontroluj, či je brána zatvorená a zavri bránu. Brána je teraz "
           "otvorená. Kto je dnes doma a kto je vonku? Vypni svetlá v "
           "obývačke a zhasni v kuchyni. Aká je teplota v spálni a koľko "
           "stupňov je vonku? Zamkni vchodové dvere a skontroluj garáž. "
           "Nastav časovač na päť minút a zruš časovač. "
           "Povedz mi predpoveď počasia aj na zajtra, aké bude počasie "
           "v Bratislave, bude pršať alebo snežiť?"),
    "en": ("Check if the gate is closed and close the gate. The gate is open "
           "right now. Who is home today and who is outside? Turn off the "
           "lights in the living room and the kitchen. What is the bedroom "
           "temperature and what is the outdoor temperature? Lock the front "
           "door and check the garage door too. What is the weather forecast "
           "for tomorrow in Bratislava, will it rain or snow?"),
}


class STTService:
    def __init__(self, model_size: str, device: str, compute_type: str):
        self.model_size = model_size
        self.device = device
        self._lock = threading.Lock()
        self._cpu_model = None
        try:
            self.model = self._load(device, compute_type)
        except Exception as e:
            if device == "cpu":
                raise
            # GPU memory is shared with the LLM: start on CPU rather than not at all.
            log.error("Loading on %s failed (%s) - falling back to CPU int8", device, e)
            self.device = "cpu"
            self.model = self._load("cpu", "int8")
        # First call initialises the kernels; do it now, not on a user's turn.
        t0 = time.perf_counter()
        self._run(self.model, np.zeros(SAMPLE_RATE, dtype=np.float32), "auto")
        log.info("Warm-up done in %.2fs on %s", time.perf_counter() - t0, self.device)

    def _load(self, device: str, compute_type: str) -> WhisperModel:
        log.info("Loading faster-whisper model %r (%s/%s)...", self.model_size, device, compute_type)
        t0 = time.perf_counter()
        model = WhisperModel(self.model_size, device=device, compute_type=compute_type, cpu_threads=16)
        log.info("Model loaded in %.1fs", time.perf_counter() - t0)
        return model

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
            data = np.frombuffer(audio[:len(audio) & ~1], dtype=np.int16).astype(np.float32) / 32768.0
        if src_rate != SAMPLE_RATE:
            idx = np.linspace(0.0, len(data) - 1, int(len(data) * SAMPLE_RATE / src_rate))
            data = np.interp(idx, np.arange(len(data)), data).astype(np.float32)
        return data

    @staticmethod
    def _run(model: WhisperModel, audio_f32: np.ndarray, language: str) -> tuple[str, str, dict, float, float, float]:
        probs: dict = {}
        if language not in LANGS:
            _, _, all_probs = model.detect_language(audio_f32)
            scores = dict(all_probs)
            probs = {lang: round(float(scores.get(lang, 0.0)), 4) for lang in LANGS}
            # English only when it is Whisper's first choice. Short Slovak clips
            # are often scored as Polish or Czech with Slovak itself near zero,
            # so comparing en against sk alone files them as English.
            language = "en" if all_probs and all_probs[0][0] == "en" else "sk"
        # Greedy decoding on 1-2 s room clips produced phonetic gibberish
        # ("Česomarete a Brana" for "či som zavrel bránu"): the language
        # token only biases the decode, it is not a dictionary. Beam search
        # + a domain initial_prompt keep output in the household lexicon.
        segments, info = model.transcribe(
            audio_f32, language=language, beam_size=5, vad_filter=False,
            condition_on_previous_text=False, repetition_penalty=1.15,
            initial_prompt=_DOMAIN_PROMPTS.get(language))
        segs = list(segments)
        text = " ".join(s.text.strip() for s in segs).strip()
        confidence = 0.0
        no_speech = 0.0
        if segs:
            confidence = sum(s.avg_logprob for s in segs) / len(segs)
            no_speech = max(getattr(s, "no_speech_prob", 0.0) for s in segs)
            # Prompt-echo hallucinations: on long ambient/no-speech clips
            # Whisper recites its initial_prompt verbatim (the exact prompt
            # weather question "answered" twice from 9-10 s empty clips).
            # Real speech, even noisy, keeps avg_logprob well above -0.9.
            if text and (confidence < -0.9 or no_speech > 0.7):
                log.warning("hallucination rejected (logprob=%.2f no_speech=%.2f): %r",
                            confidence, no_speech, text[:70])
                text = ""
        return text, language, probs, info.duration, round(confidence, 3), round(no_speech, 3)

    def transcribe(self, audio: bytes, language: str | None, sample_rate: int = SAMPLE_RATE) -> dict:
        audio_f32 = self._to_mono_f32(audio, sample_rate)
        language = (language or "auto").lower()
        t0 = time.perf_counter()
        used = self.device
        with self._lock:
            try:
                text, lang, probs, duration, confidence, no_speech = self._run(self.model, audio_f32, language)
            except Exception as e:
                if self.device == "cpu":
                    raise
                # e.g. CUDA out of memory mid-flight: answer this turn on CPU.
                log.error("GPU transcribe failed (%s) - retrying on CPU", e)
                if self._cpu_model is None:
                    self._cpu_model = self._load("cpu", "int8")
                text, lang, probs, duration, confidence, no_speech = self._run(self._cpu_model, audio_f32, language)
                used = "cpu"
        return {
            "text": text,
            "language": lang,
            "duration": round(duration, 2),
            "confidence": confidence,
            "no_speech_prob": no_speech,
            "seconds": round(time.perf_counter() - t0, 3),
            "lang_probs": probs,
            "device": used,
        }


async def _read_request(request: web.Request) -> tuple[bytes, str, str]:
    """Returns (audio, language, sample_rate) from a raw body or a multipart form."""
    language = request.query.get("language", "")
    sample_rate = request.query.get("sample_rate", "")
    if not request.content_type.startswith("multipart/"):
        return await request.read(), language, sample_rate
    audio = b""
    async for part in await request.multipart():
        if part.name == "file":
            audio = bytes(await part.read(decode=False))
        elif part.name == "language":
            language = language or (await part.text()).strip()
        elif part.name == "sample_rate":
            sample_rate = sample_rate or (await part.text()).strip()
    return audio, language, sample_rate


async def handle_transcribe(request: web.Request) -> web.Response:
    svc: STTService = request.app["svc"]
    data, language, rate = await _read_request(request)
    try:
        sample_rate = int(rate or SAMPLE_RATE)
    except ValueError:
        sample_rate = SAMPLE_RATE
    if len(data) < 1000:
        return web.json_response({"error": "audio too small"}, status=400)
    try:
        result = await asyncio.get_running_loop().run_in_executor(
            None, svc.transcribe, data, language, sample_rate)
    except Exception as e:
        log.exception("transcribe failed")
        return web.json_response({"error": str(e)}, status=500)
    log.info("%.2fs audio -> %s in %.3fs on %s probs=%s", result["duration"], result["language"],
             result["seconds"], result["device"], result["lang_probs"])
    return web.json_response(result)


async def handle_health(request: web.Request) -> web.Response:
    return web.json_response({"status": "ok", "device": request.app["svc"].device})


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
    app = web.Application(client_max_size=16 * 1024 * 1024)
    app["svc"] = STTService(args.model, args.device, args.compute)
    app.router.add_post("/transcribe", handle_transcribe)
    app.router.add_get("/health", handle_health)
    web.run_app(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
