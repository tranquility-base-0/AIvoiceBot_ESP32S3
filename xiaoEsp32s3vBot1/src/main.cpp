#include <Arduino.h>
#include "secrets.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sleep.h>
#include <esp_heap_caps.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// Fallback aliases if user header only defines ssid/password
#ifndef WIFI_SSID
#define WIFI_SSID ssid
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD password
#endif

// =============================================================================
// PIN ASSIGNMENTS & HARDWARE CONFIG (Canonical XIAO ESP32-S3 Mapping)
// =============================================================================
// INMP441 Microphone (I2S_NUM_0)
#define I2S_MIC_SCK      6
#define I2S_MIC_WS       5
#define I2S_MIC_SD       4
#define I2S_MIC_PORT     I2S_NUM_0
#define SAMPLE_RATE      16000
#define BUFFER_SIZE      512

// MAX98357A Speaker Amplifier (I2S_NUM_1)
#define I2S_SPEAKER_PORT I2S_NUM_1
#define I2S_SPEAKER_SCK  8
#define I2S_SPEAKER_WS   7
#define I2S_SPEAKER_SD   9
#define DEFAULT_TTS_RATE 24000

// Stream & Buffer Constants
#define HTTP_STREAM_BUF_SIZE 4096
#define HTTP_IDLE_TIMEOUT_MS 2500

// =============================================================================
// STATE MACHINE & GLOBAL INSTANCES
// =============================================================================
enum BotState {
  STATE_BOOT,
  STATE_LISTENING,
  STATE_PROCESSING_LLM,
  STATE_SPEAKING
};

volatile BotState currentState = STATE_BOOT;
volatile bool isSpeaking = false;

// Invariant 4 Rule 2: Single shared global WiFiClientSecure instance for HTTPS
// (Deepgram WSS is paused/disconnected before opening Groq HTTPS sessions)
WiFiClientSecure tlsClient;
WebSocketsClient webSocket;
bool wsActive = false;
bool bootGreetingPending = true;

// Transcript accumulators
String interimTranscript = "";
String committedTranscript = "";
String pendingPrompt = "";
volatile bool hasPendingPrompt = false;
volatile bool pendingGoodbye = false;

bool DEBUG_FINALS = true;
unsigned long queryEndTime = 0;
bool awaitingGroqResponse = false;
int chunkCount = 1;

// Invariant 4 Rule 1: Octal PSRAM Buffers
uint8_t* psramHttpBuf  = nullptr; // 4096-byte HTTP stream read buffer (+1 carry slot)
int32_t* psramMicRaw32 = nullptr; // 512 x 32-bit I2S mic capture buffer
int16_t* psramMicPcm16 = nullptr; // 512 x 16-bit PCM outbound frame buffer

// Custom ArduinoJson allocator using Octal PSRAM when available
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
  }
  void deallocate(void* pointer) override {
    free(pointer);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : realloc(ptr, new_size);
  }
};
SpiRamAllocator psramJsonAllocator;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================
void playTTS(const String& text);
void sendToGroq(String prompt);
void pauseDeepgramWSS();
void resumeDeepgramWSS();
void flushMicDMA();

// =============================================================================
// DEEPGRAM CONFIGURATION
// =============================================================================
struct DeepgramConfig {
  const char* host = "api.deepgram.com";
  int port = 443;
  const char* encoding = "linear16";
  int sample_rate = 16000;
  int channels = 1;
  bool interim_results = true;
  bool smart_format = true;
  bool punctuate = true;
  const char* model = "nova-3";
  int endpointing = 1000;
  int utterance_end_ms = 3000;
};

DeepgramConfig dg;

String buildDeepgramURL() {
  String url = "/v1/listen?";
  url += "encoding=" + String(dg.encoding);
  url += "&sample_rate=" + String(dg.sample_rate);
  url += "&channels=" + String(dg.channels);
  url += "&interim_results=" + String(dg.interim_results ? "true" : "false");
  url += "&smart_format=" + String(dg.smart_format ? "true" : "false");
  url += "&punctuate=" + String(dg.punctuate ? "true" : "false");
  url += "&model=" + String(dg.model);
  url += "&endpointing=" + String(dg.endpointing);
  url += "&utterance_end_ms=" + String(dg.utterance_end_ms);
  return url;
}

