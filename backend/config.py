import os

# ── TCP server (device connections) ────────────────────────────────────────
HOST = os.getenv("HERVOICE_HOST", "0.0.0.0")
PORT = int(os.getenv("HERVOICE_PORT", "8765"))

# ── STT — whisper-server (faster-whisper medium, CUDA float16) ─────────────
# POST /transcribe  multipart: file=audio.wav, language=auto|sk|en
# Returns: {"text":"...","language":"sk","duration":2.3}
WHISPER_SERVER_URL = os.getenv("WHISPER_SERVER_URL", "http://127.0.0.1:15556")

# ── LLM — llama-server (Qwen3.5-27B, OpenAI-compat) ───────────────────────
LLM_SERVER_URL    = os.getenv("LLM_SERVER_URL", "http://127.0.0.1:8080")
LLM_MODEL         = os.getenv("LLM_MODEL", "Qwen_Qwen3.5-27B-Q4_K_M.gguf")
LLM_MAX_TOKENS    = int(os.getenv("LLM_MAX_TOKENS", "256"))
LLM_TEMPERATURE   = float(os.getenv("LLM_TEMPERATURE", "0.7"))
LLM_SYSTEM_PROMPT = os.getenv(
    "LLM_SYSTEM_PROMPT",
    "You are a helpful voice assistant. Reply in plain text only, no markdown. "
    "Keep responses short and natural for speech."
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
