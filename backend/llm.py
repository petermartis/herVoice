"""
LLM module — calls llama-server (Qwen3.5-27B-Q4_K_M.gguf, OpenAI-compat).
POST /v1/chat/completions  JSON: OpenAI messages format
"""
import json
import logging
import time
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
_EN_HINT = "Please answer in English. "

# Injected into the user message on the gateway path: the backend's voice
# rules ride as a system message under the FULL agent persona (17k tokens),
# and the small model sometimes drowns them and replies with a bare plan
# ("# Checking gate sensor state") without ever calling a tool. Instructions
# riding on the user turn carry far more weight than the system tail.
_VOICE_DIRECTIVE = (
    "\n\n[VOICE: a question about a current state (gate, doors, lock, lights, "
    "temperature, weather, who is home) REQUIRES calling the matching "
    "mcp__homey__* tool in this turn FIRST, then answering with ONLY the JSON "
    "object. A reply that only announces a plan or status without a tool "
    "result is wrong. Reply in the language the user just spoke.]"
)


def _voice_payload(messages: list[dict]) -> dict:
    """Streaming payload for the explicit local Hermes voice path."""
    p = {
        "model": config.HERMES_LLM_MODEL,
        "provider": config.HERMES_LLM_PROVIDER,
        "messages": messages,
        "stream": True,
    }
    if config.HERMES_DISABLE_THINKING:
        p["model_options"] = {"reasoning": {"enabled": False}}
    return p


class LlamaLLM:
    def __init__(self):
        self._session: Optional["aiohttp.ClientSession"] = None

    def _get_session(self) -> "aiohttp.ClientSession":
        if self._session is None or self._session.closed:
            headers = {"Content-Type": "application/json"}
            if config.LLM_API_KEY:
                headers["Authorization"] = f"Bearer {config.LLM_API_KEY}"
            self._session = aiohttp.ClientSession(headers=headers)
        return self._session

    async def complete(self, history: list[dict], lang: str = "sk",
                       on_first_chunk=None) -> str:
        """
        Send conversation history to llama-server and return the assistant reply.
        history: list of {"role": "system"|"user"|"assistant", "content": str}
        lang: detected language code ('sk', 'en', ...) — used to hint the model
        on_first_chunk: optional callback fired when content is available
        (non-streaming backend: at reply completion)
        """
        if not _HAS_AIOHTTP:
            log.warning("aiohttp unavailable, returning stub LLM response")
            return "I heard you, but I'm currently in stub mode."

        # Prepend a language hint to the last user message (both languages:
        # an un-hinted garbled clip lets the surrounding Slovak context leak
        # into English answers)
        messages = list(history)
        if lang in ("sk", "en") and messages and messages[-1]["role"] == "user":
            hint = _SK_HINT if lang == "sk" else _EN_HINT
            messages = messages[:-1] + [
                {"role": "user",
                 "content": hint + messages[-1]["content"]}
            ]
        if config.LLM_NO_THINK and messages and messages[-1]["role"] == "user":
            # Qwen3 thinking models: /no_think skips reasoning tokens (voice latency)
            messages = messages[:-1] + [
                {"role": "user",
                 "content": messages[-1]["content"] + " /no_think"}
            ]

        payload = {
            "model":       config.LLM_MODEL,
            "messages":    messages,
            "max_tokens":  config.LLM_MAX_TOKENS,
            "temperature": config.LLM_TEMPERATURE,
            "stream":      False,
        }
        if config.LLM_NO_THINK:
            # Strata ignores the /no_think tag; this is the switch it honours.
            payload["chat_template_kwargs"] = {"enable_thinking": False}

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
                if on_first_chunk:
                    on_first_chunk()
                return reply
        except aiohttp.ClientError as e:
            log.exception("LLM HTTP error: %s", e)
            return "Sorry, I couldn't reach the language model."
        except (KeyError, IndexError) as e:
            log.exception("LLM response parse error: %s", e)
            return "Sorry, I received an unexpected response."

    async def stream_reply(self, history: list[dict], lang: str = "sk",
                           conversation_id: Optional[str] = None):
        """Non-streaming backend: yield the full reply in one delta."""
        reply = await self.complete(history, lang)
        if reply:
            yield reply

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()


