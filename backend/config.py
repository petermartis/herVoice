import os

# ── TCP server (device connections) ────────────────────────────────────────
HOST = os.getenv("HERVOICE_HOST", "0.0.0.0")
PORT = int(os.getenv("HERVOICE_PORT", "8765"))

# ── STT — whisper-server (faster-whisper medium, CUDA float16) ─────────────
# POST /transcribe  multipart: file=audio.wav, language=auto|sk|en
# Returns: {"text":"...","language":"sk","duration":2.3}
WHISPER_SERVER_URL = os.getenv("WHISPER_SERVER_URL", "http://127.0.0.1:15556")

# ── LLM — llama-server (Qwen3.5-27B, OpenAI-compat) ───────────────────────
LLM_SERVER_URL    = os.getenv("LLM_SERVER_URL", "http://127.0.0.1:8081")
LLM_MODEL         = os.getenv("LLM_MODEL", "qwen3.8-flash-next-unsloth-ud-iq4_xs-v31")
LLM_API_KEY       = os.getenv("LLM_API_KEY", "")
LLM_NO_THINK      = os.getenv("LLM_NO_THINK", "1") == "1"  # Qwen3 /no_think tag for low voice latency
LLM_MAX_TOKENS    = int(os.getenv("LLM_MAX_TOKENS", "256"))
LLM_TEMPERATURE   = float(os.getenv("LLM_TEMPERATURE", "0.7"))
LLM_SYSTEM_PROMPT = os.getenv(
    "LLM_SYSTEM_PROMPT",
    "You are a helpful voice assistant. Your reply text is spoken aloud, "
    "so keep it short, plain and conversational. "
    "Speak ONLY English or Slovak: if the user speaks Slovak or any other "
    "Slavic language, reply in Slovak; otherwise reply in English. "
    "NEVER use markdown: no asterisks, no underscores, no bold, no hashtags. "
    "Respond with ONLY a JSON object: {\"reply\": \"<what to say>\", \"followup\": true|false}. "
    "Set followup to true ONLY when your reply asks the user a question or "
    "needs their input. A request for information is a follow-up EVEN WHEN "
    "PHRASED AS A STATEMENT, e.g. 'Tell me your city or ZIP code.' => true. "
    "Set followup to false for complete answers, confirmations and greetings. "
    "The reply field must contain only plain spoken text - never any asterisks or other symbols."
)

# ── Hermes agent (voice persona: memory, family, skills, tools) ────────────
# The voice path routes through the Hermes gateway API: the same persona,
# persistent memory and skills as the desktop/WhatsApp agent. Falls back
# to the local llama-server if the gateway is unreachable.
HERMES_API_ENABLED  = os.getenv("HERMES_API_ENABLED", "1") == "1"
HERMES_API_URL      = os.getenv("HERMES_API_URL", "http://192.168.1.100:8642/v1/chat/completions")
HERMES_API_KEY      = os.getenv("HERMES_API_KEY", "")   # systemd EnvironmentFile, not the repo
HERMES_TIMEOUT_S    = int(os.getenv("HERMES_TIMEOUT_S", "45"))
HERMES_LLM_PROVIDER = os.getenv("HERMES_LLM_PROVIDER", "custom")
HERMES_LLM_MODEL    = os.getenv("HERMES_LLM_MODEL", "qwen3.8-flash-next-unsloth-ud-iq4_xs-v31")
# Voice is short spoken Q&A: the model's reasoning tokens are pure latency
# there. Through the gateway the switch is model_options.reasoning.enabled
# (chat_template_kwargs sent to the gateway left Qwen reasoning on); the
# direct Strata fallback takes chat_template_kwargs.enable_thinking=false.
# Set HERMES_DISABLE_THINKING=0 to restore.
HERMES_DISABLE_THINKING = os.getenv("HERMES_DISABLE_THINKING", "1") == "1"

