"""
Per-connection session state machine.
Coordinates STT → LLM → TTS pipeline for one device session.
"""
import asyncio
import json
import logging
import re
import time
from enum import Enum, auto
from typing import Optional

from protocol import (
    recv_frame, send_frame, send_error, parse_session_start,
    FRAME_SESSION_START, FRAME_PCM_UP, FRAME_PCM_UP_END,
    FRAME_PCM_DOWN, FRAME_PCM_DOWN_END, FRAME_REPLY_META
)
import config

log = logging.getLogger("SESSION")


# Statement-form question detectors for the follow-up flag. The model
# sometimes phrases follow-ups as imperatives ("Tell me your city or
# ZIP code.") with no trailing '?'. A false positive only costs a 10 s
# self-closing listen window; a false negative costs a repeat wake word.
_FOLLOWUP_HINTS = re.compile(
    r"\b(tell me|let me know|please (provide|give|say|tell|confirm)|"
    r"what (city|location|zip|name|number|time|day|are you|do you|would you)|"
    r"which|can you|could you|would you like|do you want|shall i|should i|"
    r"your (city|location|zip|name|age|preference|choice|answer))\b",
    re.IGNORECASE,
)


def _looks_like_question(text: str) -> bool:
    """Heuristic: does this reply expect the user to answer?"""
    if not text:
        return False
    if text.strip().endswith("?"):
        return True
    return bool(_FOLLOWUP_HINTS.search(text))


def parse_llm_output(raw: str) -> tuple[str, bool]:
    """Parse the LLM JSON contract -> (spoken_text, expects_user_reply).

    Falls back to a '?' heuristic when the model ignored the JSON format,
    which errs on the safe side (a missed follow-up flag only costs one
    extra wake word, never a dropped answer).
    """
    txt = raw.strip()
    if txt.startswith("```"):
        txt = re.sub(r"^\s*```(?:json)?\s*|\s*```\s*$", "", txt).strip()
    candidates = [txt]
    m = re.search(r"\{.*\}", txt, re.DOTALL)
    if m and m.group(0) != txt:
        candidates.append(m.group(0))
    for cand in candidates:
        try:
            obj = json.loads(cand)
            if isinstance(obj, dict) and "reply" in obj:
                reply = str(obj.get("reply", "")).strip()
                followup = bool(obj.get("followup", False)) or _looks_like_question(reply)
                return reply, followup
        except (ValueError, TypeError):
            pass
    reply = raw.strip()
    return reply, bool(reply) and _looks_like_question(reply)