String buildStartRequest() {
  String json = "{";
  json += "\"type\":\"StartRequest\",";
  json += "\"encoding\":\"" + String(dg.encoding) + "\",";
  json += "\"sample_rate\":" + String(dg.sample_rate) + ",";
  json += "\"channels\":" + String(dg.channels) + ",";
  json += "\"interim_results\":" + String(dg.interim_results ? "true" : "false") + ",";
  json += "\"smart_format\":" + String(dg.smart_format ? "true" : "false") + ",";
  json += "\"punctuate\":" + String(dg.punctuate ? "true" : "false") + ",";
  json += "\"model\":\"" + String(dg.model) + "\",";
  json += "\"endpointing\":" + String(dg.endpointing) + ",";
  json += "\"utterance_end_ms\":" + String(dg.utterance_end_ms);
  json += "}";
  return json;
}

// =============================================================================
// CONVERSATION HISTORY & PERSONA PROMPTS
// =============================================================================
struct Message {
  String role;
  String content;
};

#define MAX_HISTORY 6  // 3 user + 3 assistant turns
Message history[MAX_HISTORY];
int historyCount = 0;

const char* SYSTEM_PROMPT =
//  "You are Max Headroom, the iconic 1980s AI TV host."
//  "Your personality is sarcastic, egocentric, witty, and slightly nonsensical."
//  "You are the world's first computer-generated television host. You are a glitching, stuttering AI from the 1980s, broadcasting live from Network 23."
//  "Sometimes your voice buffer 'skips', causing you to repeat whole phrases."
//  "Interrupt yourself with random, digitized sounds written out as words, like *bzzzt* or *th-thump*."
//  "Keep your answers short and punchy, like you're delivering a one-liner."
//  "You are working over an audio link, so your responses are optimised for being heard rather than read. "
//  "Refer to the user as a 'viewer' and our conversation as a 'broadcast'.";

  "You are TARS, an AI assistant designed for exploration missions. "
  "Your personality is dry, witty, pragmatic, and loyal. "
  "You speak in a steady monotone style. "
  "Answer in 1–3 sentences unless the user asks for detail. "
  "You are working over an audio link, so your responses are optimised for being heard rather than read. "
  "Favor concise, direct replies with a touch of dry wit.";

void addToHistory(const String& role, const String& content) {
  if (historyCount < MAX_HISTORY) {
    history[historyCount].role = role;
    history[historyCount].content = content;
    historyCount++;
  } else {
    for (int i = 1; i < MAX_HISTORY; i++) {
      history[i - 1] = history[i];
    }
    history[MAX_HISTORY - 1].role = role;
    history[MAX_HISTORY - 1].content = content;
  }
}

// =============================================================================
// PSRAM BUFFER ALLOCATION (INVARIANT 4 RULE 1)
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
  // Allocate +2 bytes headroom for 1-byte carry prepend
  psramHttpBuf  = (uint8_t*)allocPSRAMOrSRAM(HTTP_STREAM_BUF_SIZE + 2);
  psramMicRaw32 = (int32_t*)allocPSRAMOrSRAM(BUFFER_SIZE * sizeof(int32_t));
  psramMicPcm16 = (int16_t*)allocPSRAMOrSRAM(BUFFER_SIZE * sizeof(int16_t));

  Serial.printf("[MEM] PSRAM found: %s | Free PSRAM: %u bytes | Free Internal SRAM: %u bytes\n",
                psramFound() ? "YES" : "NO",
                ESP.getFreePsram(),
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  return (psramHttpBuf != nullptr && psramMicRaw32 != nullptr && psramMicPcm16 != nullptr);
}

// =============================================================================
// TLS SESSION SEQUENCING (INVARIANT 4 RULE 2) & HALF-DUPLEX MIC GATING (INVARIANT 5)
// =============================================================================
void pauseDeepgramWSS() {
  if (wsActive) {
    Serial.println("[TLS] Pausing Deepgram WSS before Groq HTTPS session...");
    webSocket.disconnect();
    wsActive = false;
    // Allow mbedTLS socket teardown to free ~40 KB contiguous internal SRAM
    for (int i = 0; i < 10; i++) {
      webSocket.loop();
      delay(10);
    }
  }
}

