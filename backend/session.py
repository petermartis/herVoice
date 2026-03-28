"""
Per-connection session state machine.
Coordinates STT → LLM → TTS pipeline for one device session.
"""
import asyncio
import io
import logging
import time
from enum import Enum, auto
from typing import Optional

from protocol import (
    recv_frame, send_frame, send_error, parse_session_start,
    FRAME_SESSION_START, FRAME_PCM_UP, FRAME_PCM_UP_END,
    FRAME_PCM_DOWN, FRAME_PCM_DOWN_END
)
import config

log = logging.getLogger("SESSION")


class SessionState(Enum):
    INIT           = auto()
    RECEIVING_AUDIO = auto()
    STT            = auto()
    LLM            = auto()
    TTS            = auto()
    DONE           = auto()
    ERROR          = auto()


class Session:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                 stt, llm, tts, conversation_history: list):
        self.reader  = reader
        self.writer  = writer
        self.stt     = stt
        self.llm     = llm
        self.tts     = tts
        self.history = conversation_history

        self.state      = SessionState.INIT
        self.device_id  = 0
        self.sample_rate = config.DEFAULT_SAMPLE_RATE
        self.audio_buf  = bytearray()

        peer = writer.get_extra_info("peername")
        self.peer_str = f"{peer[0]}:{peer[1]}" if peer else "unknown"
        self.session_id = f"{self.peer_str}-{int(time.time())}"

    async def run(self) -> None:
        t_start = time.perf_counter()
        log.info("[%s] Session started", self.session_id)

        try:
            await self._handle_session_start()
            await self._receive_audio()

            t_audio_done = time.perf_counter()
            log.info("[%s] Audio received in %.2fs, bytes=%d",
                     self.session_id, t_audio_done - t_start, len(self.audio_buf))

            transcript = await self._run_stt()
            t_stt = time.perf_counter()
            log.info("[%s] STT done in %.2fs: %r",
                     self.session_id, t_stt - t_audio_done, transcript)

            if not transcript.strip():
                log.warning("[%s] Empty transcript, skipping LLM/TTS", self.session_id)
                await send_frame(self.writer, FRAME_PCM_DOWN_END)
                return

            reply = await self._run_llm(transcript)
            t_llm = time.perf_counter()
            log.info("[%s] LLM done in %.2fs: %r",
                     self.session_id, t_llm - t_stt, reply)

            await self._run_tts_and_stream(reply)
            t_tts = time.perf_counter()
            log.info("[%s] TTS+stream done in %.2fs, total=%.2fs",
                     self.session_id, t_tts - t_llm, t_tts - t_start)

            self.state = SessionState.DONE

        except asyncio.IncompleteReadError:
            log.warning("[%s] Client disconnected mid-session", self.session_id)
            self.state = SessionState.ERROR
        except Exception as e:
            log.exception("[%s] Session error: %s", self.session_id, e)
            self.state = SessionState.ERROR
            try:
                await send_error(self.writer, str(e))
            except Exception:
                pass

    async def _handle_session_start(self) -> None:
        self.state = SessionState.INIT
        frame_type, payload = await recv_frame(self.reader)
        if frame_type != FRAME_SESSION_START:
            raise ValueError(f"Expected Session Start (0x00), got 0x{frame_type:02x}")
        info = parse_session_start(payload)
        self.device_id   = info["device_id"]
        self.sample_rate = info["sample_rate"] or config.DEFAULT_SAMPLE_RATE
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
                log.warning("[%s] Unexpected frame 0x%02x during audio receive", self.session_id, frame_type)

    async def _run_stt(self) -> str:
        self.state = SessionState.STT
        return await self.stt.transcribe(bytes(self.audio_buf), self.sample_rate)

    async def _run_llm(self, transcript: str) -> str:
        self.state = SessionState.LLM
        self.history.append({"role": "user", "content": transcript})
        reply = await self.llm.complete(self.history)
        self.history.append({"role": "assistant", "content": reply})
        # Keep history bounded (last 10 turns)
        if len(self.history) > 20:
            self.history = self.history[-20:]
        return reply

    async def _run_tts_and_stream(self, text: str) -> None:
        self.state = SessionState.TTS
        CHUNK_BYTES = 1024  # ~32ms @ 16kHz 16-bit mono

        async for chunk in self.tts.synthesize(text, self.sample_rate):
            await send_frame(self.writer, FRAME_PCM_DOWN, chunk)

        await send_frame(self.writer, FRAME_PCM_DOWN_END)
