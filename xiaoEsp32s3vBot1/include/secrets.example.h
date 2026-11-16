#ifndef SECRETS_H
#define SECRETS_H

// =============================================================================
// Wi-Fi Credentials
// =============================================================================
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

// Legacy lowercase aliases for backward compatibility with older sketches
static const char* ssid     = WIFI_SSID;
static const char* password = WIFI_PASSWORD;

// =============================================================================
// Cloud API Keys (Track A: Standalone Cloud Pipeline)
// =============================================================================
#define DEEPGRAM_API_KEY "YOUR_DEEPGRAM_API_KEY"
#define GROQ_API_KEY     "YOUR_GROQ_API_KEY"

#endif // SECRETS_H
