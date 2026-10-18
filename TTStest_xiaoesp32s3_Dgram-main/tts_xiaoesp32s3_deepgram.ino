#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino.h>
#include "driver/i2s.h"
#include "config.h"

// I2S pin definitions (as you requested)
#define I2S_BCLK 8   // Bit Clock (BCK)
#define I2S_LRC  7   // Left/Right Clock (LCK/WS)
#define I2S_DOUT 9   // Data Out (DIN)

// Debug switches
#define DEBUG_PRINT_HEADER  true   // prints first header bytes
#define DEBUG_PRINT_HEX     false  // prints hex of every chunk (can flood Serial)

// Buffer sizes
#define BUFFER_SIZE 512   // bytes incoming chunk size (must be multiple of 2 for 16-bit PCM)

uint8_t buffer[BUFFER_SIZE];
int16_t stereoBuf[BUFFER_SIZE]; // enough room: BUFFER_SIZE bytes => BUFFER_SIZE/2 samples; stereo expands to same # int16_t entries

// Function declarations
void setupWiFi();
void setupI2S();
void speakText(const char* text);
void playTestTone(int freqHz, int durationMs);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Deepgram ESP32-S3 Voice Bot ===");

  setupWiFi();
  setupI2S();

  // optional: audible test tone to verify wiring & amp
  playTestTone(1000, 800); // 1 kHz for 800 ms

  // Test TTS
  speakText("Hello! This is your Deepgram voice bot speaking clearly through the ESP32-S3 audio output system.");
}

void loop() {
  // nothing here for now
  delay(10000);
}

// ======== WIFI SETUP ========
void setupWiFi() {
  Serial.println("[WiFi] Connecting to network...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttemptTime = millis();
  const unsigned long wifiTimeout = 20000; // 20 seconds

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] ✅ Connected!");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\n[WiFi] ❌ Connection FAILED. Check SSID/password or signal strength.");
  }
}

// ======== I2S SETUP ========
void setupI2S() {
  Serial.println("[I2S] Initializing I2S driver...");

  // Recommended config for MAX98357A: standard I2S, 16-bit, stereo frames
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // use stereo frames (we will duplicate mono -> stereo)
    .communication_format = I2S_COMM_FORMAT_I2S,  // standard I2S
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t err;
  err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] ❌ Driver install failed! Error code: %d\n", err);
    return;
  }

  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S] ❌ Pin setup failed! Error code: %d\n", err);
    return;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("[I2S] ✅ I2S driver initialized successfully.");
}