def strip_markdown(text: str) -> str:
    """Make a reply TTS-safe. Models habitually bold keywords with **...**
    despite the plain-text prompt, and TTS would literally say 'asterisk'.
    This is the enforcement point - never trust the model for this."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)  # [label](url) -> label
    text = re.sub(r"[*_#`~]+", "", text)                        # bold/italic/heading/code
    return re.sub(r"\s+", " ", text).strip()


# --- Streaming pipeline helpers -----------------------------------------
_PIPE_SENTINEL = object()

# A sentence boundary: . ! ? followed by whitespace (covers sentence case
# and list items ending in punctuation).
_SENT_BOUNDARY = re.compile(r"(?<=[.!?])\s+")
_RE_REPLY_KEY = re.compile(r'"reply"\s*:\s*"')


def _iter_reply_text(buf: str) -> str:
    """The spoken reply text visible in a (possibly partial) LLM output.

    Handles the JSON contract incrementally: everything between the
    opening quote of "reply" and the first unescaped closing quote.
    Plain-text fallback (model ignored JSON): the whole buffer, as long
    as no JSON brace has appeared yet.
    """
    m = _RE_REPLY_KEY.search(buf)
    if m:
        rest = buf[m.end():]
        i = 0
        while i < len(rest):
            if rest[i] == "\\":
                i += 2
                continue
            if rest[i] == '"':
                rest = rest[:i]
                break
            i += 1
        # A trailing lone backslash is an incomplete escape split across
        # deltas - do not surface it yet (it would otherwise re-escape the
        # next character retroactively).
        if rest.endswith("\\"):
            rest = rest[:-1]
        return rest
    if "{" not in buf:
        return buf.strip()
    return ""


class SessionState(Enum):
    INIT            = auto()
    RECEIVING_AUDIO = auto()
    STT             = auto()
    LLM             = auto()
    TTS             = auto()
    DONE            = auto()
    ERROR           = auto()


# Whisper confuses Slavic languages, and TTS voices exist only for EN/SK:
# every Slavic detection maps to the Slovak path.
_SLAVIC_TO_SK = {"sk", "sl", "hr", "sr", "bs"}


class Session:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                 stt, llm, tts, conversation_history: Optional[list] = None,
                 first_frame: Optional[tuple] = None,
                 sticky_lang: Optional[str] = None):
        self.reader  = reader
        self.writer  = writer
        self.stt     = stt
        self.llm     = llm
        self.tts     = tts
        self.history = conversation_history if conversation_history is not None else []
        # Session-start frame already read by the server (to learn
        # device_id before picking the per-device history) - replay it
        # in _handle_session_start if provided.
        self._first_frame = first_frame

        self.expects_reply = False
        self.state       = SessionState.INIT
        self.device_id   = 0
        self.sample_rate = config.DEVICE_SAMPLE_RATE
        self.audio_buf   = bytearray()
        self.lang        = "sk"   # detected language, threaded through pipeline
        # Conversation language inherited by follow-up turns on the same
        # connection (set by handle_client from the previous session).
        self.sticky_lang    = sticky_lang
        self.lang_confirmed = False  # True once STT produced a transcript
        # Latency measurement: stage marks in seconds relative to _t0
        # (session start). See _log_latency() for the line format.
        self._t0    = 0.0
        self._marks: dict[str, float] = {}

        peer = writer.get_extra_info("peername")
        self.peer_str   = f"{peer[0]}:{peer[1]}" if peer else "unknown"
        self.session_id = f"{self.peer_str}-{int(time.time())}"

    def _mark(self, name: str) -> None:
        self._marks[name] = time.perf_counter() - self._t0

    def _log_latency(self, note: str = "") -> None:
        """One line per turn: per-stage durations + FAT (first audio time)."""
        m = self._marks
        if not m:
            return  # session never got audio (idle follow-up wait)

        def f(x: float) -> str:
            return f"{x:.2f}" if x >= 0 else "  - "

        def dur(a: str, b: str) -> float:
            return m[b] - m[a] if a in m and b in m else -1.0

        line = (f"audio={f(m.get('audio', -1))} stt={f(dur('audio', 'stt'))} "
                f"llm_first={f(m.get('llm_first', -1))} "
                f"llm={f(dur('stt', 'llm'))} "
                f"FAT={f(m.get('tts_first', -1))} "
                f"tail={f(dur('llm', 'done'))} lang={self.lang}")
        log.info("[%s] LATENCY %s%s", self.session_id, line,
                 f"  ({note})" if note else "")

    async def run(self) -> None:
        t_start = time.perf_counter()
        self._t0 = t_start
        log.info("[%s] Session started", self.session_id)

        try:
            await self._handle_session_start()
            await self._receive_audio()

            t_audio_done = time.perf_counter()
            self._mark("audio")
            log.info("[%s] Audio received in %.2fs, bytes=%d",
                     self.session_id, t_audio_done - t_start, len(self.audio_buf))

            transcript, raw_lang = await self._run_stt()
            # Language policy: TTS voices exist for EN and SK only, and
            # Whisper confuses Slavic languages (Slovenian/Croatian/Serbian
            # often detected as hr/sr/sl). Normalize: all Slavic -> sk,
            # everything else -> en. Follow-up turns inherit the
            # conversation language (sticky_lang) - a one-word answer is
            # the worst case for detection.
            if self.sticky_lang:
                self.lang = self.sticky_lang
            else:
                self.lang = "sk" if raw_lang in _SLAVIC_TO_SK else "en"
            t_stt = time.perf_counter()
            self._mark("stt")
            log.info("[%s] STT done in %.2fs: lang=%s (raw=%s) %r",
                     self.session_id, t_stt - t_audio_done,
                     self.lang, raw_lang, transcript)

            if not transcript.strip():
                log.warning("[%s] Empty transcript, skipping LLM/TTS", self.session_id)
                self._log_latency("empty transcript")
                await send_frame(self.writer, FRAME_PCM_DOWN_END)
                return
            self.lang_confirmed = True

            await self._run_llm_tts(transcript)
            t_tts = time.perf_counter()
            self._log_latency()
            log.info("[%s] Pipeline done, total=%.2fs",
                     self.session_id, t_tts - t_start)

            self.state = SessionState.DONE

        except asyncio.IncompleteReadError:
            # Client went away: propagate so handle_client() closes the loop
            self._log_latency("client disconnected")
            log.info("[%s] Client disconnected mid-session", self.session_id)
            self.state = SessionState.ERROR
            raise
        except Exception as e:
            self._log_latency("session error")
            log.exception("[%s] Session error: %s", self.session_id, e)
            self.state = SessionState.ERROR
            try:
                await send_error(self.writer, str(e))
            except Exception:
                pass

    async def _handle_session_start(self) -> None:
        self.state = SessionState.INIT
        if self._first_frame is not None:
            frame_type, payload = self._first_frame
            self._first_frame = None
        else:
            frame_type, payload = await recv_frame(self.reader)
        if frame_type != FRAME_SESSION_START:
            raise ValueError(f"Expected Session Start (0x00), got 0x{frame_type:02x}")
        info = parse_session_start(payload)
        self.device_id   = info["device_id"]
        self.sample_rate = info["sample_rate"] or config.DEVICE_SAMPLE_RATE
        log.info("[%s] device_id=0x%08X sample_rate=%d",
                 self.session_id, self.device_id, self.sample_rate)

    async def _receive_audio(self) -> None:
        self.state = SessionState.RECEIVING_AUDIO
        self.audio_buf.clear()
        max_bytes = config.MAX_SESSION_AUDIO_BYTES

        while True:
            frame_type, payload = await recv_frame(self.reader)
            if frame_type == FRAME_PCM_UP:
                self.audio_buf.extend(payload)
                if len(self.audio_buf) > max_bytes:
                    log.warning("[%s] Audio buffer exceeded max, truncating", self.session_id)
                    self.audio_buf = self.audio_buf[:max_bytes]
            elif frame_type == FRAME_PCM_UP_END:
                break
            else:
                log.warning("[%s] Unexpected frame 0x%02x during audio receive",
                            self.session_id, frame_type)

    async def _run_stt(self) -> tuple[str, str]:
        """Returns (transcript_text, language_code)."""
        self.state = SessionState.STT
        # Follow-up turns force the known conversation language: skips the
        # detection pass and is immune to one-word mis-detection.
        return await self.stt.transcribe(
            bytes(self.audio_buf), self.sample_rate,
            language=self.sticky_lang or "auto")

    async def _run_llm_tts(self, transcript: str) -> None:
        """Streaming pipeline: LLM streams -> sentence splitter -> TTS per
        sentence -> PCM frames to the device. First audio arrives after the
        FIRST sentence, not the full reply (the FAT win).

        Producer/consumer over a queue: the LLM keeps generating (and
        sentence N+1 is synthesized) while sentence N is playing."""
        self.state = SessionState.LLM
        # The system prompt must always be present - a first session on a
        # connection could otherwise run without it (no JSON contract,
        # no plain-text rules).
        if not self.history or self.history[0].get("role") != "system":
            self.history.insert(0, {"role": "system", "content": config.LLM_SYSTEM_PROMPT})
        self.history.append({"role": "user", "content": transcript})

        q: asyncio.Queue = asyncio.Queue()
        result: dict = {}

        async def produce():
            buf = ""
            visible = ""
            pending = ""
            first_delta = False
            try:
                async for delta in self.llm.stream_reply(
                        self.history, lang=self.lang):
                    if not first_delta:
                        self._mark("llm_first")
                        first_delta = True
                    buf += delta
                    v = _iter_reply_text(buf)
                    if len(v) > len(visible):
                        pending += v[len(visible):]
                        visible = v
                        while True:
                            m = _SENT_BOUNDARY.search(pending)
                            if not m:
                                break
                            sent = strip_markdown(pending[:m.end()]).strip()
                            pending = pending[m.end():]
                            if sent:
                                await q.put(sent)
            finally:
                self._mark("llm")
                result["buf"] = buf
                # Flush the trailing fragment (last sentence may lack the
                # boundary whitespace before the stream ends).
                await q.put(strip_markdown(pending).strip())
                await q.put(_PIPE_SENTINEL)

        async def consume():
            nonlocal _played_any
            while True:
                item = await q.get()
                if item is _PIPE_SENTINEL:
                    break
                if not item:
                    continue
                self.state = SessionState.TTS
                async for chunk in self.tts.synthesize(
                        item, lang=self.lang,
                        output_sample_rate=self.sample_rate):
                    if not _played_any:
                        self._mark("tts_first")  # FAT: first audio frame
                        _played_any = True
                    await send_frame(self.writer, FRAME_PCM_DOWN, chunk)

        _played_any = False
        await asyncio.gather(produce(), consume())

        raw = result.get("buf", "")
        log.info("LLM raw: %r", raw[:400])
        reply, self.expects_reply = parse_llm_output(raw)
        reply = strip_markdown(reply)
        if not reply:
            reply = "Sorry, I did not catch that."

        # If nothing was streamed (empty/failed reply), speak the fallback
        # so the device never goes silent.
        if not _played_any:
            self.state = SessionState.TTS
            async for chunk in self.tts.synthesize(
                    reply, lang=self.lang,
                    output_sample_rate=self.sample_rate):
                self._mark("tts_first")
                _played_any = True
                await send_frame(self.writer, FRAME_PCM_DOWN, chunk)

        self.history.append({"role": "assistant", "content": reply})
        # Keep history bounded (system prompt + last 20 messages).
        # Truncate IN PLACE: self.history may be the shared per-device
        # list that handle_client() still holds a reference to.
        if len(self.history) > 21:
            del self.history[1:-20]
        log.info("[%s] LLM followup flag: %s", self.session_id, self.expects_reply)

        # Tell the device whether the LLM expects a reply (keeps the line
        # open for a follow-up) or the conversation turn is final.
        await send_frame(self.writer, FRAME_REPLY_META,
                         b"\x01" if self.expects_reply else b"\x00")
        self._mark("done")
        await send_frame(self.writer, FRAME_PCM_DOWN_END)