class HermesLLM:
    """Routes completions through the Hermes agent API (:8642).

    The gateway runs the full agent loop - persona, persistent memory,
    user profile, skills and tools - on top of the per-request system
    prompt we send. Streaming is REQUIRED: non-streaming responses carry
    ~18 s of gateway post-processing, streaming measured 3.1 s to first
    content. On any failure it falls back to the local llama-server so
    the speaker never goes silent.
    """

    def __init__(self):
        self._session: Optional["aiohttp.ClientSession"] = None
        self._fallback = LlamaLLM()

    def _get_session(self) -> "aiohttp.ClientSession":
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(
                headers={
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {config.HERMES_API_KEY}",
                }
            )
        return self._session

    async def _stream_complete(self, messages: list[dict],
                               on_first_chunk=None) -> str:
        payload = _voice_payload(messages)
        session = self._get_session()
        async with session.post(
            config.HERMES_API_URL, json=payload,
            timeout=aiohttp.ClientTimeout(total=config.HERMES_TIMEOUT_S),
        ) as resp:
            if resp.status != 200:
                body = await resp.text()
                raise RuntimeError(f"HTTP {resp.status}: {body[:200]}")
            buf = ""
            fired = False
            async for raw in resp.content:
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    ev = json.loads(data)
                except ValueError:
                    continue
                choices = ev.get("choices") or []
                if choices:
                    delta = choices[0].get("delta") or {}
                    chunk = delta.get("content")
                    if chunk:
                        buf += chunk
                        if on_first_chunk and not fired:
                            on_first_chunk()
                            fired = True
            return buf.strip()

    async def complete(self, history: list[dict], lang: str = "sk",
                       on_first_chunk=None) -> str:
        if not _HAS_AIOHTTP:
            return await self._fallback.complete(history, lang, on_first_chunk)
        if not config.HERMES_API_KEY:
            log.warning("HERMES_API_KEY not set, using local LLM")
            return await self._fallback.complete(history, lang, on_first_chunk)

        # Swap the leading system message for the voice-mode prompt; the
        # gateway layers the agent persona/memory/skills on top.
        messages = list(history)
        if messages and messages[0].get("role") == "system":
            messages[0] = {
                "role": "system",
                "content": config.LLM_VOICE_SYSTEM_PROMPT,
            }
        if lang in ("sk", "en") and messages and messages[-1]["role"] == "user":
            hint = _SK_HINT if lang == "sk" else _EN_HINT
            messages = messages[:-1] + [
                {"role": "user", "content": hint + messages[-1]["content"]}
            ]
        try:
            t0 = time.time()
            reply = await self._stream_complete(messages, on_first_chunk)
            log.info("Hermes LLM reply in %.1fs: %r",
                     time.time() - t0, reply[:120])
            if not reply:
                raise RuntimeError("empty stream")
            return reply
        except Exception as e:
            log.warning("Hermes LLM failed (%s) - falling back to local LLM", e)
            return await self._fallback.complete(history, lang, on_first_chunk)

    async def stream(self, messages: list[dict], conversation_id: Optional[str] = None):
        """Yield raw content deltas from the Hermes SSE stream."""
        payload = _voice_payload(messages)
        session = self._get_session()
        # With a session id the gateway continues its own stored transcript,
        # which includes the tool calls and their results. A history resent
        # by us holds only the spoken replies, and the model then copies
        # that pattern: it answers home questions without calling a tool.
        headers = {"X-Hermes-Session-Id": conversation_id} if conversation_id else None
        async with session.post(
            config.HERMES_API_URL, json=payload, headers=headers,
            timeout=aiohttp.ClientTimeout(
                total=config.HERMES_TIMEOUT_S + 75, sock_read=30),
        ) as resp:
            if resp.status != 200:
                body = await resp.text()
                raise RuntimeError(f"HTTP {resp.status}: {body[:200]}")
            async for raw in resp.content:
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    ev = json.loads(data)
                except ValueError:
                    continue
                choices = ev.get("choices") or []
                if choices:
                    chunk = (choices[0].get("delta") or {}).get("content")
                    if chunk:
                        yield chunk

    async def stream_reply(self, history: list[dict], lang: str = "sk",
                           conversation_id: Optional[str] = None):
        """Stream the voice reply; fall back to local LLM on failure.
        Falls back only if nothing has been emitted yet (a mid-stream
        failure would otherwise speak the reply twice)."""
        if not _HAS_AIOHTTP or not config.HERMES_API_KEY:
            async for d in self._fallback.stream_reply(history, lang):
                yield d
            return
        messages = list(history)
        if messages and messages[0].get("role") == "system":
            messages[0] = {
                "role": "system",
                "content": config.LLM_VOICE_SYSTEM_PROMPT,
            }
        else:
            messages.insert(0, {
                "role": "system",
                "content": config.LLM_VOICE_SYSTEM_PROMPT,
            })
        if conversation_id:
            # The gateway already holds the earlier turns of this conversation.
            messages = [messages[0]] + [m for m in messages[-1:] if m.get("role") == "user"]
        if messages and messages[-1].get("role") == "user":
            messages[-1] = {**messages[-1],
                            "content": messages[-1]["content"] + _VOICE_DIRECTIVE}
        emitted = False
        try:
            async for d in self.stream(messages, conversation_id):
                emitted = True
                yield d
        except Exception as e:
            log.warning("Hermes stream failed (%s) - falling back to local LLM", e)
            if not emitted:
                async for d in self._fallback.stream_reply(history, lang):
                    yield d
            else:
                raise

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()
        await self._fallback.close()


def make_llm():
    """Pick the LLM backend: Hermes agent when configured, else local."""
    if config.HERMES_API_ENABLED and config.HERMES_API_KEY:
        log.info("LLM backend: Hermes agent API (%s) provider=%s model=%s household context=%d chars",
                 config.HERMES_API_URL, config.HERMES_LLM_PROVIDER,
                 config.HERMES_LLM_MODEL, len(config.VOICE_CONTEXT))
        return HermesLLM()
    log.info("LLM backend: local llama-server (%s)", config.LLM_SERVER_URL)
    return LlamaLLM()
