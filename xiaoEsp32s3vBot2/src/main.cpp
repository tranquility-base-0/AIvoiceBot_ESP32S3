#include <Arduino.h>
#include "secrets.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>

// =============================================================================
// PIN ASSIGNMENTS & HARDWARE CONFIG (Canonical XIAO ESP32-S3 Mapping)
// =============================================================================
// INMP441 Microphone (I2S_NUM_0 — 16000 Hz, 16-bit mono PCM output)
#define I2S_MIC_PORT     I2S_NUM_0
#define I2S_MIC_SCK      6
#define I2S_MIC_WS       5
#define I2S_MIC_SD       4
#define MIC_SAMPLE_RATE  16000
#define MIC_FRAME_SAMPLES 512

// MAX98357A Speaker Amplifier (I2S_NUM_1 — 24000 Hz, 16-bit mono s16le PCM)
#define I2S_SPEAKER_PORT I2S_NUM_1
#define I2S_SPEAKER_SCK  8
#define I2S_SPEAKER_WS   7
#define I2S_SPEAKER_SD   9
#define SPK_SAMPLE_RATE  24000

// PSRAM Buffer Sizes (Invariant 4)
#define PSRAM_PLAYBACK_BUF_SIZE 8192
#define PLAYBACK_IDLE_TIMEOUT_MS 600

// =============================================================================
// HALF-DUPLEX TERMINAL STATE & GLOBALS
// =============================================================================
enum TerminalState {
  STATE_LISTENING,
  STATE_SPEAKING
};

volatile TerminalState currentState = STATE_LISTENING;
unsigned long lastRxAudioMs = 0;

WebSocketsClient webSocket;

// Invariant 4: Buffers allocated in 8 MB Octal PSRAM
int32_t* psramMicRaw32    = nullptr;
int16_t* psramMicPcm16    = nullptr;
uint8_t* psramPlaybackBuf = nullptr;

// Invariant 1: 2-Byte Carry Alignment state across WebSocket binary frames
uint8_t carryByte = 0;
bool hasCarry = false;

// =============================================================================
// PSRAM ALLOCATION HELPERS (INVARIANT 4)
// =============================================================================
static void* allocPSRAMOrSRAM(size_t size) {
  void* ptr = nullptr;
  if (psramFound()) {
    ptr = ps_malloc(size);
    if (!ptr) {
      ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
  }
  if (!ptr) {
    ptr = malloc(size);
  }
  return ptr;
}

bool initPSRAMBuffers() {
  psramMicRaw32    = (int32_t*)allocPSRAMOrSRAM(MIC_FRAME_SAMPLES * sizeof(int32_t));
  psramMicPcm16    = (int16_t*)allocPSRAMOrSRAM(MIC_FRAME_SAMPLES * sizeof(int16_t));
  psramPlaybackBuf = (uint8_t*)allocPSRAMOrSRAM(PSRAM_PLAYBACK_BUF_SIZE + 2);

  Serial.printf("[MEM] PSRAM enabled: %s | Free PSRAM: %u bytes | Free Internal Heap: %u bytes\n",
                psramFound() ? "YES" : "NO",
                ESP.getFreePsram(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  return (psramMicRaw32 != nullptr && psramMicPcm16 != nullptr && psramPlaybackBuf != nullptr);
}

// =============================================================================
// HALF-DUPLEX ECHO & POP PREVENTION (INVARIANT 5)
// =============================================================================
void flushMicDMA() {
  if (!psramMicRaw32) return;
  size_t bytes_read = 0;
  for (int i = 0; i < 6; i++) {
    i2s_read(I2S_MIC_PORT, (void*)psramMicRaw32, MIC_FRAME_SAMPLES * sizeof(int32_t), &bytes_read, 0);
  }
}

void enterSpeakingState() {
  if (currentState != STATE_SPEAKING) {
    currentState = STATE_SPEAKING;
    carryByte = 0;
    hasCarry = false;
    // Gate I2S_NUM_0 mic capture so the bot never transcribes its own voice
    i2s_stop(I2S_MIC_PORT);
    Serial.println("[STATE] -> SPEAKING (Mic gated)");
  }
  lastRxAudioMs = millis();
}

void enterListeningState() {
  if (currentState != STATE_LISTENING) {
    // Zero speaker DMA buffer to prevent repeating tail clicks/pops
    i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
    carryByte = 0;
    hasCarry = false;
    // Restart mic and drain stale DMA frames before streaming to host
    i2s_start(I2S_MIC_PORT);
    flushMicDMA();
    currentState = STATE_LISTENING;
    Serial.println("[STATE] -> LISTENING (Speaker DMA zeroed, Mic DMA flushed)");
  }
}

// =============================================================================
// 2-BYTE ALIGNED PCM PLAYBACK (INVARIANT 1: CARRY-BYTE RULE)
// =============================================================================
void playAlignedPCMFrame(const uint8_t* payload, size_t length) {
  if (length == 0 || !psramPlaybackBuf) return;

  enterSpeakingState();

  size_t offset = 0;
  while (offset < length) {
    size_t writeIdx = 0;
    if (hasCarry) {
      psramPlaybackBuf[0] = carryByte;
      writeIdx = 1;
      hasCarry = false;
    }

    size_t maxCopy = PSRAM_PLAYBACK_BUF_SIZE - writeIdx;
    size_t chunkLen = length - offset;
    if (chunkLen > maxCopy) {
      chunkLen = maxCopy;
    }

    memcpy(psramPlaybackBuf + writeIdx, payload + offset, chunkLen);
    offset += chunkLen;

    size_t totalBytes = writeIdx + chunkLen;
    if (totalBytes & 1) {
      carryByte = psramPlaybackBuf[totalBytes - 1];
      hasCarry = true;
    }

    size_t evenBytes = totalBytes & ~((size_t)1);
    if (evenBytes > 0) {
      size_t bytes_written = 0;
      esp_err_t err = i2s_write(I2S_SPEAKER_PORT, psramPlaybackBuf, evenBytes, &bytes_written, portMAX_DELAY);
      if (err != ESP_OK) {
        Serial.printf("[I2S SPK] Write error: %d\n", err);
      }
    }
  }
}

// =============================================================================
// WEBSOCKET EVENT HANDLER (ZERO-TLS, ZERO-JSON, HEADERLESS 24kHz s16le PCM)
// =============================================================================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from Host Bridge.");
      if (currentState == STATE_SPEAKING) {
        enterListeningState();
      }
      break;

    case WStype_CONNECTED:
      Serial.printf("[WS] Connected to ws://%s:%d%s\n", BRIDGE_HOST, BRIDGE_PORT, BRIDGE_PATH);
      enterListeningState();
      break;

    case WStype_TEXT:
      // Lightweight text control markers from host_bridge ("TTS_START" / "TTS_END")
      if (length >= 9 && strncmp((const char*)payload, "TTS_START", 9) == 0) {
        enterSpeakingState();
      } else if (length >= 7 && strncmp((const char*)payload, "TTS_END", 7) == 0) {
        enterListeningState();
      }
      break;

    case WStype_BIN:
      // Raw 24000 Hz 16-bit mono s16le PCM frame from Kokoro-ONNX host bridge
      playAlignedPCMFrame(payload, length);
      break;

    case WStype_ERROR:
      Serial.println("[WS] Socket error.");
      break;

    default:
      break;
  }
}

// =============================================================================
// I2S INITIALIZATION (CANONICAL PINOUT)
// =============================================================================
void configureI2S_Mic() {
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = MIC_FRAME_SAMPLES,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };

  esp_err_t err = i2s_driver_install(I2S_MIC_PORT, &mic_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S MIC] Driver install failed: %d\n", err);
    return;
  }
  i2s_set_pin(I2S_MIC_PORT, &pin_config);
}

