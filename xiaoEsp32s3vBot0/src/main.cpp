#include <Arduino.h>
#include <config1.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// =============================================================================
// PIN ASSIGNMENTS & HARDWARE CONFIG
// =============================================================================

// INMP441 Microphone (I2S Port 0)
#define I2S_MIC_SCK     6
#define I2S_MIC_WS      5
#define I2S_MIC_SD      4
#define I2S_MIC_PORT    I2S_NUM_0
#define SAMPLE_RATE     16000
#define BUFFER_SIZE     512

// Amplifier pin definitions (I2S Port 1)
#define I2S_SPEAKER_PORT I2S_NUM_1
#define I2S_SPEAKER_SCK  8
#define I2S_SPEAKER_WS   7
#define I2S_SPEAKER_SD   9

// =============================================================================
// GLOBAL VARIABLES & BUFFERS (Optimized for Heap/Stack)
// =============================================================================

WebSocketsClient webSocket;
String interimTranscript = "";
String committedTranscript = "";
bool DEBUG_FINALS = true;
unsigned long queryEndTime = 0;
bool awaitingGroqResponse = false;
int chunkCount = 1;
bool isSpeaking = false;

// Global audio buffers (moved off the stack for stability)
int32_t raw32[BUFFER_SIZE]; // 32-bit I2S read buffer
int16_t pcm16[BUFFER_SIZE]; // 16-bit PCM buffer for Deepgram

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
// CONVERSATION HISTORY & TARS PROMPT
// =============================================================================

struct Message {
  String role;
  String content;
};

#define MAX_HISTORY 6
Message history[MAX_HISTORY];
int historyCount = 0;

const char* SYSTEM_PROMPT =
  "You are TARS, an AI assistant designed for exploration missions. "
  "Your personality is dry, witty, pragmatic, and loyal. "
  "You speak in a steady monotone style. "
  "Answer in 1–3 sentences unless the user asks for detail. "
  "You are working over an audio link, so your responses are optimised for being heard rather than read. "
  "Favor concise, direct replies with a touch of dry wit.";

void addToHistory(String role, String content) {
  if (historyCount < MAX_HISTORY) {
    history[historyCount].role = role;
    history[historyCount].content = content;
    historyCount++;
  } else {
    // Shift left to drop oldest message
    for (int i = 1; i < MAX_HISTORY; i++) {
      history[i - 1] = history[i];
    }
    history[MAX_HISTORY - 1].role = role;
    history[MAX_HISTORY - 1].content = content;
  }
}

// =============================================================================
// TTS FUNCTION (Simplified for now)
// =============================================================================

// Forward declaration
void playTTS(String text);

void playTTS(String text) {
  isSpeaking = true;
  Serial.println("Starting Groq TTS playback...");

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/audio/speech");
  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(1024);
  doc["model"] = "playai-tts";
  doc["input"] = text;
  doc["voice"] = "Atlas-PlayAI";
  doc["response_format"] = "wav";

  String requestBody;
  serializeJson(doc, requestBody);
  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    WiFiClient* stream = http.getStreamPtr();

    // Skip the 44-byte WAV header
    byte header[44];
    stream->readBytes(header, 44);

    // Buffer for TTS data (static or global is better for large HTTP streaming)
    static uint8_t tts_buffer[BUFFER_SIZE];
    size_t bytes_written = 0;

    while (http.connected() && (stream->available() > 0)) {
      int len = stream->readBytes(tts_buffer, sizeof(tts_buffer));
      if (len > 0) {
        i2s_write(I2S_SPEAKER_PORT, tts_buffer, len, &bytes_written, portMAX_DELAY);
      }
    }
  } else {
    Serial.printf("Groq TTS request failed, HTTP code: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
  Serial.println("Groq TTS playback finished.");
  isSpeaking = false;
}

// =============================================================================
// GROQ LLM
// =============================================================================

void sendToGroq(String prompt) {
  prompt.trim();
  if (prompt.length() < 2) return;

  addToHistory("user", prompt);

  Serial.print("\n--------------------------\nSending to Groq: ");
  Serial.println(prompt);

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/chat/completions");
  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(8192); // Big enough for history + request
  doc["model"] = "llama-3.3-70b-versatile";
  doc["temperature"] = 0.75;
  doc["max_tokens"] = 100;

  JsonArray messages = doc.createNestedArray("messages");

  // System prompt
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;

  // Conversation history
  for (int i = 0; i < historyCount; i++) {
    JsonObject msg = messages.createNestedObject();
    msg["role"] = history[i].role;
    msg["content"] = history[i].content;
  }

  String requestBody;
  serializeJson(doc, requestBody);

  Serial.printf("[History count=%d, FreeHeap=%d]\n", historyCount, ESP.getFreeHeap());

  awaitingGroqResponse = true;
  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    if (awaitingGroqResponse) {
      unsigned long elapsed = millis() - queryEndTime;
      Serial.printf("[End→First Groq Byte latency] %lu ms\n", elapsed);
      awaitingGroqResponse = false;
    }

    String payload = http.getString();
    DynamicJsonDocument responseDoc(16384); // Larger for the entire response
    deserializeJson(responseDoc, payload);

    const char* response_text = responseDoc["choices"][0]["message"]["content"];

    Serial.print("Groq Response: ");
    Serial.println(response_text);

    addToHistory("assistant", String(response_text));

    // Play audio of Groq's response
    playTTS(String(response_text));

  } else {
    Serial.printf("❌ Groq request failed, HTTP code: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  Serial.println("--------------------------\n");
}

// =============================================================================
// WEBSOCKET HANDLING
// =============================================================================

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("❌ WebSocket Disconnected!");
      break;

    case WStype_CONNECTED: {
      Serial.printf("✅ WebSocket Connected to: %s\n", payload);

      // Start Deepgram stream
      String startReq = buildStartRequest();
      webSocket.sendTXT(startReq);

      // Send greeting to Groq (This is the heavy operation that requires 8192 stack)
      sendToGroq("System boot complete. Please respond with a short greeting like 'I'm here.'");

      Serial.println("🎙️ Listening...");
      break;
    }

    case WStype_TEXT: {
      StaticJsonDocument<4096> doc;
      DeserializationError err = deserializeJson(doc, payload, length);

      if (err) {
        Serial.print("JSON parse error: ");
        Serial.println(err.c_str());
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
            // Show interim results
            interimTranscript = current;
             if (DEBUG_FINALS) {
                Serial.printf("[chunk=%d] [is_final=FALSE] interim: \"%s\"\n", chunkCount, transcript);
            }
          }
        }
      }
      else if (strcmp(message_type, "UtteranceEnd") == 0) {
        Serial.printf("[%lu ms] [UtteranceEnd]\n", millis());

        // Compile final transcript
        String final_transcript = committedTranscript;
        if (interimTranscript.length() > 0) {
          final_transcript += interimTranscript;
        }
        final_transcript.trim();

        if (final_transcript.length() > 0) {
          // Check for goodbye
          String normalized = final_transcript;
          normalized.toLowerCase();

          if (normalized.startsWith("goodbye") || normalized.startsWith("bye")) {
            Serial.println("\n👋 Goodbye detected. Entering deep sleep...");
            delay(100);
            webSocket.disconnect();
            WiFi.disconnect(true);
            delay(200);
            esp_deep_sleep_start();
          }

          // Send to Groq
          Serial.println("[Forwarding speech to Groq]");
          queryEndTime = millis();
          awaitingGroqResponse = true;
          sendToGroq(final_transcript);
          chunkCount = 1;
        }

        // Reset buffers
        committedTranscript = "";
        interimTranscript = "";
      }
      break;
    }

    case WStype_ERROR:
      Serial.printf("⚠️ WebSocket Error: %s\n", payload);
      break;

    default:
      break;
  }
}

