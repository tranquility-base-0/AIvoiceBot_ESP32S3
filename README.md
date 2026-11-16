# AIvoiceBot ESP32-S3

**Status: Active dual-track development (Track A Cloud Standalone & Track B Local LAN Bridge)**

AIvoiceBot is a voice-assistant platform built around the **Seeed Studio XIAO ESP32-S3** (8 MB Quad SPI Flash, 8 MB Octal PSRAM) using dual I2S buses for `INMP441` microphone capture and `MAX98357A` Class-D speaker playback.

---

## Repository Architecture

### Active Development Tracks

1. **[`xiaoEsp32s3vBot1/`](xiaoEsp32s3vBot1/) — Track A (Standalone Cloud Bot):**
   - Direct on-chip pipeline: `INMP441` (`16 kHz` PCM) $\rightarrow$ Deepgram Nova-3 WSS STT $\rightarrow$ Groq LLM (`llama-3.3-70b-versatile`) HTTPS $\rightarrow$ Groq PlayAI TTS (`wav`) HTTPS $\rightarrow$ `MAX98357A` (`24 kHz` PCM).
   - Hardened with all five ESP32-S3 audio and memory invariants: stateful WAV `"fmt "` + `"data"` subchunk scanner, 2-byte carry-buffer PCM sample alignment, complete chunked HTTP stream consumption with idle timeout, Octal PSRAM buffer allocation with single-TLS session sequencing, and half-duplex mic gating + DMA buffer zeroing.

2. **[`xiaoEsp32s3vBot2/`](xiaoEsp32s3vBot2/) + [`host_bridge/`](host_bridge/) — Track B (Local LAN Audio Terminal + Host Bridge):**
   - **`xiaoEsp32s3vBot2/`:** Thin, zero-TLS WebSocket PCM transceiver (`ws://<BRIDGE_HOST>:8765/ws/audio`). Streams `16 kHz` `s16le` mono mic frames in `LISTENING` mode and plays headerless `24 kHz` `s16le` mono TTS frames in `SPEAKING` mode using Octal PSRAM buffering and 2-byte carry alignment.
   - **`host_bridge/`:** Python FastAPI WebSocket server (`bridge_server.py`) providing pluggable STT (local `faster-whisper` with VAD endpointing or Deepgram proxy), **Groq LLM** (`llama-3.3-70b-versatile` with configurable `OPENAI_BASE_URL`), sentence-streamed **Kokoro-ONNX** TTS (`24000 Hz` `s16le` binary frames), customizable personas ([`host_bridge/personas/system_prompt.txt`](host_bridge/personas/system_prompt.txt)), and an optional Max Headroom glitch/espeak-ng DSP hook (`ENABLE_GLITCH_DSP`).

### Frozen & Historical Reference Targets

- **[`xiaoEsp32s3vBot0/`](xiaoEsp32s3vBot0/)** — Frozen earlier baseline snapshot retained for comparison.
- **[`previousDraft/`](previousDraft/)** — Read-only archive of earlier Arduino `.ino` sketches (`TTStest_xiaoesp32s3_Dgram`, `TTStest_xiaoesp32s3_Groq`, `voicebot_xiaoesp32s3` with initial `<SPIFFS.h>` experiment, and `websocketTest_xiaoesp32s3` where the TARS and Max Headroom persona prompts originated) plus [Troubleshooting_notes.md](Troubleshooting_notes.md).

---

## Hardware & Canonical Pinout

* **Board:** Seeed Studio XIAO ESP32-S3 (`seeed_xiao_esp32s3`, `board_build.arduino.memory_type = qio_opi`, `-DBOARD_HAS_PSRAM`)
* **Microphone (`I2S_NUM_0`):** `INMP441` MEMS Microphone (`16000 Hz`, 32-bit I2S frame shifted `>> 11` to 16-bit mono PCM)
* **Amplifier (`I2S_NUM_1`):** `MAX98357A` I2S Class-D Amplifier (`24000 Hz` default, 16-bit mono PCM)

| I2S Bus | Peripheral | Signal | XIAO ESP32-S3 GPIO | Direction |
| :--- | :--- | :--- | :--- | :--- |
| `I2S_NUM_0` | `INMP441` Mic | `SCK` (BCLK) | `GPIO 6` | Output from ESP32 |
| `I2S_NUM_0` | `INMP441` Mic | `WS` (LRC) | `GPIO 5` | Output from ESP32 |
| `I2S_NUM_0` | `INMP441` Mic | `SD` (DOUT) | `GPIO 4` | Input to ESP32 (`L/R` tied to `GND`) |
| `I2S_NUM_1` | `MAX98357A` Amp | `BCLK` | `GPIO 8` | Output from ESP32 |
| `I2S_NUM_1` | `MAX98357A` Amp | `LRC` (WS) | `GPIO 7` | Output from ESP32 |
| `I2S_NUM_1` | `MAX98357A` Amp | `DIN` | `GPIO 9` | Output from ESP32 |

---

## Setup & Build Instructions

### 1. Credentials Hygiene
Never commit real Wi-Fi credentials or API keys.
* **Track A (`xiaoEsp32s3vBot1`):**
  ```bash
  cp xiaoEsp32s3vBot1/include/secrets.example.h xiaoEsp32s3vBot1/include/secrets.h
  ```
  Edit `xiaoEsp32s3vBot1/include/secrets.h` with your `WIFI_SSID`, `WIFI_PASSWORD`, `DEEPGRAM_API_KEY`, and `GROQ_API_KEY`.
* **Track B (`xiaoEsp32s3vBot2` & `host_bridge`):**
  ```bash
  cp xiaoEsp32s3vBot2/include/secrets.example.h xiaoEsp32s3vBot2/include/secrets.h
  cp host_bridge/.env.example host_bridge/.env
  ```
  Set `BRIDGE_HOST` in `xiaoEsp32s3vBot2/include/secrets.h` to your host machine's LAN IP, and configure `GROQ_API_KEY` (and optional `DEEPGRAM_API_KEY` / Kokoro model paths) in `host_bridge/.env`.

### 2. Building Firmware (PlatformIO)
```bash
# Compile Track A (Standalone Cloud Bot)
pio run -d xiaoEsp32s3vBot1

# Compile Track B (Local LAN WebSocket Audio Terminal)
pio run -d xiaoEsp32s3vBot2
```

### 3. Running the Track B Python Host Bridge
```bash
cd host_bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn bridge_server:app --host 0.0.0.0 --port 8765
```