void configureI2S_Speaker() {
  i2s_config_t tx_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SPK_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_SPEAKER_PORT, &tx_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S SPK] Driver install failed: %d\n", err);
    return;
  }

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPEAKER_SCK,
    .ws_io_num = I2S_SPEAKER_WS,
    .data_out_num = I2S_SPEAKER_SD,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_set_pin(I2S_SPEAKER_PORT, &pin_config);
  i2s_set_clk(I2S_SPEAKER_PORT, SPK_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
}

// =============================================================================
// SETUP & LOOP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Track B: Local LAN WebSocket Audio Terminal (xiaoEsp32s3vBot2) ===");

  if (!initPSRAMBuffers()) {
    Serial.println("FATAL: PSRAM buffer allocation failed!");
    while (true) { delay(1000); }
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWi-Fi connection failed! Rebooting...");
    ESP.restart();
  }
  Serial.printf("\nConnected! IP: %s | RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  configureI2S_Mic();
  configureI2S_Speaker();
  flushMicDMA();

  // Plain ws:// LAN connection (zero mbedTLS overhead)
  webSocket.begin(BRIDGE_HOST, BRIDGE_PORT, BRIDGE_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);
}

void loop() {
  webSocket.loop();

  // Safety fallback: if host stops sending binary audio frames without TTS_END,
  // automatically zero speaker DMA and return to LISTENING after timeout
  if (currentState == STATE_SPEAKING && (millis() - lastRxAudioMs > PLAYBACK_IDLE_TIMEOUT_MS)) {
    enterListeningState();
  }

  if (!webSocket.isConnected()) {
    delay(2);
    return;
  }

  if (currentState == STATE_LISTENING) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_MIC_PORT,
                             (void*)psramMicRaw32,
                             MIC_FRAME_SAMPLES * sizeof(int32_t),
                             &bytes_read,
                             20 / portTICK_PERIOD_MS);

    if (err == ESP_OK && bytes_read > 0) {
      size_t samples_read = bytes_read / sizeof(int32_t);
      for (size_t i = 0; i < samples_read; i++) {
        psramMicPcm16[i] = (int16_t)(psramMicRaw32[i] >> 11);
      }
      webSocket.sendBIN((uint8_t*)psramMicPcm16, samples_read * sizeof(int16_t));
    }
  } else {
    delay(1);
  }
}
