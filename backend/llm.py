"""
LLM module — calls OpenClaw (OpenAI-compatible) with Qwen 3.5.
"""
import logging
from typing import Optional

log = logging.getLogger("LLM")

try:
    import aiohttp
    _HAS_AIOHTTP = True
except ImportError:
    _HAS_AIOHTTP = False
    log.warning("aiohttp not installed, LLM will be a stub")

import config


class OpenClawLLM:
    def __init__(self):
        self._session: Optional["aiohttp.ClientSession"] = None

    def _get_session(self) -> "aiohttp.ClientSession":
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(
                headers={
                    "Authorization": f"Bearer {config.OPENCLAW_API_KEY}",
                    "Content-Type": "application/json",
                }
            )
        return self._session

    async def complete(self, history: list[dict]) -> str:
        """
        Send conversation history to OpenClaw/Qwen and return the assistant reply text.
        history: list of {"role": "system"|"user"|"assistant", "content": str}
        """
        if not _HAS_AIOHTTP:
            log.warning("aiohttp unavailable, returning stub LLM response")
            return "I heard you, but I'm currently in stub mode."

        url = f"{config.OPENCLAW_BASE_URL.rstrip('/')}/chat/completions"
        payload = {
            "model":       config.OPENCLAW_MODEL,
            "messages":    history,
            "max_tokens":  config.LLM_MAX_TOKENS,
            "temperature": config.LLM_TEMPERATURE,
            "stream":      False,
        }

        session = self._get_session()
        try:
            async with session.post(url, json=payload, timeout=aiohttp.ClientTimeout(total=30)) as resp:
                if resp.status != 200:
                    body = await resp.text()
                    log.error("LLM request failed: HTTP %d — %s", resp.status, body[:200])
                    return "Sorry, I couldn't process that request."
                data = await resp.json()
                reply = data["choices"][0]["message"]["content"].strip()
                usage = data.get("usage", {})
                log.debug("LLM usage: %s", usage)
                return reply
        except aiohttp.ClientError as e:
            log.exception("LLM HTTP error: %s", e)
            return "Sorry, I couldn't reach the language model."
        except (KeyError, IndexError) as e:
            log.exception("LLM response parse error: %s", e)
            return "Sorry, I received an unexpected response."

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()
