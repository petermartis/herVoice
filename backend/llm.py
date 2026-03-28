"""
LLM module — calls llama-server (Qwen3.5-27B-Q4_K_M.gguf, OpenAI-compat).
POST /v1/chat/completions  JSON: OpenAI messages format
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

# Prepended when detected language is Slovak to nudge the model to reply in Slovak
_SK_HINT = "Odpoveď prosím po slovensky. "


class LlamaLLM:
    def __init__(self):
        self._session: Optional["aiohttp.ClientSession"] = None

    def _get_session(self) -> "aiohttp.ClientSession":
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(
                headers={"Content-Type": "application/json"}
            )
        return self._session

    async def complete(self, history: list[dict], lang: str = "sk") -> str:
        """
        Send conversation history to llama-server and return the assistant reply.
        history: list of {"role": "system"|"user"|"assistant", "content": str}
        lang: detected language code ('sk', 'en', ...) — used to hint the model
        """
        if not _HAS_AIOHTTP:
            log.warning("aiohttp unavailable, returning stub LLM response")
            return "I heard you, but I'm currently in stub mode."

        # Build messages: system prompt + history
        # For Slovak, prepend a language hint to the last user message
        messages = list(history)
        if lang == "sk" and messages and messages[-1]["role"] == "user":
            messages = messages[:-1] + [
                {"role": "user",
                 "content": _SK_HINT + messages[-1]["content"]}
            ]

        payload = {
            "model":       config.LLM_MODEL,
            "messages":    messages,
            "max_tokens":  config.LLM_MAX_TOKENS,
            "temperature": config.LLM_TEMPERATURE,
            "stream":      False,
        }

        url = f"{config.LLM_SERVER_URL.rstrip('/')}/v1/chat/completions"
        session = self._get_session()
        try:
            async with session.post(url, json=payload,
                                    timeout=aiohttp.ClientTimeout(total=60)) as resp:
                if resp.status != 200:
                    body = await resp.text()
                    log.error("LLM request failed: HTTP %d — %s", resp.status, body[:200])
                    return "Sorry, I couldn't process that request."
                data = await resp.json()
                reply = data["choices"][0]["message"]["content"].strip()
                usage = data.get("usage", {})
                log.debug("LLM usage: %s", usage)
                log.info("LLM reply: %r", reply[:120])
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