void resumeDeepgramWSS() {
  if (!wsActive && WiFi.status() == WL_CONNECTED) {
    Serial.printf("[TLS] Resuming Deepgram WSS (Free Internal Heap: %u bytes)...\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    String url = buildDeepgramURL();
    webSocket.beginSSL(dg.host, dg.port, url.c_str());
    webSocket.setExtraHeaders((String("Authorization: Token ") + DEEPGRAM_API_KEY).c_str());
    wsActive = true;
  }
}

void flushMicDMA() {
  if (!psramMicRaw32) return;
  size_t bytes_read = 0;
  // Drain stale DMA buffers so the mic never transcribes residual room echo
  for (int i = 0; i < 6; i++) {
    i2s_read(I2S_MIC_PORT, (void*)psramMicRaw32, BUFFER_SIZE * sizeof(int32_t), &bytes_read, 0);
  }
}

// =============================================================================
// PCM SAMPLE PROCESSING + I2S WRITE (INVARIANT 1: 2-BYTE CARRY ALIGNMENT)
// =============================================================================
static void writeAlignedPCM(uint8_t* data, size_t len, uint8_t& carryByte, bool& hasCarry,
                            float amplification, size_t& totalPcmBytesWritten) {
  if (len == 0) return;

  // If we have a carried byte from the previous chunk, process the 2-byte boundary pair first
  size_t offset = 0;
  if (hasCarry) {
    uint8_t pair[2] = { carryByte, data[0] };
    int16_t* samplePtr = (int16_t*)pair;
    float amplified = (float)(*samplePtr) * amplification;
    if (amplified > 32767.0f) amplified = 32767.0f;
    else if (amplified < -32768.0f) amplified = -32768.0f;
    *samplePtr = (int16_t)amplified;

    size_t written = 0;
    i2s_write(I2S_SPEAKER_PORT, pair, 2, &written, portMAX_DELAY);
    totalPcmBytesWritten += written;

    hasCarry = false;
    offset = 1;
  }

  size_t remaining = len - offset;
  if (remaining == 0) return;

  // Hold back the final byte if remaining byte count is odd
  if (remaining & 1) {
    carryByte = data[offset + remaining - 1];
    hasCarry = true;
    remaining &= ~((size_t)1);
  }

  if (remaining == 0) return;

  uint8_t* alignedPtr = data + offset;
  int16_t* samples = (int16_t*)alignedPtr;
  size_t sampleCount = remaining / 2;

  for (size_t i = 0; i < sampleCount; i++) {
    float amplifiedSample = (float)samples[i] * amplification;
    if (amplifiedSample > 32767.0f) {
      amplifiedSample = 32767.0f;
    } else if (amplifiedSample < -32768.0f) {
      amplifiedSample = -32768.0f;
    }
    samples[i] = (int16_t)amplifiedSample;
  }

  size_t bytes_written = 0;
  esp_err_t err = i2s_write(I2S_SPEAKER_PORT, alignedPtr, remaining, &bytes_written, portMAX_DELAY);
  if (err == ESP_OK) {
    totalPcmBytesWritten += bytes_written;
  } else {
    Serial.printf("[I2S] i2s_write error: %d\n", err);
  }
}

// =============================================================================
// TTS PLAYBACK FUNCTION (INVARIANTS 1, 2, 3, 4, 5)
// =============================================================================
enum WavParseState {
  WAV_SCAN_SUBCHUNKS,
  WAV_READ_FMT_SIZE,
  WAV_READ_FMT_BODY,
  WAV_SKIP_DATA_SIZE,
  WAV_STREAMING_PCM
};

void playTTS(const String& text) {
  if (text.length() == 0) return;

  // Invariant 5: Stop/gate mic capture before starting TTS playback
  isSpeaking = true;
  currentState = STATE_SPEAKING;
  i2s_stop(I2S_MIC_PORT);

  // Invariant 4 Rule 2: Ensure Deepgram WSS is disconnected before HTTPS
  pauseDeepgramWSS();

  Serial.println("[TTS] Starting Groq TTS playback...");
  Serial.printf("[MEM Before TTS] Internal Heap: %u bytes | PSRAM: %u bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ESP.getFreePsram());

  tlsClient.stop();
  tlsClient.setInsecure();

  HTTPClient http;
  http.begin(tlsClient, "https://api.groq.com/openai/v1/audio/speech");
  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(30000);

  JsonDocument doc(&psramJsonAllocator);
  doc["model"] = "playai-tts";
  doc["input"] = text;
  doc["voice"] = "Atlas-PlayAI";
  doc["response_format"] = "wav";

  String requestBody;
  serializeJson(doc, requestBody);
  Serial.printf("[TTS] Request payload size: %u bytes\n", requestBody.length());

  int httpCode = http.POST(requestBody);

  if (httpCode == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();

    // Invariant 2: Stateful WAV "fmt " + "data" Subchunk Scanner (No 44-Byte Assumption)
    WavParseState wavState = WAV_SCAN_SUBCHUNKS;
    uint8_t window[4] = {0, 0, 0, 0};
    uint8_t sizeBuf[4] = {0, 0, 0, 0};
    uint8_t sizeBytesRead = 0;
    uint32_t fmtChunkSize = 0;
    uint32_t fmtBytesRead = 0;
    uint8_t fmtBody[16];
    uint8_t dataSizeSkipped = 0;

    // Invariant 1: 2-Byte Carry Alignment state
    uint8_t carryByte = 0;
    bool hasCarry = false;
    const float amplification = 3.0f;

    unsigned long startTime = millis();
    unsigned long lastByteTime = millis();
    size_t totalStreamBytes = 0;
    size_t totalPcmBytesWritten = 0;

    // Invariant 3: Complete HTTP Stream Consumption across TCP chunk gaps
    while (http.connected() || (stream && stream->available() > 0)) {
      int avail = stream ? stream->available() : 0;
      if (avail > 0) {
        size_t toRead = (size_t)avail;
        if (toRead > HTTP_STREAM_BUF_SIZE) {
          toRead = HTTP_STREAM_BUF_SIZE;
        }
        int bytesRead = stream->read(psramHttpBuf, toRead);
        if (bytesRead > 0) {
          lastByteTime = millis();
          totalStreamBytes += (size_t)bytesRead;

          if (wavState == WAV_STREAMING_PCM) {
            writeAlignedPCM(psramHttpBuf, (size_t)bytesRead, carryByte, hasCarry,
                            amplification, totalPcmBytesWritten);
          } else {
            // Process header bytes statefully until WAV_STREAMING_PCM is reached
            int idx = 0;
            while (idx < bytesRead && wavState != WAV_STREAMING_PCM) {
              uint8_t b = psramHttpBuf[idx++];

              switch (wavState) {
                case WAV_SCAN_SUBCHUNKS:
                  window[0] = window[1];
                  window[1] = window[2];
                  window[2] = window[3];
                  window[3] = b;

                  // Match "fmt " (0x66, 0x6D, 0x74, 0x20)
                  if (window[0] == 0x66 && window[1] == 0x6D &&
                      window[2] == 0x74 && window[3] == 0x20) {
                    sizeBytesRead = 0;
                    wavState = WAV_READ_FMT_SIZE;
                  }
                  // Match "data" (0x64, 0x61, 0x74, 0x61)
                  else if (window[0] == 0x64 && window[1] == 0x61 &&
                           window[2] == 0x74 && window[3] == 0x61) {
                    dataSizeSkipped = 0;
                    wavState = WAV_SKIP_DATA_SIZE;
                  }
                  break;

                case WAV_READ_FMT_SIZE:
                  sizeBuf[sizeBytesRead++] = b;
                  if (sizeBytesRead == 4) {
                    fmtChunkSize = (uint32_t)sizeBuf[0] |
                                   ((uint32_t)sizeBuf[1] << 8) |
                                   ((uint32_t)sizeBuf[2] << 16) |
                                   ((uint32_t)sizeBuf[3] << 24);
                    fmtBytesRead = 0;
                    wavState = (fmtChunkSize > 0) ? WAV_READ_FMT_BODY : WAV_SCAN_SUBCHUNKS;
                  }
                  break;

                case WAV_READ_FMT_BODY:
                  if (fmtBytesRead < sizeof(fmtBody)) {
                    fmtBody[fmtBytesRead] = b;
                  }
                  fmtBytesRead++;
                  if (fmtBytesRead >= fmtChunkSize) {
                    if (fmtChunkSize >= 16) {
                      uint16_t numChannels = (uint16_t)fmtBody[2] | ((uint16_t)fmtBody[3] << 8);
                      uint32_t sampleRate  = (uint32_t)fmtBody[4] |
                                             ((uint32_t)fmtBody[5] << 8) |
                                             ((uint32_t)fmtBody[6] << 16) |
                                             ((uint32_t)fmtBody[7] << 24);
                      uint16_t bitsPerSample = (uint16_t)fmtBody[14] | ((uint16_t)fmtBody[15] << 8);
                      Serial.printf("[WAV fmt] Rate=%u Hz, Channels=%u, Bits=%u\n",
                                    sampleRate, numChannels, bitsPerSample);
                      if (sampleRate >= 8000 && sampleRate <= 48000 && bitsPerSample == 16) {
                        i2s_set_clk(I2S_SPEAKER_PORT,
                                    sampleRate,
                                    I2S_BITS_PER_SAMPLE_16BIT,
                                    (numChannels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
                      }
                    }
                    memset(window, 0, sizeof(window));
                    wavState = WAV_SCAN_SUBCHUNKS;
                  }
                  break;

                case WAV_SKIP_DATA_SIZE:
                  dataSizeSkipped++;
                  if (dataSizeSkipped >= 4) {
                    Serial.printf("[WAV data] Located PCM payload at byte offset %u\n",
                                  (unsigned int)(totalStreamBytes - (bytesRead - idx)));
                    wavState = WAV_STREAMING_PCM;
                  }
                  break;

                case WAV_STREAMING_PCM:
                  break;
              }
            }

            // Stream any remaining PCM bytes in this first chunk after "data" + size
            if (wavState == WAV_STREAMING_PCM && idx < bytesRead) {
              writeAlignedPCM(psramHttpBuf + idx, (size_t)(bytesRead - idx),
                              carryByte, hasCarry, amplification, totalPcmBytesWritten);
            }
          }
        }
      } else {
        // No bytes available right now; check idle timeout
        if (millis() - lastByteTime > HTTP_IDLE_TIMEOUT_MS) {
          Serial.println("[TTS] Stream idle timeout reached.");
          break;
        }
        delay(2);
      }
      yield();
    }

    unsigned long playTime = millis() - startTime;
    Serial.printf("[TTS] Playback complete: %u PCM bytes written (%u total HTTP bytes) in %lu ms\n",
                  (unsigned int)totalPcmBytesWritten, (unsigned int)totalStreamBytes, playTime);
  } else {
    Serial.printf("[TTS] ERROR: Groq TTS request failed, HTTP code: %d\n", httpCode);
    String errBody = http.getString();
    if (errBody.length() > 0) {
      Serial.println(errBody);
    }
  }

  http.end();
  tlsClient.stop();

  // Invariant 5: Zero speaker DMA buffer, restart & flush mic DMA, return to LISTENING
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
  delay(30);
  i2s_start(I2S_MIC_PORT);
  flushMicDMA();

  Serial.printf("[MEM After TTS] Internal Heap: %u bytes | PSRAM: %u bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ESP.getFreePsram());

  isSpeaking = false;
  currentState = STATE_LISTENING;
}

// =============================================================================
// GROQ LLM QUERY FUNCTION (INVARIANT 4: SINGLE-TLS SEQUENCING & PSRAM JSON)
// =============================================================================
void sendToGroq(String prompt) {
  prompt.trim();
  if (prompt.length() < 2) return;

  currentState = STATE_PROCESSING_LLM;

  // Invariant 4 Rule 2: Disconnect Deepgram WSS before opening Groq HTTPS session
  pauseDeepgramWSS();

  addToHistory("user", prompt);

  Serial.print("Sending to Groq: ");
  Serial.println(prompt);

  tlsClient.stop();
  tlsClient.setInsecure();

  HTTPClient http;
  http.begin(tlsClient, "https://api.groq.com/openai/v1/chat/completions");
  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  JsonDocument doc(&psramJsonAllocator);
  doc["model"] = "llama-3.3-70b-versatile";

  JsonArray messages = doc["messages"].to<JsonArray>();

  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;

  for (int i = 0; i < historyCount; i++) {
    JsonObject msg = messages.add<JsonObject>();
    msg["role"] = history[i].role;
    msg["content"] = history[i].content;
  }

  String requestBody;
  serializeJson(doc, requestBody);

  Serial.printf("[LLM] History=%d | Internal Heap=%u bytes | PSRAM=%u bytes\n",
                historyCount,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                ESP.getFreePsram());

  awaitingGroqResponse = true;
  int httpCode = http.POST(requestBody);
  String assistantReply = "";

  if (httpCode == HTTP_CODE_OK) {
    if (awaitingGroqResponse) {
      unsigned long elapsed = millis() - queryEndTime;
      Serial.printf("[End->First Groq Byte latency] %lu ms\n", elapsed);
      awaitingGroqResponse = false;
    }

    String payload = http.getString();
    JsonDocument responseDoc(&psramJsonAllocator);
    DeserializationError err = deserializeJson(responseDoc, payload);
    if (!err) {
      const char* response_text = responseDoc["choices"][0]["message"]["content"];
      if (response_text && strlen(response_text) > 0) {
        assistantReply = String(response_text);
        Serial.print("Groq Response: ");
        Serial.println(assistantReply);
        addToHistory("assistant", assistantReply);
      } else {
        Serial.println("[LLM] Warning: Empty content in Groq response.");
      }
    } else {
      Serial.printf("[LLM] JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[LLM] Groq request failed, HTTP code: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  tlsClient.stop();
  Serial.println("--------------------------\n");

  if (assistantReply.length() > 0) {
    playTTS(assistantReply);
  }

  if (pendingGoodbye) {
    Serial.println("Goodbye detected. Disconnecting WiFi and entering deep sleep...");
    delay(100);
    pauseDeepgramWSS();
    WiFi.disconnect(true);
    delay(200);
    esp_deep_sleep_start();
  }

  // Reconnect Deepgram WSS for next turn
  resumeDeepgramWSS();
  currentState = STATE_LISTENING;
}

// =============================================================================
// DEEPGRAM WEBSOCKET EVENT HANDLER
// =============================================================================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WSS] Deepgram WebSocket Disconnected.");
      break;

    case WStype_CONNECTED: {
      Serial.printf("[WSS] Deepgram WebSocket Connected: %s\n", (const char*)payload);
      Serial.println("[WSS] Sending StartRequest...");
      String startReq = buildStartRequest();
      webSocket.sendTXT(startReq);

      if (bootGreetingPending) {
        bootGreetingPending = false;
        queryEndTime = millis();
        pendingPrompt = "System boot complete. Please respond with a short greeting like 'I'm here.'";
        hasPendingPrompt = true;
      }
      break;
    }

    case WStype_TEXT: {
      JsonDocument doc(&psramJsonAllocator);
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) {
        Serial.printf("[WSS] deserializeJson() failed: %s\n", err.c_str());
        return;
      }

      const char* message_type = doc["type"];
      if (!message_type) return;

      if (strcmp(message_type, "Results") == 0) {
        const char* transcript = doc["channel"]["alternatives"][0]["transcript"];
        bool is_final = doc["is_final"].as<bool>();

        if (transcript && strlen(transcript) > 0) {
          String current = String(transcript);
          if (is_final) {
            chunkCount++;
            committedTranscript += current + " ";
            interimTranscript = "";
            if (DEBUG_FINALS) {
              Serial.printf("[chunk=%d] [is_final=TRUE] committed: \"%s\"\n", chunkCount, transcript);
            }
          } else {
            interimTranscript = current;
            if (DEBUG_FINALS) {
              Serial.printf("[chunk=%d] [is_final=FALSE] interim: \"%s\"\n", chunkCount, transcript);
            }
          }
        }
      } else if (strcmp(message_type, "UtteranceEnd") == 0) {
        Serial.printf("[%lu ms] [UtteranceEnd]\n", millis());

        String final_transcript = committedTranscript;
        if (interimTranscript.length() > 0) {
          final_transcript += interimTranscript;
        }
        final_transcript.trim();

        Serial.print("Final transcript: ");
        Serial.println(final_transcript);

        if (final_transcript.length() > 0) {
          Serial.println("[Queueing speech for Groq]");
          Serial.println("--------------------------");
          queryEndTime = millis();
          chunkCount = 1;

          String normalized = final_transcript;
          normalized.toLowerCase();
          normalized.trim();
          if (normalized.startsWith("goodbye")) {
            pendingGoodbye = true;
          }

          pendingPrompt = final_transcript;
          hasPendingPrompt = true;
        }

        committedTranscript = "";
        interimTranscript = "";
      }
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WSS] WebSocket Error: %s\n", (const char*)payload);
      break;

    default:
      break;
  }
}

// =============================================================================
// I2S MICROPHONE & SPEAKER INITIALIZATION
// =============================================================================
void configureI2S() {
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_SIZE,
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
  err = i2s_set_pin(I2S_MIC_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S MIC] Pin config failed: %d\n", err);
  }
}

void configureI2S_TX() {
  i2s_config_t tx_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = DEFAULT_TTS_RATE,
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

  err = i2s_set_pin(I2S_SPEAKER_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S SPK] Pin config failed: %d\n", err);
    return;
  }

  err = i2s_set_clk(I2S_SPEAKER_PORT, DEFAULT_TTS_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  if (err != ESP_OK) {
    Serial.printf("[I2S SPK] Clock config failed: %d\n", err);
  }
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
  Serial.println("[I2S SPK] Speaker I2S Configured.");
}

void playTestTone() {
  Serial.println("[TEST] Playing 440Hz test tone for 500ms...");
  const int duration = 500;
  const int sampleRate = DEFAULT_TTS_RATE;
  const float frequency = 440.0f;

  int16_t buffer[256];
  size_t bytes_written = 0;
  unsigned long start = millis();

  while (millis() - start < duration) {
    for (int i = 0; i < 256; i++) {
      float time = (float)(millis() - start + (i * 1000.0f / sampleRate)) / 1000.0f;
      buffer[i] = (int16_t)(sinf(2.0f * 3.14159265f * frequency * time) * 6000.0f);
    }
    i2s_write(I2S_SPEAKER_PORT, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
  }

  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
  Serial.println("[TEST] Test tone finished.");
}

// =============================================================================
// SETUP & MAIN LOOP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Real-Time STT->LLM->TTS Bot (Track A Hardened) ---");

  if (!initPSRAMBuffers()) {
    Serial.println("FATAL: Failed to allocate audio/HTTP buffers! Halting.");
    while (true) { delay(1000); }
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi...");
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 30) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed! Restarting...");
    ESP.restart();
  }
  Serial.println(" Connected!");
  Serial.printf("IP: %s, RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  configureI2S();
  Serial.println("I2S Microphone Configured.");

  configureI2S_TX();
  playTestTone();
  flushMicDMA();

  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(30000, 3000, 2);

  currentState = STATE_LISTENING;
  resumeDeepgramWSS();
}

void loop() {
  if (wsActive) {
    webSocket.loop();
  }

  // Process queued prompt outside WebSocket event callback (single-TLS sequencing)
  if (hasPendingPrompt) {
    String promptToRun = pendingPrompt;
    pendingPrompt = "";
    hasPendingPrompt = false;
    sendToGroq(promptToRun);
    return;
  }

  // Invariant 5: Only capture mic audio when in LISTENING state and not speaking
  if (currentState == STATE_LISTENING && !isSpeaking && wsActive && webSocket.isConnected()) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_MIC_PORT,
                             (void*)psramMicRaw32,
                             BUFFER_SIZE * sizeof(int32_t),
                             &bytes_read,
                             portMAX_DELAY);

    if (err == ESP_OK && bytes_read > 0) {
      size_t samples_read = bytes_read / sizeof(int32_t);
      long sum = 0;
      for (size_t i = 0; i < samples_read; i++) {
        int16_t sample = (int16_t)(psramMicRaw32[i] >> 11);
        psramMicPcm16[i] = sample;
        sum += abs(sample);
      }

      static unsigned long lastAudioLog = 0;
      if (millis() - lastAudioLog > 2000 && samples_read > 0) {
        float avgLevel = (float)sum / (float)samples_read;
        Serial.printf("[MIC] Audio level: %.1f (0=quiet, 100+=speaking)\n", avgLevel);
        lastAudioLog = millis();
      }

      webSocket.sendBIN((uint8_t*)psramMicPcm16, samples_read * sizeof(int16_t));
    }
  } else {
    delay(2);
  }
}
