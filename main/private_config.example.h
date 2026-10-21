#pragma once

/* Copiar como private_config.h y completar con los datos de tu red y servidores. */
#define WIFI_SSID "tu-red-wifi"
#define WIFI_PASSWORD "tu-contrasena"

#define TRANSCRIPTION_URL "http://192.168.1.50:8001/v1/audio/transcriptions"
#define TRANSCRIPTION_MODEL "Systran/faster-whisper-small"

#define OLLAMA_CHAT_URL "http://192.168.1.50:11434/api/chat"
#define OLLAMA_MODEL "qwen3.8:27b"

#define TTS_URL "http://192.168.1.50:8001/v1/audio/speech"
#define TTS_MODEL "speaches-ai/Kokoro-82M-v1.0-ONNX"
#define TTS_VOICE "ef_dora"