// ======== PLAY A TEST TONE (mono) ========
void playTestTone(int freqHz, int durationMs) {
  Serial.printf("[TEST] Sending %d Hz tone for %d ms...\n", freqHz, durationMs);

  const int sampleRate = 24000;
  const int16_t amplitude = 8000; // moderate amplitude
  int totalSamples = (sampleRate * durationMs) / 1000;

  const int chunkSamples = 256; // generate in small chunks
  int16_t genBuf[chunkSamples * 2]; // stereo buffer (interleaved)

  int sampleIndex = 0;
  while (sampleIndex < totalSamples) {
    int batch = min(chunkSamples, totalSamples - sampleIndex);
    for (int i = 0; i < batch; ++i) {
      float t = (float)(sampleIndex + i) / sampleRate;
      int16_t s = (int16_t)(sin(2.0f * PI * freqHz * t) * amplitude);
      // duplicate to stereo
      genBuf[i*2] = s;
      genBuf[i*2 + 1] = s;
    }
    size_t bytes_written;
    i2s_write(I2S_NUM_0, genBuf, batch * 2 * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    sampleIndex += batch;
  }

  Serial.println("[TEST] Done.");
}

// ======== DEEPGRAM TTS FUNCTION ========
void speakText(const char* text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TTS] ❌ WiFi not connected. Cannot contact Deepgram.");
    return;
  }

  Serial.println("[TTS] Preparing Deepgram TTS request...");
  HTTPClient http;

  // Request PCM16 linear16 at 24000 Hz for model aura-2-thalia-en
  String url = "https://api.deepgram.com/v1/speak?model=aura-2-thalia-en&encoding=linear16";
  http.begin(url);

  http.useHTTP10(true);  // 🔑 prevents "Transfer-Encoding: chunked"
  http.addHeader("Authorization", String("Token ") + DEEPGRAM_API_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "audio/wav");  // Request pure binary WAV output
  String requestBody = String("{\"text\": \"") + text + "\"}";

  Serial.println("[TTS] Sending POST request to Deepgram...");
  Serial.print("[TTS] Request body: ");
  Serial.println(requestBody);

  int httpCode = http.POST(requestBody);

  if (httpCode > 0) {
    Serial.printf("[TTS] HTTP response code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      Serial.println("[TTS] ✅ Deepgram connection successful! Streaming audio...");

      WiFiClient* stream = http.getStreamPtr();
      size_t totalBytesPlayed = 0;
      size_t totalReceived = 0;
      bool headerSkipped = false;
      size_t headerBytesSkipped = 0;
      const size_t wavHeaderSize = 44;
      bool printedHeaderFirst = false;

      while (http.connected()) {
        int len = stream->readBytes(buffer, sizeof(buffer));
        if (len <= 0) break;
        totalReceived += len;

        // On first data, optionally print leading bytes for diagnosis
        if (!printedHeaderFirst && DEBUG_PRINT_HEADER) {
          Serial.print("[TTS] First bytes: ");
          for (int i = 0; i < min(len, 128); i++) {
            if (buffer[i] < 0x10) Serial.print("0");
            Serial.print(buffer[i], HEX);
            Serial.print(" ");
          }
          Serial.println();
          printedHeaderFirst = true;
        }

        if (DEBUG_PRINT_HEX) {
          Serial.print("[TTS] Chunk (len=");
          Serial.print(len);
          Serial.print("): ");
          for (int i = 0; i < len; i++) {
            if (buffer[i] < 0x10) Serial.print("0");
            Serial.print(buffer[i], HEX);
            Serial.print(" ");
          }
          Serial.println();
        }

        // Skip WAV header bytes (44 bytes) BEFORE decoding/playing
        int procOffset = 0;
        int procLen = len;
        if (!headerSkipped) {
          if (headerBytesSkipped + (size_t)len < wavHeaderSize) {
            headerBytesSkipped += len;
            // still inside header; skip entire chunk
            continue;
          } else {
            // chunk contains the tail of the header; remove header bytes first
            size_t skip = wavHeaderSize - headerBytesSkipped;
            if (skip < (size_t)len) {
              memmove(buffer, buffer + skip, len - skip);
              procLen = len - skip;
              procOffset = 0;
            } else {
              // unlikely, handled above
              continue;
            }
            headerSkipped = true;
            headerBytesSkipped = wavHeaderSize;
            Serial.println("[TTS] WAV header skipped. Beginning PCM playback.");
          }
        }

        // procLen now is number of PCM bytes (16-bit little endian mono)
        // Ensure procLen is even (16-bit samples)
        if (procLen <= 0) continue;
        if ((procLen & 1) != 0) {
          // drop the last trailing byte (should not normally happen)
          procLen -= 1;
        }

        // Duplicate mono(16bit) -> stereo interleaved (16-bit)
        int16_t* pcm16 = (int16_t*)(buffer + procOffset);
        int sampleCount = procLen / 2; // number of 16-bit samples (mono)
        // stereoBuf size is BUFFER_SIZE int16_t entries; sampleCount*2 must not exceed BUFFER_SIZE
        if (sampleCount * 2 > (int)(sizeof(stereoBuf)/sizeof(int16_t))) {
          Serial.println("[TTS] ⚠️ chunk too large for stereoBuf - truncating");
          sampleCount = (sizeof(stereoBuf)/sizeof(int16_t)) / 2;
        }

        for (int i = 0, j = 0; i < sampleCount; ++i, j += 2) {
          int16_t s = pcm16[i];
          stereoBuf[j]     = s; // left
          stereoBuf[j + 1] = s; // right
        }

        size_t bytesToWrite = (size_t)sampleCount * 2 * sizeof(int16_t); // sampleCount * 2 channels * 2 bytes
        size_t bytes_written = 0;
        esp_err_t res = i2s_write(I2S_NUM_0, stereoBuf, bytesToWrite, &bytes_written, portMAX_DELAY);
        if (res != ESP_OK) {
          Serial.printf("[TTS] i2s_write failed err=%d\n", (int)res);
          break;
        }
        totalBytesPlayed += bytes_written;
      } // while stream

      Serial.printf("[TTS] ✅ Audio stream finished. Total bytes received: %u, total bytes played: %u\n",
                    (unsigned int)totalReceived, (unsigned int)totalBytesPlayed);
    } else {
      Serial.printf("[TTS] ❌ Deepgram returned error code %d\n", httpCode);
      String body = http.getString();
      Serial.print("[TTS] Body: ");
      Serial.println(body);
    }
  } else {
    Serial.printf("[TTS] ❌ HTTP POST failed. Error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}
