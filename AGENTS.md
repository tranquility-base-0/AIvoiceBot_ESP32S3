## 1. Repository Boundaries & Non-Destructive Rules

1. **Read-Only Historical Targets:**
   - `previousDraft/` — **DO NOT** modify, move, or delete. Use strictly as a reference for pinouts, persona prompts (`system_prompt.txt` / `.json`), and earlier WebSocket/API experiments.
   - `xiaoEsp32s3vBot0/` — **DO NOT** modify unless explicitly asked. Treat as the frozen baseline snapshot.
2. **Active Development Targets:**
   - `xiaoEsp32s3vBot1/` — **Track A (Standalone Cloud Bot):** Direct ESP32-S3 pipeline (`INMP441` -> Deepgram WSS -> Groq LLM HTTPS -> Groq/Cloud TTS HTTPS -> `MAX98357A`). Focus here is fixing WAV header parsing, HTTP short-read alignment, I2S DMA lifecycle, and TLS heap fragmentation.
   - `xiaoEsp32s3vBot2/` — **Track B Firmware (Local LAN Audio Terminal):** Zero-TLS, thin WebSocket PCM transceiver (`ws://<HOST_IP>:8765/ws/audio`). No on-chip HTTPS, JSON, or WAV parsing.
   - `host_bridge/` — **Track B Host Server (Python):** Local FastAPI/WebSocket daemon running STT (`faster-whisper` or Deepgram proxy), **Groq LLM**, and **Kokoro-ONNX** (`24000 Hz` `s16le` PCM streaming) + optional DSP/glitch hooks.
3. **Secrets Hygiene:**
   - **NEVER** commit real Wi-Fi SSIDs/passwords, Deepgram keys, or Groq API keys.
   - Firmware projects must read credentials from `include/secrets.h` (git-ignored) and provide a committed `include/secrets.example.h`.
   - Python host code must read credentials from `host_bridge/.env` (git-ignored) and provide `host_bridge/.env.example`.

---

## 2. Target Hardware & Pinout Specification

* **Board:** Seeed Studio XIAO ESP32-S3 (`seeed_xiao_esp32s3`)
* **Memory:** 8 MB Quad SPI Flash, 8 MB Octal PSRAM (`-DBOARD_HAS_PSRAM`, `board_build.arduino.memory_type = qio_opi`)
* **Audio Input (`I2S_NUM_0`):** `INMP441` MEMS Microphone (`16000 Hz`, 16-bit mono PCM derived from 32-bit I2S frame shifted `>> 11`)
* **Audio Output (`I2S_NUM_1`):** `MAX98357A` I2S Class-D Amplifier (`24000 Hz` default for Kokoro-ONNX / cloud TTS, 16-bit mono PCM)

### Canonical Pin Mapping (Verified against `xiaoEsp32s3vBot0` and `xiaoEsp32s3vBot1`)
| Bus | Peripheral | Signal | GPIO Pin | Direction |
| :--- | :--- | :--- | :--- | :--- |
| `I2S_NUM_0` | `INMP441` Mic | `SCK` (BCLK) | `GPIO 6` | Output from ESP32 |
| `I2S_NUM_0` | `INMP441` Mic | `WS` (LRC) | `GPIO 5` | Output from ESP32 |
| `I2S_NUM_0` | `INMP441` Mic | `SD` (DOUT) | `GPIO 4` | Input to ESP32 (`L/R` -> GND) |
| `I2S_NUM_1` | `MAX98357A` Amp | `BCLK` | `GPIO 8` | Output from ESP32 |
| `I2S_NUM_1` | `MAX98357A` Amp | `LRC` (WS) | `GPIO 7` | Output from ESP32 |
| `I2S_NUM_1` | `MAX98357A` Amp | `DIN` | `GPIO 9` | Output from ESP32 |

### Persona & Prompt Notes (Recovered from `previousDraft/` and `xiaoEsp32s3vBot1/`)
* Earlier prototypes in `previousDraft/voicebot_xiaoesp32s3-main/` included `<SPIFFS.h>` for local asset experiments, while `previousDraft/websocketTest_xiaoesp32s3-main/` and `xiaoEsp32s3vBot1/` embedded two character personas directly in `SYSTEM_PROMPT`:
  1. **TARS (Default):** Dry, witty, pragmatic exploration mission AI speaking in a concise 1–3 sentence monotone style optimized for audio links.
  2. **Max Headroom (Alternate / Glitch Persona):** Sarcastic, witty 1980s computer-generated TV host broadcasting live from Network 23 with voice buffer skips, phrase repeats, and digitized interjections (`*bzzzt*`, `*th-thump*`). Preserved in `host_bridge/personas/system_prompt.txt` alongside the optional `ENABLE_GLITCH_DSP` audio post-processor.

