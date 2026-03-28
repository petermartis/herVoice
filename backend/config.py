import os

# Server
HOST = os.getenv("HERVOICE_HOST", "0.0.0.0")
PORT = int(os.getenv("HERVOICE_PORT", "8765"))

# STT (faster-whisper)
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "medium")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")    # or "cuda"
WHISPER_COMPUTE = os.getenv("WHISPER_COMPUTE", "int8") # or "float16"
WHISPER_LANGUAGE = os.getenv("WHISPER_LANGUAGE", None) # None = auto-detect
WHISPER_INITIAL_PROMPT = os.getenv("WHISPER_INITIAL_PROMPT", "")

# LLM (OpenClaw / OpenAI-compatible)
OPENCLAW_BASE_URL = os.getenv("OPENCLAW_BASE_URL", "http://localhost:11434/v1")
OPENCLAW_API_KEY  = os.getenv("OPENCLAW_API_KEY", "nocreds")
OPENCLAW_MODEL    = os.getenv("OPENCLAW_MODEL", "qwen2.5:3b")
OPENCLAW_SYSTEM_PROMPT = os.getenv(
    "OPENCLAW_SYSTEM_PROMPT",
    "You are a helpful voice assistant. Keep your responses short and natural for speech."
)
LLM_MAX_TOKENS = int(os.getenv("LLM_MAX_TOKENS", "256"))
LLM_TEMPERATURE = float(os.getenv("LLM_TEMPERATURE", "0.7"))

# TTS (Piper)
PIPER_BINARY = os.getenv("PIPER_BINARY", "piper")
PIPER_MODEL  = os.getenv("PIPER_MODEL", "/usr/share/piper/voices/en_US-lessac-medium.onnx")
PIPER_SAMPLE_RATE = int(os.getenv("PIPER_SAMPLE_RATE", "16000"))

# Audio
DEFAULT_SAMPLE_RATE = 16000
DEFAULT_BIT_DEPTH   = 16
DEFAULT_CHANNELS    = 1

# Session
MAX_SESSION_AUDIO_BYTES = 10 * DEFAULT_SAMPLE_RATE * 2  # 10 seconds
