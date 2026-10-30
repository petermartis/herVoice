"""
herVoice backend server — asyncio TCP server.
Accepts device connections and runs voice pipeline per session.
"""

import asyncio
import logging
import signal
import sys
from typing import Optional

import config
from protocol import (
    recv_frame, parse_session_start,
    FRAME_SESSION_START, FrameError,
)
from session import Session
from stt import WhisperSTT
from llm import make_llm
from tts import PiperTTS

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
    stream=sys.stdout,
)
log = logging.getLogger("SERVER")


class HerVoiceServer:
    def __init__(self):
        self.stt = WhisperSTT()
        self.llm = make_llm()
        self.tts = PiperTTS()
        # Per-device conversation history keyed by device_id
        self._histories: dict[int, list] = {}

    async def startup(self) -> None:
        log.info("Loading STT model...")
        await self.stt.load()
        log.info("STT model loaded")

    def _get_history(self, device_id: int) -> list:
        if device_id not in self._histories:
            self._histories[device_id] = [
                {"role": "system", "content": config.LLM_SYSTEM_PROMPT}
            ]
        return self._histories[device_id]

    async def handle_client(self, reader: asyncio.StreamReader,
                             writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        log.info("New connection from %s:%s", *peer)

        # Each device connection keeps its conversation context alive
        # across sessions. We peek at the first session start to learn
        # device_id BEFORE creating the Session, so EVERY session
        # (including the first) runs on the per-device history - system
        # prompt plus all prior turns.
        history: Optional[list] = None
        # Normalized language of the last real transcript on this
        # connection - inherited by follow-up turns.
        convo_lang: Optional[str] = None

        while True:
            try:
                if history is None:
                    ftype, payload = await recv_frame(reader)
                    if ftype != FRAME_SESSION_START:
                        raise FrameError(
                            f"Expected session start (0x00), got 0x{ftype:02x}")
                    device_id = parse_session_start(payload)["device_id"]
                    history = self._get_history(device_id)
                    session = Session(reader, writer, self.stt, self.llm,
                                      self.tts, history,
                                      first_frame=(ftype, payload),
                                      sticky_lang=convo_lang)
                else:
                    session = Session(reader, writer, self.stt, self.llm,
                                      self.tts, history,
                                      sticky_lang=convo_lang)
                await session.run()
                if session.lang_confirmed:
                    convo_lang = session.lang
            except (ConnectionResetError, asyncio.IncompleteReadError,
                    BrokenPipeError, FrameError):
                log.info("Client %s:%s disconnected", *peer)
                break
            except Exception as e:
                log.exception("Unexpected error for %s:%s: %s", *peer, e)
                break

        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass

    async def serve(self) -> None:
        await self.startup()
        server = await asyncio.start_server(
            self.handle_client,
            config.HOST,
            config.PORT,
        )
        addrs = ", ".join(str(s.getsockname()) for s in server.sockets)
        log.info("herVoice backend listening on %s", addrs)

        async with server:
            await server.serve_forever()


async def main() -> None:
    srv = HerVoiceServer()
    loop = asyncio.get_running_loop()

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, lambda: asyncio.ensure_future(shutdown(srv)))

    await srv.serve()


async def shutdown(srv: HerVoiceServer) -> None:
    log.info("Shutting down...")
    tasks = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    asyncio.get_event_loop().stop()


if __name__ == "__main__":
    asyncio.run(main())
