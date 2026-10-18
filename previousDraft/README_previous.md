# AIvoiceBot ESP32-S3

**Status: Under construction — experimental prototype**

AIvoiceBot is an experimental voice-assistant project built around the ESP32-S3. The project explores a speech-processing pipeline that combines speech-to-text (STT), a large language model (LLM), and text-to-speech (TTS).

The intended pipeline uses Deepgram for speech recognition and Groq for language generation and speech synthesis.

## Current status

The project is a work in progress. Several experimental sketches and standalone tests have been collected, but the integrated voice-assistant implementation has not yet been fully verified. In particular, reliable end-to-end LLM-to-TTS playback remains under investigation.

**This project is not yet a finished or release-ready application.** Functionality, reliability, and hardware compatibility may change as development continues.

## Development

Development is currently focused on reviewing the existing prototypes, identifying integration issues, and improving the reliability of audio playback.

See [Troubleshooting notes](Troubleshooting%20notes.md) for the current investigation findings.

## Disclaimer

This repository is provided for experimentation and development. You may need compatible hardware, API credentials, and service access to reproduce the prototypes. Keep API keys and other credentials private; never commit them to version control.
