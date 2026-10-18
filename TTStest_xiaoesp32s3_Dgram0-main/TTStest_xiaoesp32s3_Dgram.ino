#include "config4.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// WiFi credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Deepgram API
const char* deepgram_api_key = DEEPGRAM_API_KEY;
const char* deepgram_url = "https://api.deepgram.com/v1/speak?model=aura-2-cora-en&encoding=linear16";

// I2S pins for MAX98357A
#define I2S_BCLK_PIN  8   // Bit clock
#define I2S_LRC_PIN   7   // Left/Right clock (WS)
#define I2S_DIN_PIN   9   // Data input
#define I2S_NUM       I2S_NUM_0

// Audio buffer
uint8_t* audio_buffer = nullptr;
size_t audio_buffer_size = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("Starting Deepgram TTS Test...");
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Initialize I2S for audio playback
  setupI2S();
  
  // Test TTS
  delay(2000);
  testTTS("Hello world, this is a test from ESP32!");
}

void loop() {
  // Play audio every 10 seconds
  delay(10000);
  testTTS("Testing Deepgram text to speech on ESP32!");
}

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000,  // Deepgram typically returns 24kHz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // Changed to stereo
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,  // Increased buffer size
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRC_PIN,
    .data_out_num = I2S_DIN_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t result = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  if (result != ESP_OK) {
    Serial.printf("Failed to install I2S driver: %d\n", result);
    return;
  }

  result = i2s_set_pin(I2S_NUM, &pin_config);
  if (result != ESP_OK) {
    Serial.printf("Failed to set I2S pins: %d\n", result);
    return;
  }

  // Clear any existing data in I2S DMA buffers
  i2s_zero_dma_buffer(I2S_NUM);

  Serial.println("I2S initialized successfully");
}

void testTTS(const char* text) {
  Serial.printf("Converting text to speech: %s\n", text);
  
  if (getAudioFromDeepgram(text)) {
    Serial.println("Audio streaming completed successfully!");
  } else {
    Serial.println("Failed to get audio from Deepgram");
  }
  
  // No need to clean up buffer since we're streaming directly
}

bool getAudioFromDeepgram(const char* text) {
  HTTPClient http;
  http.begin(deepgram_url);
  http.addHeader("Authorization", String("Token ") + deepgram_api_key);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000); // 15 second timeout
  
  // Create JSON payload
  DynamicJsonDocument doc(1024);
  doc["text"] = text;
  String jsonString;
  serializeJson(doc, jsonString);

  Serial.println("Sending request to Deepgram...");
  Serial.printf("Request size: %d bytes\n", jsonString.length());
  
  int httpResponseCode = http.POST(jsonString);
  
  Serial.printf("HTTP Response Code: %d\n", httpResponseCode);

  if (httpResponseCode == 200) {
    WiFiClient* stream = http.getStreamPtr();
    
    Serial.println("Streaming and playing audio directly...");
    
    // Play audio as we receive it to save memory
    const size_t chunk_size = 1024;
    uint8_t temp_buffer[chunk_size];
    int16_t stereo_buffer[chunk_size];
    size_t total_bytes = 0;
    bool first_chunk = true;
    bool header_skipped = false;
    size_t header_skip_bytes = 0;
    int timeout_count = 0;
    
    while ((stream->connected() || stream->available()) && timeout_count < 100) {
      if (stream->available()) {
        size_t available = stream->available();
        size_t to_read = (available < chunk_size) ? available : chunk_size;
        size_t bytes_read = stream->readBytes(temp_buffer, to_read);
        
        if (bytes_read > 0) {
          timeout_count = 0; // Reset timeout counter
          total_bytes += bytes_read;
          
          // Debug first chunk to see the actual data format
          if (first_chunk) {
            Serial.printf("First chunk (%d bytes): ", bytes_read);
            for (int i = 0; i < min(16, (int)bytes_read); i++) {
              Serial.printf("%02X ", temp_buffer[i]);
            }
            Serial.println();
            first_chunk = false;
          }
          
          uint8_t* audio_data = temp_buffer;
          size_t audio_data_size = bytes_read;
          
          // Skip any header data in the first chunks
          if (!header_skipped) {
            // Look for the start of audio data after headers
            for (size_t i = 0; i < bytes_read - 8; i++) {
              // Look for "data" chunk marker in WAV file
              if (temp_buffer[i] == 'd' && temp_buffer[i+1] == 'a' && 
                  temp_buffer[i+2] == 't' && temp_buffer[i+3] == 'a') {
                header_skip_bytes = i + 8; // Skip "data" + size (4 bytes each)
                Serial.printf("Found WAV data chunk at offset %d\n", header_skip_bytes);
                header_skipped = true;
                break;
              }
            }
            
            // If we found the data chunk, skip to audio data
            if (header_skipped && header_skip_bytes < bytes_read) {
              audio_data = temp_buffer + header_skip_bytes;
              audio_data_size = bytes_read - header_skip_bytes;
              Serial.printf("Skipping %d header bytes, playing %d audio bytes\n", 
                           header_skip_bytes, audio_data_size);
            } else if (!header_skipped) {
              // Still looking for data chunk, skip this chunk
              continue;
            }
          }
          
          // Only process even number of bytes for 16-bit samples
          size_t bytes_to_process = (audio_data_size / 2) * 2;
          size_t samples = bytes_to_process / 2; // 16-bit samples
          
          if (samples > 0) {
            int16_t* mono_samples = (int16_t*)audio_data;
            
            for (size_t i = 0; i < samples; i++) {
              stereo_buffer[i * 2] = mono_samples[i];     // Left
              stereo_buffer[i * 2 + 1] = mono_samples[i]; // Right
            }
            
            size_t bytes_to_write = samples * 4; // stereo 16-bit
            size_t bytes_written;
            
            esp_err_t result = i2s_write(I2S_NUM, stereo_buffer, bytes_to_write, &bytes_written, 1000);
            if (result != ESP_OK) {
              Serial.printf("I2S write error: %d\n", result);
            }
          }
          
          if (total_bytes % 16384 == 0) {
            Serial.printf("Streamed and played %d bytes...\n", total_bytes);
          }
        }
      } else {
        timeout_count++;
        delay(50);
      }
    }
    
    if (timeout_count >= 100) {
      Serial.println("Stream timeout reached");
    }
    
    Serial.printf("Streaming completed. Total: %d bytes\n", total_bytes);
    http.end();
    
    // Set dummy values for compatibility with existing code
    audio_buffer_size = total_bytes;
    return total_bytes > 0;
    
  } else {
    Serial.printf("HTTP error: %d\n", httpResponseCode);
    if (httpResponseCode == -1) {
      Serial.println("Connection timeout - check network or API key");
    }
    
    // Try to get response body for more info
    if (http.connected()) {
      String response = http.getString();
      Serial.println("Response: " + response);
    }
  }

  http.end();
  return false;
}

