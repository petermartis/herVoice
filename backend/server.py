"""
herVoice backend server — asyncio TCP server.
Accepts device connections and runs voice pipeline per session.
"""

import asyncio
import logging
import signal
import sys

import config
from session import Session
from stt import WhisperSTT
from llm import OpenClawLLM
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
        self.llm = OpenClawLLM()
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
                {"role": "system", "content": config.OPENCLAW_SYSTEM_PROMPT}
            ]
        return self._histories[device_id]

    async def handle_client(self, reader: asyncio.StreamReader,
                             writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        log.info("New connection from %s:%s", *peer)

        # Each device connection keeps its conversation context alive across sessions
        # We peek at the session start to get device_id before creating Session
        history: list = []  # will be updated after first session_start

        while True:
            try:
                session = Session(reader, writer, self.stt, self.llm, self.tts, history)
                await session.run()
                # Update history reference after session populates device_id
                history = self._get_history(session.device_id)
                session.history = history
            except (ConnectionResetError, asyncio.IncompleteReadError, BrokenPipeError):
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