---

## 3. Five Mandatory ESP32-S3 Audio & Memory Invariants

Every firmware modification in `xiaoEsp32s3vBot1/` and `xiaoEsp32s3vBot2/` **MUST** enforce these five rules:

### Invariant 1: 2-Byte PCM Sample Alignment (Carry-Byte Rule)
TCP streams, chunked HTTP responses (`WiFiClientSecure::read`), and WebSocket frames can return an **odd number of bytes** when packets split mid-sample. Passing an odd byte count to `i2s_write()` on a 16-bit (`int16_t`) I2S bus shifts every subsequent sample by 1 byte, turning audio into full-scale static.
* **Rule:** Always maintain a `uint8_t carryByte` and `bool hasCarry` across network reads. Prepend any carried byte to the next read buffer, and if `totalBytes` is odd (`totalBytes & 1`), hold back the final byte for the next iteration so `i2s_write()` only ever receives `totalBytes & ~1` (strictly even byte counts).

### Invariant 2: Stateful WAV `"data"` Subchunk Scanner (No 44-Byte Assumption)
Cloud TTS WAV responses frequently include `LIST`, `INFO`, or `fact` chunks, making headers 56–80+ bytes instead of 44 bytes, or set the `data` chunk length to `0xFFFFFFFF` for streaming WAVs.
* **Rule:** Never hardcode `stream->readBytes(header, 44)`. Use a state machine that scans incoming bytes with a 4-byte sliding window until it matches `0x64, 0x61, 0x74, 0x61` (`"data"`), reads/parses `fmt ` (`sampleRate`, `numChannels`, `bitsPerSample`) along the way, discards the 4 bytes immediately following `"data"` (chunk size), and only then transitions to `STREAMING_PCM`.

### Invariant 3: Complete HTTP Stream Consumption
During chunked HTTPS transfers, `stream->available()` often returns `0` temporarily while TCP packets are in flight even though the response is not finished.
* **Rule:** Loop while `http.connected() || stream->available() > 0`. Track an idle timeout (e.g., `2500 ms` with zero bytes received) rather than exiting the loop the first time `stream->available() == 0`.

### Invariant 4: Internal SRAM vs. Octal PSRAM Separation (`mbedTLS` Survival)
`mbedTLS` (used by `WiFiClientSecure` and `WebSocketsClient`) requires ~40 KB of **contiguous internal SRAM** per TLS handshake.
* **Rule 1:** Allocate all audio ring buffers, HTTP stream read buffers (`>= 2048` bytes), and large JSON buffers in **Octal PSRAM** via `ps_malloc()` or `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.
* **Rule 2 (Track A):** Never run two active TLS sessions concurrently if internal heap drops below `80 KB`. Pause/disconnect the Deepgram WSS client before opening the Groq HTTPS connection, and reuse a single global `WiFiClientSecure` instance sequentially for Groq LLM and Groq TTS.

### Invariant 5: Half-Duplex Echo & Pop Prevention
* **Rule:** Stop/gate `I2S_NUM_0` mic capture before starting TTS playback on `I2S_NUM_1` so the bot never transcribes its own voice. When TTS playback ends, call `i2s_zero_dma_buffer(I2S_SPEAKER_PORT)` to prevent repeating DMA tail clicks, flush stale mic DMA frames on `I2S_NUM_0`, and then return to the `LISTENING` state.

---

## 4. Build, Verification & Tooling Commands

* **Compile Track A (Standalone Cloud):**
  ```bash
  pio run -d xiaoEsp32s3vBot1
  ```
* **Compile Track B (Local LAN Terminal):**
  ```bash
  pio run -d xiaoEsp32s3vBot2
  ```
* **Check Memory / Flash Usage:**
  Inspect the `RAM:` and `Flash:` summary emitted by `pio run` and ensure internal static RAM remains well below 50%.
* **Validate Python Host Bridge Syntax & Imports:**
  ```bash
  python3 -m py_compile host_bridge/bridge_server.py
  ```
