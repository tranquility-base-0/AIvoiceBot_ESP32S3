#include <WiFi.h>
#include <HTTPClient.h>      // <-- Use the high-level client
#include <SPIFFS.h>
#include <ESP8266Audio.h>
#include <AudioOutputI2S.h>
#include <AudioGeneratorMP3.h>

// Your WiFi and API Key credentials
#include "config.h"

// ==== Deepgram ====
const char *DG_APIKEY = DEEPGRAM_API_KEY;

// ==== I2S Pinout (Corrected for XIAOGPT) ====
#define I2S_SPEAKER_BCLK GPIO_NUM_8  // Bit clock (BCLK)
#define I2S_SPEAKER_LRC  GPIO_NUM_7  // Left/right clock (LRCK or WS)
#define I2S_SPEAKER_DIN  GPIO_NUM_9  // Data out (DIN)
//#define I2S_SPEAKER_BCLK 4
//#define I2S_SPEAKER_LRC  3
//#define I2S_SPEAKER_DIN  2

// ==== Globals ====
HTTPClient http;
AudioGeneratorMP3 *mp3;
AudioOutputI2S *out;

// Custom source to wrap a generic WiFiClient stream
class AudioFileSourceStream : public AudioFileSource {
public:
  AudioFileSourceStream(WiFiClient *stream) : stream(stream) {}
  virtual ~AudioFileSourceStream() { if (stream) stream->stop(); }
  virtual bool open(const char *filename) { return true; }
  virtual uint32_t read(void *data, uint32_t len) {
    if (!stream || !stream->connected()) return 0;
    return stream->read((uint8_t*)data, len);
  }
  virtual bool seek(int32_t pos, int dir) { return false; }
  virtual bool close() { if (stream) stream->stop(); return true; }
  virtual uint32_t getSize() { return 0; }
  virtual uint32_t getPos() { return 0; }
  virtual bool isOpen() { return stream && stream->connected(); }
private:
  WiFiClient *stream;
};
AudioFileSourceStream *file;

// ==== Setup ====
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Connected!");

  // Use HTTPClient for a robust request
  http.begin("https://api.deepgram.com/v1/speak?model=aura-2-thalia-en");
  http.addHeader("Authorization", String("Token ") + DG_APIKEY);
  http.addHeader("Content-Type", "application/json");

  String requestBody = "{\"text\":\"Hello, this is a Deepgram TTS test on ESP32.\"}";
  
  Serial.println("Sending TTS request to Deepgram...");
  int httpCode = http.POST(requestBody);

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("✅ Got successful response, starting audio pipeline...");
    // Get a pointer to the clean, de-chunked stream
    WiFiClient *stream = http.getStreamPtr();

    // Setup audio pipeline with the clean stream
    file = new AudioFileSourceStream(stream);
    out  = new AudioOutputI2S(); 
    out->SetPinout(I2S_SPEAKER_BCLK, I2S_SPEAKER_LRC, I2S_SPEAKER_DIN);
    out->SetGain(0.8);
    out->SetRate(24000);

    mp3 = new AudioGeneratorMP3();
    if (!mp3->begin(file, out)) {
      Serial.println("❌ Failed to start MP3 decoder");
    } else {
      Serial.println("✅ MP3 Decoder started successfully.");
    }
  } else {
    Serial.printf("❌ HTTP POST failed, error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
  }
}

// ==== Loop ====
void loop() {
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      Serial.println("✅ MP3 playback finished.");
      mp3->stop();
      http.end(); // Clean up the HTTPClient connection
    }
  }
}