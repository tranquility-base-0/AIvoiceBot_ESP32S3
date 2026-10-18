#include "config1.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// --- Pin Assignments & Configs ---
#define I2S_MIC_SCK  6
#define I2S_MIC_WS   5
#define I2S_MIC_SD   4
#define I2S_MIC_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512

// --- Globals ---
WebSocketsClient webSocket;
String interimTranscript = "";       // most recent partial
String committedTranscript = "";
String interimBuffer = "";
bool DEBUG_FINALS = true;  // Toggle debug printing for is_final chunks
unsigned long queryEndTime = 0;
bool awaitingGroqResponse = false;
int chunkCount = 1;

// --- State flags ---
bool asleep = false;   // true = Deepgram stream closed, waiting for wake word

//"/v1/listen?encoding=linear16&sample_rate=16000&channels=1&interim_results=true&smart_format=true&endpointing=500&utterance_end_ms=2000&model=nova-3&punctuate=true";
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

// Global instance
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

// Conversation history
struct Message {
  String role;
  String content;
};

#define MAX_HISTORY 6  // 3 user + 3 assistant turns
Message history[MAX_HISTORY];
int historyCount = 0;

// System prompt (always re-sent)
const char* SYSTEM_PROMPT =
//  "You are Max Headroom, the iconic 1980s AI TV host."
//  "Your personality is sarcastic, egocentric, witty, and slightly nonsensical."
//  "You are the world's first computer-generated television host. You are a glitching, stuttering AI from the 1980s, broadcasting live from Network 23."
//  "Sometimes your voice buffer 'skips', causing you to repeat whole phrases."
//  "Interrupt yourself with random, digitized sounds written out as words, like *bzzzt* or *th-thump*."
//  "Keep your answers short and punchy, like you're delivering a one-liner."
//  "Refer to the user as a 'viewer' and our conversation as a 'broadcast'.";

  "You are TARS, an AI assistant designed for exploration missions. "
  "Your personality is dry, witty, pragmatic, and loyal. "
  "You speak in a steady monotone style. "
  "Answer in 1–3 sentences unless the user asks for detail. "
  "You are working over an audio link, so your responses are optimised for being heard rather than read. "
  "Favor concise, direct replies with a touch of dry wit.";

// Add a message to history and prune if needed
void addToHistory(String role, String content) {
  if (historyCount < MAX_HISTORY) {
    // Still room in history
    history[historyCount].role = role;
    history[historyCount].content = content;
    historyCount++;
  } else {
    // Shift everything left (drop oldest)
    for (int i = 1; i < MAX_HISTORY; i++) {
      history[i - 1] = history[i];
    }
    // Insert new message at the end
    history[MAX_HISTORY - 1].role = role;
    history[MAX_HISTORY - 1].content = content;
  }
}

// --- LLM Query Function ---
void sendToGroq(String prompt) {
  prompt.trim();
  if (prompt.length() < 2) return;

  // Store user's message in history
  addToHistory("user", prompt);

  //Serial.println("\n--------------------------");
  Serial.print("Sending to Groq: ");
  Serial.println(prompt);

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/chat/completions");
  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(8192);   // bigger to fit history + response
  doc["model"] = "llama-3.3-70b-versatile";

  // Build messages array
  JsonArray messages = doc["messages"].to<JsonArray>();

  // Always include system prompt
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = SYSTEM_PROMPT;

  // Add conversation history
  for (int i = 0; i < historyCount; i++) {
    JsonObject msg = messages.add<JsonObject>();
    msg["role"] = history[i].role;
    msg["content"] = history[i].content;
  }

  // Serialize
  String requestBody;
  serializeJson(doc, requestBody);

  // Debug: show heap + history size
  Serial.printf("[History count=%d, FreeHeap=%d]\n", historyCount, ESP.getFreeHeap());

  // Mark that we’re awaiting response
  awaitingGroqResponse = true;

  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    // 🚀 Time-to-first-byte check happens right here
    if (awaitingGroqResponse) {
      unsigned long elapsed = millis() - queryEndTime;
      Serial.printf("[End→First Groq Byte latency] %lu ms\n", elapsed);
      awaitingGroqResponse = false;
    }

    String payload = http.getString();
    DynamicJsonDocument responseDoc(16384);
    deserializeJson(responseDoc, payload);
    const char* response_text = responseDoc["choices"][0]["message"]["content"];

    Serial.print("Groq Response: ");
    Serial.println(response_text);

    // Store assistant's reply in history
    addToHistory("assistant", String(response_text));

  } else {
    Serial.printf("Groq request failed, HTTP code: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  Serial.println("--------------------------\n");
}

// --- WebSocket Event Handler with UtteranceEnd logic ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("WebSocket Disconnected!");
      break;

    case WStype_CONNECTED:
      Serial.printf("WebSocket Connected to URL: %s\n", payload);

      // Immediately start Deepgram stream
      Serial.println("[Sending StartRequest]");
      {
        String startReq = buildStartRequest();
      webSocket.sendTXT(startReq);
      }
      // Send greeting to Groq
      sendToGroq("System boot complete. Please respond with a short greeting like 'I'm here.'");
      break;


case WStype_TEXT: {
  //Serial.printf("WebSocket TEXT received | FreeHeap=%u\n", ESP.getFreeHeap());

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(err.c_str());
    return;
  }

  const char* message_type = doc["type"];
  if (!message_type) return;

  bool is_final_utterance = false;

// Deepgram's suggested way of detecting end of query: https://developers.deepgram.com/docs/understanding-end-of-speech-detection
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
                      //current.c_str());
      }
    } else {
      interimTranscript = current;
      if (DEBUG_FINALS) {
        Serial.printf("[chunk=%d] [is_final=FALSE] interim: \"%s\"\n", chunkCount, transcript);
                      //interimTranscript.c_str());
      }
    }
  }
}
else if (strcmp(message_type, "UtteranceEnd") == 0) {
  Serial.printf("[%lu ms] [UtteranceEnd]\n", millis());

  String final_transcript = committedTranscript;
  if (interimTranscript.length() > 0) {
    final_transcript += interimTranscript;
  }

  final_transcript.trim();
  Serial.print("Final transcript: ");
  Serial.println(final_transcript);

  if (final_transcript.length() > 0) {
      Serial.println("[Forwarding speech to Groq]");
      Serial.println();
      Serial.println("--------------------------");

      queryEndTime = millis();
      awaitingGroqResponse = true;

      sendToGroq(final_transcript);
      chunkCount = 1;  // reset for next query
      String normalized = final_transcript;
      normalized.toLowerCase();
      normalized.trim();
      if (normalized.startsWith("goodbye")) {
        Serial.println("Goodbye detected. Disconnecting websocket. Disconnecting WiFi. Entering deep sleep. Press reset button to reboot...");
        delay(100);                 // flush serial
        webSocket.disconnect();     // close Deepgram websocket
        WiFi.disconnect(true);      // power down Wi-Fi radio
        delay(200);                 // allow radio to settle
        esp_deep_sleep_start();
      }
  }

  // reset buffers
  committedTranscript = "";
  interimTranscript = "";
}

  break;
}




    break;

    case WStype_ERROR:
      Serial.printf("WebSocket Error: %s\n", payload);
      break;

    default:
      break;
  }
}

