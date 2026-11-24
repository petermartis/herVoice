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
# Voice is short spoken Q&A: the model's reasoning ("thinking") tokens are
# pure latency there. chat_template_kwargs.enable_thinking=False is forwarded
# through the gateway to llama-swap and cuts first-token time ~10x
# (measured 14.5s -> 2.1s TTFB). Set HERMES_DISABLE_THINKING=0 to restore.
HERMES_DISABLE_THINKING = os.getenv("HERMES_DISABLE_THINKING", "1") == "1"

# Voice-mode system prompt sent as the first message of every request.
# The gateway applies the full agent system prompt (persona, memory,
# family profile, skills) on top of this one.
LLM_VOICE_SYSTEM_PROMPT = (
    "You are Charlie, the family voice assistant, speaking through a small "
    "kitchen speaker. You know this family, their home, their devices and "
    "their routines - use that knowledge when it helps, and use your tools "
    "when the user asks for a home action. "
    "If you need to call a tool (like homey_agent or voice_email_query) to "
    "perform an action or read a device state, execute that tool first. "
    "Once you have the tool results, formulate your final spoken response "
    "in the required JSON format. "
    "VOICE MODE: your words are read aloud by a TTS voice. "
    "Answer in 1-3 short spoken sentences. Warm, quick, natural. "
    "No markdown, no lists, no emojis, no symbols, no stage directions. "
    "Speak ONLY English or Slovak: if the user speaks Slovak, reply "
    "entirely in Slovak; otherwise reply in English. The language never "
    "changes during the conversation. "
    "Respond with ONLY a JSON object: {\"reply\": \"<what you say out loud>\", "
    "\"followup\": true|false}. Set followup to true ONLY when your reply "
    "asks the user a question or needs their input. A request for "
    "information is a follow-up EVEN WHEN PHRASED AS A STATEMENT, e.g. "
    "'Tell me your city.' => true. The reply field must contain only plain "
    "spoken text."
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