// =============================================================================
// I2S MICROPHONE SETUP
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

  i2s_driver_install(I2S_MIC_PORT, &mic_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pin_config);
}

// =============================================================================
// I2S SPEAKER SETUP
// =============================================================================

void configureI2S_TX() {
  i2s_config_t tx_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000, // TTS audio is 24000 Hz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_driver_uninstall(I2S_SPEAKER_PORT);
  i2s_driver_install(I2S_SPEAKER_PORT, &tx_config, 0, NULL);

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPEAKER_SCK,
    .ws_io_num = I2S_SPEAKER_WS,
    .data_out_num = I2S_SPEAKER_SD,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_set_pin(I2S_SPEAKER_PORT, &pin_config);

  // Set the clock after installation
  i2s_set_clk(I2S_SPEAKER_PORT, 24000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  Serial.println("Speaker I2S Configured.");
}

// =============================================================================
// SETUP & LOOP
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n=== TARS Voice Assistant (STT + LLM) ===\n");

  // Connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  int wifi_retry = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retry < 20) {
    delay(500);
    Serial.print(".");
    wifi_retry++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n❌ WiFi connection failed!");
    Serial.println("Restarting...");
    ESP.restart();
  }

  Serial.println(" Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Signal: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  // Setup I2S microphone and speaker
  configureI2S();
  Serial.println("✅ I2S Microphone configured");
  configureI2S_TX();
  Serial.println("✅ I2S Speaker configured");

  Serial.printf("[Free Heap: %d bytes]\n\n", ESP.getFreeHeap());

  // Connect to Deepgram WebSocket
  String url = buildDeepgramURL();

  // *** CRITICAL FIX: Increase WebSocket task stack size ***
  webSocket.setStackSize(8192);

  webSocket.beginSSL(dg.host, dg.port, url.c_str());
  webSocket.setExtraHeaders((String("Authorization: Token ") + DEEPGRAM_API_KEY).c_str());
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  webSocket.loop();

  if (!webSocket.isConnected()) return;

  // Read microphone data using global buffers (raw32, pcm16)
  size_t bytes_read = 0;

  i2s_read(I2S_MIC_PORT, (void*)raw32, sizeof(raw32), &bytes_read, portMAX_DELAY);

  if (bytes_read > 0) {
    size_t samples_read = bytes_read / 4;

    // Convert 32-bit to 16-bit PCM
    for (size_t i = 0; i < samples_read; i++) {
      pcm16[i] = raw32[i] >> 11;  // Adjust shift for volume (11-14)
    }

    // Send audio to Deepgram
    webSocket.sendBIN((uint8_t*)pcm16, samples_read * sizeof(int16_t));
  }
}