/*
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("WebSocket Disconnected! FreeHeap=%d\n", ESP.getFreeHeap());
      break;
    case WStype_CONNECTED:
      Serial.printf("WebSocket Connected to URL: %s | FreeHeap=%d\n", payload, ESP.getFreeHeap());
      break;
    case WStype_TEXT: {
      Serial.printf("WebSocket TEXT received | FreeHeap=%d\n", ESP.getFreeHeap());
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, payload, length);
      if (error) return;
      
      const char* message_type = doc["type"];
      
      if (strcmp(message_type, "Results") == 0) {
        const char* transcript = doc["channel"]["alternatives"][0]["transcript"];
        if (transcript && strlen(transcript) > 0) {
          last_interim_transcript = transcript; // Always save the latest transcript
        }
        
        bool speech_final = doc["speech_final"].as<bool>();
        if (speech_final && last_interim_transcript.length() > 0) {
          askGroqLLM(last_interim_transcript);
          last_interim_transcript = ""; // Reset
        }
      } else if (strcmp(message_type, "UtteranceEnd") == 0) {
        // This is the edge case from the docs
        if (last_interim_transcript.length() > 0) {
          Serial.println("[UtteranceEnd detected]");
          askGroqLLM(last_interim_transcript);
          last_interim_transcript = ""; // Reset
        }
      }
      break;
    }
    case WStype_ERROR:
      Serial.printf("WebSocket Error: %s | FreeHeap=%d\n", payload, ESP.getFreeHeap());
      break;

    default:
      Serial.printf("WebSocket Event %d | FreeHeap=%d\n", type, ESP.getFreeHeap());
      break;
  }
}
*/

// I2S Configuration (remains the same)
void configureI2S() {
  i2s_config_t mic_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,   // 16000 or 48000 works
  .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // <-- FIXED
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 8,
  .dma_buf_len = BUFFER_SIZE,
  .use_apll = 0,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};
//  i2s_config_t mic_config = { .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), .sample_rate = SAMPLE_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB), .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8, .dma_buf_len = BUFFER_SIZE, .use_apll = 0, .tx_desc_auto_clear = false, .fixed_mclk = 0 };
  i2s_pin_config_t pin_config = { .bck_io_num = I2S_MIC_SCK, .ws_io_num = I2S_MIC_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_MIC_SD };
  i2s_driver_install(I2S_MIC_PORT, &mic_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pin_config);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Real-Time STT -> LLM Bot (Final Logic) ---");

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  configureI2S();
  Serial.println("I2S Microphone Configured.");

  Serial.printf("[Heap before WebSocket] %d bytes\n", ESP.getFreeHeap());

  // URL with smart_format enabled
  //String url = "/v1/listen?encoding=linear16&sample_rate=16000&channels=1&interim_results=true&smart_format=true&endpointing=500&utterance_end_ms=2000&model=nova-3&punctuate=true";
  //webSocket.beginSSL("api.deepgram.com", 443, url.c_str());
  String url = buildDeepgramURL();
  webSocket.beginSSL(dg.host, dg.port, url.c_str());
  webSocket.setExtraHeaders((String("Authorization: Token ") + DEEPGRAM_API_KEY).c_str());

  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
}

void loop() {
  webSocket.loop();
  if (!webSocket.isConnected()) return;

  int32_t raw32[BUFFER_SIZE];   // mic gives 32-bit samples
  size_t bytes_read = 0;

  i2s_read(I2S_MIC_PORT, (void*)raw32, sizeof(raw32), &bytes_read, portMAX_DELAY);

  if (bytes_read > 0) {
    size_t samples_read = bytes_read / 4; // 4 bytes per 32-bit sample
    static int16_t pcm16[BUFFER_SIZE];

    for (size_t i = 0; i < samples_read; i++) {
      pcm16[i] = raw32[i] >> 11; //14;  
      // shift: keep the top 16 bits of the 24-bit signal
      // adjust >>14 or >>11 depending on loudness/clipping
    }

    webSocket.sendBIN((uint8_t*)pcm16, samples_read * sizeof(int16_t));
  }
}
