#include "config1.h"
#include <WiFi.h>
#include <esp_sleep.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// --- Pin Assignments & Configs ---
// INMP441 MIC
#define I2S_MIC_SCK  6
#define I2S_MIC_WS   5
#define I2S_MIC_SD   4
#define I2S_MIC_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512

// Amplifier pin definitions
// --- Pin Assignments for Speaker (xiaogpt pins on I2S Port 1) ---
#define I2S_SPEAKER_PORT I2S_NUM_1  // <-- Using Port 1 to avoid conflict with mic
#define I2S_SPEAKER_SCK  8          // Serial Clock (BCK / BCLK)
#define I2S_SPEAKER_WS   7          // Word Select (LRC / LRCK)
#define I2S_SPEAKER_SD   9          // Serial Data (DIN)

// --- Globals ---
WebSocketsClient webSocket;
String interimTranscript = "";       // most recent partial
String committedTranscript = "";
String interimBuffer = "";
bool DEBUG_FINALS = true;  // Toggle debug printing for is_final chunks
unsigned long queryEndTime = 0;
bool awaitingGroqResponse = false;
int chunkCount = 1;
bool isSpeaking = false; // Flag to prevent mic from listening to the speaker

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
//  "You are working over an audio link, so your responses are optimised for being heard rather than read. "
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
    // Play audio of Groq's response
    playTTS(String(response_text));

  } else {
    Serial.printf("Groq request failed, HTTP code: %d\n", httpCode);
    Serial.println(http.getString());
  }

  http.end();
  Serial.println("--------------------------\n");
}



// --- TTS Function ---
// --- Replace your playTTS function with this new version ---

// --- Replace your playTTS function with this version to fix the crash ---