void playAudio() {
  if (!audio_buffer || audio_buffer_size == 0) {
    Serial.println("No audio data to play");
    return;
  }

  Serial.println("Starting audio playback...");
  
  // Show raw data analysis
  Serial.printf("Audio buffer size: %d bytes\n", audio_buffer_size);
  Serial.printf("First 16 bytes (hex): ");
  for (int i = 0; i < min(16, (int)audio_buffer_size); i++) {
    Serial.printf("%02X ", audio_buffer[i]);
  }
  Serial.println();
  
  // Treat as raw PCM data (no header to skip)
  uint8_t* ptr = audio_buffer;
  size_t data_size = audio_buffer_size;
  
  Serial.printf("Playing %d bytes as raw PCM data\n", data_size);
  
  // Convert mono to stereo and play
  const size_t chunk_size = 512;
  int16_t stereo_buffer[chunk_size];
  
  size_t total_written = 0;
  size_t samples_processed = 0;
  
  while (total_written < data_size - 1) { // -1 to avoid reading past buffer
    size_t samples_to_process = min(chunk_size / 2, (data_size - total_written) / 2);
    
    if (samples_to_process == 0) break;
    
    // Convert mono samples to stereo
    int16_t* mono_samples = (int16_t*)(ptr + total_written);
    for (size_t i = 0; i < samples_to_process; i++) {
      int16_t sample = mono_samples[i];
      stereo_buffer[i * 2] = sample;     // Left channel
      stereo_buffer[i * 2 + 1] = sample; // Right channel (duplicate)
    }
    
    size_t bytes_to_write = samples_to_process * 4; // 2 channels * 2 bytes each
    size_t bytes_written;
    
    esp_err_t result = i2s_write(I2S_NUM, stereo_buffer, bytes_to_write, &bytes_written, 1000);
    if (result != ESP_OK) {
      Serial.printf("I2S write error: %d\n", result);
      break;
    }
    
    total_written += samples_to_process * 2; // Original mono bytes consumed
    samples_processed += samples_to_process;
    
    // Show progress every 1000 samples
    if (samples_processed % 1000 == 0) {
      Serial.printf("Played %d samples so far...\n", samples_processed);
    }
    
    // Small delay to prevent buffer overflow
    delay(1);
  }
  
  Serial.printf("Audio playback completed. Processed %d samples (%d bytes)\n", 
                samples_processed, total_written);
                
  // Add a small delay to let the audio finish
  delay(100);
}