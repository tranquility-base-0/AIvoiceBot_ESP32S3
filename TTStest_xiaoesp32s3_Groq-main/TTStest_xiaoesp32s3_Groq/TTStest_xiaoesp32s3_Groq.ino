#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "config4.h"

// WiFi credentials from config4.h
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Groq API key from config4.h
const char* groqApiKey = GROQ_API_KEY;

// I2S pins for XIAO ESP32S3 to MAX98357A
#define I2S_BCLK 8   // Bit Clock (BCK)
#define I2S_LRC  7   // Left/Right Clock (LCK/WS)
#define I2S_DOUT 9   // Data Out (DIN)

// === Little-endian helpers ===
uint16_t readLE16(uint8_t *buf) {
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

uint32_t readLE32(uint8_t *buf) {
  return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
         ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

// === WAV header parser ===
void parseWavHeader(uint8_t *header) {
  uint16_t numChannels   = readLE16(&header[22]);
  uint32_t sampleRate    = readLE32(&header[24]);
  uint16_t bitsPerSample = readLE16(&header[34]);

  Serial.println("=== WAV HEADER INFO ===");
  Serial.print("Channels: "); Serial.println(numChannels);
  Serial.print("Sample Rate: "); Serial.println(sampleRate);
  Serial.print("Bits Per Sample: "); Serial.println(bitsPerSample);
  Serial.println("=======================");
}

// === I2S setup ===
void setupI2S(uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels) {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = sampleRate,
    .bits_per_sample = (i2s_bits_per_sample_t)bitsPerSample,
    .channel_format = (numChannels == 2) ? I2S_CHANNEL_FMT_RIGHT_LEFT : I2S_CHANNEL_FMT_ONLY_LEFT,
    //.communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .communication_format = I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
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

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// === TTS playback ===
void playGroqTTS(String text) {
  Serial.println("Generating TTS with Groq/PlayAI: " + text);

  if (text.length() > 10000) {
    Serial.println("Text too long! Max 10K characters.");
    return;
  }

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/audio/speech");

  // Set headers
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(groqApiKey));

  // Create JSON request body
  DynamicJsonDocument doc(2048);
  doc["model"] = "playai-tts";
  doc["input"] = text;
  doc["voice"] = "Fritz-PlayAI";
  doc["response_format"] = "wav";

  String requestBody;
  serializeJson(doc, requestBody);

  Serial.println("Sending request to Groq...");
  int httpResponseCode = http.POST(requestBody);

  if (httpResponseCode == 200) {
    Serial.println("Success! Streaming audio...");
    WiFiClient* stream = http.getStreamPtr();

    // === Read header aligned to RIFF ===
    uint8_t header[44];
    bool foundRIFF = false;
    while (!foundRIFF) {
      if (stream->available()) {
        uint8_t b = stream->read();
        if (b == 'R') {
          while (stream->available() < 3) delay(1);
          uint8_t next[3];
          stream->readBytes(next, 3);
          if (next[0] == 'I' && next[1] == 'F' && next[2] == 'F') {
            // Found RIFF, store in header
            header[0] = 'R';
            header[1] = 'I';
            header[2] = 'F';
            header[3] = 'F';
            int headerBytesRead = 4;
            while (headerBytesRead < 44) {
              if (stream->available()) {
                header[headerBytesRead++] = stream->read();
              } else {
                delay(1);
              }
            }
            foundRIFF = true;
          }
        }
      } else {
        delay(1);
      }
    }

    // Print header info
    parseWavHeader(header);

    // Extract values
    uint16_t numChannels   = readLE16(&header[22]);
    uint32_t sampleRate    = readLE32(&header[24]);
    uint16_t bitsPerSample = readLE16(&header[34]);
    uint32_t dataSize      = readLE32(&header[40]);

    Serial.printf("Configuring I2S: %u Hz, %u bits, %u channels\n",
                  sampleRate, bitsPerSample, numChannels);
    Serial.printf("Expecting %u bytes of audio data\n", dataSize);

    // Reconfigure I2S dynamically
    i2s_driver_uninstall(I2S_NUM_0);
    setupI2S(sampleRate, bitsPerSample, numChannels);

    Serial.println("WAV header skipped, playing audio data...");

    // === Playback loop ===
    const size_t chunkSize = 1024;
    uint8_t audioBuffer[chunkSize];
    uint32_t totalPlayed = 0;

    while (totalPlayed < dataSize) {
      if (stream->available()) {
        size_t bytesToRead = std::min((uint32_t)chunkSize, dataSize - totalPlayed);
        size_t bytesRead = stream->readBytes(audioBuffer, bytesToRead);

        if (bytesRead > 0) {
          if (bytesRead % 2 != 0) bytesRead--; // align to 16-bit

          size_t bytes_written;
          esp_err_t result = i2s_write(I2S_NUM_0, audioBuffer, bytesRead, &bytes_written, 500);
          if (result == ESP_OK) {
            totalPlayed += bytes_written;
          }
        }
      } else {
        delay(1);
      }
    }

    Serial.println("Audio playback complete!");
  } else {
    Serial.println("HTTP Error: " + String(httpResponseCode));
    String errorResponse = http.getString();
    Serial.println("Response: " + errorResponse);
  }

  http.end();
}

// === Arduino entry points ===
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Connect WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi connected!");

  // Init I2S with a safe default (will reconfigure later)
  setupI2S(48000, 16, 1);

  // Test TTS once
  delay(2000);
  playGroqTTS("Hello world! This is a text to speech test using Groq and PlayAI on the XIAO ESP32S3.");
}

void loop() {
  // Do nothing (only speak once)
}
