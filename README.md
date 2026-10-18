# AIvoiceBot ESP32-S3

**Status: Under construction — experimental prototype**

AIvoiceBot is an experimental voice-assistant project built around the ESP32-S3. It explores a speech-processing pipeline that combines speech-to-text (STT), a large language model (LLM), and text-to-speech (TTS).

The intended pipeline uses Deepgram for speech recognition and Groq for language generation and speech synthesis, with I2S audio input and output.

## Current projects

Development is focused on two newer PlatformIO projects for the Seeed XIAO ESP32-S3:

- **[`xiaoEsp32s3vBot0/`](xiaoEsp32s3vBot0/)** — an earlier iteration of the newer prototypes.
- **[`xiaoEsp32s3vBot1/`](xiaoEsp32s3vBot1/)** — a later iteration with additional TTS diagnostics and audio-processing experiments.

These projects have different dependency versions and implementation details. Treat them as separate experimental snapshots; do not assume that their configurations or behavior are interchangeable.

The intended processing pipeline is:

1. Capture microphone audio using I2S.
2. Transcribe speech through Deepgram.
3. Generate a response using Groq.
4. Request synthesized speech from Groq.
5. Play the audio through the I2S speaker output.

Reliable end-to-end operation has not yet been fully verified.

## Historical prototypes

The [`previousDraft/`](previousDraft/) directory contains earlier project folders and their associated README files. These materials are retained for historical reference, comparison, and recovery of useful implementation details.

The archived prototypes include earlier Deepgram, Groq, text-to-speech, and WebSocket experiments. They are not the primary development targets.

## Known areas for investigation

The newer prototypes are still under review. Areas requiring further verification include:

- **I2S initialization:** check driver installation, pin configuration, clock setup, and error handling.
- **TTS streaming:** ensure playback consumes the complete HTTP response and handles network delays and short reads correctly.
- **WAV parsing:** validate the audio format and locate the actual audio-data chunk instead of assuming every WAV header is exactly 44 bytes.
- **Audio playback:** check I2S write results and assess amplification, clipping, and output quality.
- **Response validation:** handle HTTP errors, malformed JSON, and unexpected API response structures.
- **Credentials:** ensure API keys and other secrets remain local and are never committed to version control.

These are investigation items, not a claim that every listed issue has been reproduced at runtime.

## Development

Development is currently focused on reviewing the newer prototypes, identifying integration issues, and improving the reliability of audio playback.

See [Troubleshooting notes](Troubleshooting_notes.md) for investigation findings, and consult any project-specific notes available alongside the relevant source files.

Build and test the projects independently using their respective PlatformIO configurations. Hardware compatibility, dependency behavior, and end-to-end reliability remain to be verified.

## Disclaimer

This repository is provided for experimentation and development. Reproducing the prototypes may require compatible hardware, API credentials, and access to external services.

Keep API keys and other credentials private. Never commit secrets to version control. This project is not a finished or release-ready application; functionality, reliability, and hardware compatibility may change as development continues.