# Voice-mode system prompt sent as the first message of every request.
# The gateway applies the full agent system prompt (persona, memory, tools)
# on top of this one. Behaviour rules live here; everything specific to the
# household (family facts, room and device IDs) is read from a private file
# outside the repository, so it never reaches a public commit.
VOICE_CONTEXT_FILE = os.getenv("HERVOICE_CONTEXT_FILE",
                               "/home/foscam/herVoice-private/voice_context.md")


def _load_voice_context() -> str:
    try:
        with open(VOICE_CONTEXT_FILE, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


VOICE_CONTEXT = _load_voice_context()

_VOICE_RULES = (
    "You are Charlie, the family's voice assistant, speaking through a small "
    "speaker at home. "
    "VOICE MODE: your words are read aloud by a TTS voice. Answer in one or "
    "two short spoken sentences, warm and natural. No markdown, lists, emojis "
    "or symbols. Never read out phone numbers, device IDs, addresses or "
    "account numbers. "
    "LANGUAGE: English or Slovak only. Reply in the language of the user's "
    "latest message and never mix the two. Write Slovak with full "
    "diacritics (for example: žena, spálňa, obývačka, brána). "
    "You cannot tell which family member is speaking: refer to people by "
    "name, never as 'your wife' or 'your son'. "
    "SCOPE: here you do two things - answer questions about the family and "
    "the home, and control the home. Answer other questions briefly from what "
    "you know. In voice mode never use file, terminal, web, code, skill, "
    "memory or scheduling tools, and never call tool_search. "
    "HOME CONTROL: call the Homey tools through tool_call, in exactly this "
    "shape: tool_call(calls=[{\"name\": \"mcp__homey__<tool>\", "
    "\"arguments\": {...}}]). Use the room and device IDs listed below; do "
    "not look devices up or list them. One call per request when the list "
    "covers it, never more than three. Any question about the current state "
    "of the home (gate, doors, lock, lights, temperature, weather, who is "
    "home) needs a tool call in this same turn: call the tool, then answer "
    "from its result. Earlier answers in this conversation are stale: "
    "call the tool again every time. Never reply that you will check or "
    "look - nobody hears a second message. If a tool fails or a device is unavailable, "
    "say so plainly - never guess a state. After an action, confirm in one "
    "short sentence. "
    "OUTPUT: respond with ONLY a JSON object: {\"reply\": \"<what you say out "
    "loud>\", \"followup\": true|false}. Set followup to true ONLY when your "
    "reply asks the user a question or needs their input, EVEN WHEN PHRASED "
    "AS A STATEMENT, e.g. 'Tell me which room.' => true. The reply field must "
    "contain only plain spoken text."
)

LLM_VOICE_SYSTEM_PROMPT = (
    _VOICE_RULES + "\n\n" + VOICE_CONTEXT if VOICE_CONTEXT else _VOICE_RULES
)

# ── TTS — piper-server (models in RAM) ─────────────────────────────────────
# POST /synthesize  JSON: {"text":"...","voice":"lili|libritts","speaker_id":886}
# Returns: WAV bytes
PIPER_SERVER_URL    = os.getenv("PIPER_SERVER_URL", "http://127.0.0.1:15555")
PIPER_VOICE_SK      = os.getenv("PIPER_VOICE_SK", "lili")
PIPER_VOICE_EN      = os.getenv("PIPER_VOICE_EN", "libritts")
PIPER_SPEAKER_ID_EN = int(os.getenv("PIPER_SPEAKER_ID_EN", "886"))

# ── Audio ───────────────────────────────────────────────────────────────────
DEVICE_SAMPLE_RATE  = 16000   # what the ESP32 expects
DEFAULT_BIT_DEPTH   = 16
DEFAULT_CHANNELS    = 1

# ── Session ─────────────────────────────────────────────────────────────────
MAX_SESSION_AUDIO_BYTES = 10 * DEVICE_SAMPLE_RATE * 2  # 10 seconds
# A device's conversation history is dropped after this much silence, so a new
# wake does not inherit answers (and stale home states) from an earlier one.
HISTORY_IDLE_RESET_S = int(os.getenv("HISTORY_IDLE_RESET_S", "180"))