void playTTS(String text) {
  isSpeaking = true;
  Serial.println("Starting Groq TTS playback with amplification...");

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
    byte header[44];
    stream->readBytes(header, 44);
    
    // ----> THE FIX: Declare the buffer as 'static' to prevent stack overflow <----
    static uint8_t buffer[BUFFER_SIZE * 2];
    
    size_t bytes_written = 0;
    const float amplification = 10.0f;

    while (http.connected() && (stream->available() > 0)) {
      int bytesRead = stream->readBytes(buffer, sizeof(buffer));
      if (bytesRead > 0) {
        int16_t* samples = (int16_t*)buffer;
        int sampleCount = bytesRead / 2;
        
        for (int i = 0; i < sampleCount; i++) {
          float amplifiedSample = (float)samples[i] * amplification;
          if (amplifiedSample > 32767.0f) amplifiedSample = 32767.0f;
          if (amplifiedSample < -32768.0f) amplifiedSample = -32768.0f;
          samples[i] = (int16_t)amplifiedSample;
        }
        
        i2s_write(I2S_SPEAKER_PORT, buffer, bytesRead, &bytes_written, portMAX_DELAY);
      }
    }
  } else {
    Serial.printf("Groq TTS request failed, HTTP code: %d\n", httpCode);
  }

  http.end();
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
  Serial.println("Groq TTS playback finished.");
  isSpeaking = false;
}
/*
// --- Revert to this simpler playTTS function ---
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

    uint8_t buffer[BUFFER_SIZE];
    size_t bytes_written = 0;
    while (http.connected() && (stream->available() > 0)) {
      int len = stream->readBytes(buffer, sizeof(buffer));
      if (len > 0) {
        i2s_write(I2S_SPEAKER_PORT, buffer, len, &bytes_written, portMAX_DELAY);
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
*/
// --- Replace your playTTS function with this diagnostic version ---
/*
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
    // ----> NEW: Check the actual content type of the response <----
    String contentType = http.header("Content-Type");
    Serial.printf("[HTTP Response] Content-Type: %s\n", contentType.c_str());

    if (contentType == "audio/wav") {
      Serial.println("Response is WAV audio. Proceeding to play...");
      WiFiClient* stream = http.getStreamPtr();
      byte header[44];
      stream->readBytes(header, 44);

      // (The rest of the header parsing and i2s_write loop remains the same)
      uint16_t numChannels = header[22] | (header[23] << 8);
      uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
      uint16_t bitsPerSample = header[34] | (header[35] << 8);

      Serial.println("----------- WAV Header Info -----------");
      Serial.printf("  - Channels: %u\n", numChannels);
      Serial.printf("  - Sample Rate: %u Hz\n", sampleRate);
      Serial.printf("  - Bits Per Sample: %u\n", bitsPerSample);
      Serial.println("---------------------------------------");

      uint8_t buffer[BUFFER_SIZE];
      size_t bytes_written = 0;
      while (http.connected() && (stream->available() > 0)) {
        int len = stream->readBytes(buffer, sizeof(buffer));
        if (len > 0) {
          i2s_write(I2S_SPEAKER_PORT, buffer, len, &bytes_written, portMAX_DELAY);
        }
      }
    } else {
      // ----> NEW: If not audio, print the response as text to find the error <----
      Serial.println("!!! ERROR: Expected 'audio/wav' but received something else.");
      String responseBody = http.getString();
      Serial.println("----------- Groq Error Response -----------");
      Serial.println(responseBody);
      Serial.println("-----------------------------------------");
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
*/
/*
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

    // ----> NEW: Read and inspect the 44-byte WAV header <----
    byte header[44];
    stream->readBytes(header, 44);

    // Parse key information from the header (assumes little-endian format)
    uint16_t numChannels = header[22] | (header[23] << 8);
    uint32_t sampleRate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    uint16_t bitsPerSample = header[34] | (header[35] << 8);

    Serial.println("----------- WAV Header Info -----------");
    Serial.printf("  - Channels: %u\n", numChannels);
    Serial.printf("  - Sample Rate: %u Hz\n", sampleRate);
    Serial.printf("  - Bits Per Sample: %u\n", bitsPerSample);
    Serial.println("---------------------------------------");
    
    // Check if the received format matches our I2S config
    if (sampleRate != 24000 || bitsPerSample != 16) {
      Serial.println("!!! WARNING: Audio format does NOT match I2S configuration !!!");
    } else {
      Serial.println("Audio format matches I2S configuration. Proceeding to play...");
    }

    uint8_t buffer[BUFFER_SIZE];
    size_t bytes_written = 0;
    while (http.connected() && (stream->available() > 0)) {
      int len = stream->readBytes(buffer, sizeof(buffer));
      if (len > 0) {
        i2s_write(I2S_SPEAKER_PORT, buffer, len, &bytes_written, portMAX_DELAY);
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
*/
/*
void playTTS(String text) {
  isSpeaking = true; // Signal that the assistant is talking
  Serial.println("Starting Groq TTS playback...");

  HTTPClient http;
  http.begin("https://api.groq.com/openai/v1/audio/speech"); 
  http.addHeader("Authorization", "Bearer " + String(GROQ_API_KEY));
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(1024);
  doc["model"] = "playai-tts";
  doc["input"] = text;
  doc["voice"] = "Atlas-PlayAI";
  // ----> CHANGE #1: Request 'wav' instead of 'pcm' <----
  doc["response_format"] = "wav";

  String requestBody;
  serializeJson(doc, requestBody);

  int httpCode = http.POST(requestBody);

  if (httpCode == 200) {
    WiFiClient* stream = http.getStreamPtr();

    // ----> CHANGE #2: Add code to skip the 44-byte WAV header <----
    // A standard WAV file header is 44 bytes long. We need to read and
    // discard it before we can start playing the raw audio data.
    byte header[44];
    stream->readBytes(header, 44);

    uint8_t buffer[BUFFER_SIZE];
    size_t bytes_written = 0;

    // The rest of the streaming logic is the same
    while (http.connected() && (stream->available() > 0)) {
      int len = stream->readBytes(buffer, sizeof(buffer));
      if (len > 0) {
        i2s_write(I2S_SPEAKER_PORT, buffer, len, &bytes_written, portMAX_DELAY);
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
*/

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

// --- I2S mic configuration function ---
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

  i2s_pin_config_t pin_config = { .bck_io_num = I2S_MIC_SCK, .ws_io_num = I2S_MIC_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_MIC_SD };
  i2s_driver_install(I2S_MIC_PORT, &mic_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pin_config);
}

// --- I2S speaker configutation function ---
// --- Replace your configureI2S_TX function with this final version ---

void configureI2S_TX() {
  // This configuration is now aligned with the working xiaogpt project
  i2s_config_t tx_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 24000, // Our TTS audio is 24000 Hz
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB, // Note: The bitwise OR is not needed
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = false, // Match xiaogpt by setting use_apll to false (0)
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  // Uninstall any previous driver
  i2s_driver_uninstall(I2S_SPEAKER_PORT);
  
  // Install the new configuration
  i2s_driver_install(I2S_SPEAKER_PORT, &tx_config, 0, NULL);

  // Set the I2S pins
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPEAKER_SCK,
    .ws_io_num = I2S_SPEAKER_WS,
    .data_out_num = I2S_SPEAKER_SD,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_set_pin(I2S_SPEAKER_PORT, &pin_config);
  
  // ----> The crucial missing line! <----
  // Manually set the clock after installation
  i2s_set_clk(I2S_SPEAKER_PORT, 24000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  
  Serial.println("Speaker I2S Configured (Final Version).");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- Real-Time STT->LLM->TTS Bot ---");

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  configureI2S();
  Serial.println("I2S Microphone Configured.");
  configureI2S_TX();
  Serial.println("I2S Speaker Configured.");


  Serial.printf("[Heap before WebSocket] %d bytes\n", ESP.getFreeHeap());

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
