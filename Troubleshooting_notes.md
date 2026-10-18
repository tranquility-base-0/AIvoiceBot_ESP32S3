# AIvoiceBot_ESP32S3 — Project Layout and Technical Handoff

## 1. Current repository organization

The repository is being reorganized to make two later ESP32-S3 voice-assistant prototypes the primary projects:

- `xiaoEsp32s3vBot0/` — earlier of the two newer PlatformIO iterations.
- `xiaoEsp32s3vBot1/` — later iteration, containing additional TTS diagnostics and audio-processing experiments.
- `previousDraft/` — archive of the earlier project folders and their associated README files. These are historical prototypes and reference material, not the primary development targets.

The five earlier project folders are:

- `TTStest_xiaoesp32s3_Dgram-main/`
- `TTStest_xiaoesp32s3_Dgram0-main/`
- `TTStest_xiaoesp32s3_Groq-main/`
- `voicebot_xiaoesp32s3-main/`
- `websocketTest_xiaoesp32s3-main/`

Their READMEs were minimal title-only placeholders. Other archived notes, including troubleshooting and TTS investigation notes, should be retained where useful and their locations documented accurately.

The root README should identify `xiaoEsp32s3vBot0/` and `xiaoEsp32s3vBot1/` as the main projects and explain that `previousDraft/` preserves the historical prototypes. Do not imply that either main project is production-ready or fully verified.

## 2. Shared architecture and scope

The projects explore an ESP32-S3 voice-assistant pipeline involving:

1. Microphone capture through I2S.
2. Speech recognition using Deepgram over WebSocket.
3. LLM responses using Groq.
4. Speech generation using Groq's text-to-speech API.
5. Audio playback through I2S.

The intended pipeline is not yet confirmed to work reliably end to end. Treat these projects as experimental firmware.

Both newer projects are PlatformIO projects targeting the Seeed XIAO ESP32-S3 and Arduino framework. Their dependency versions differ and should be evaluated separately rather than assumed interchangeable.

## 3. Differences between vBot0 and vBot1

### vBot0

- `src/main.cpp` is approximately 15,955 bytes.
- PlatformIO dependencies include ArduinoJson `^6.21.0` and WebSockets `^2.4.1`.
- The TTS function requests `playai-tts` audio from Groq, specifying the `Atlas-PlayAI` voice and WAV response format.
- It skips a presumed 44-byte WAV header, then streams audio data to I2S.
- Speaker initialization attempts to uninstall the existing I2S driver before reinstalling it with a valid configuration.
- The TTS code does not validate the WAV-header read length or check the results of I2S writes.

### vBot1

- `src/main.cpp` is approximately 28,023 bytes.
- PlatformIO dependencies include ArduinoJson `^7.2.0` and WebSockets `^2.3.6`.
- The active TTS function adds HTTP timeout configuration, a WAV-header read-length check, playback timing and byte-count diagnostics, heap reporting, yielding/delays, and 5× sample amplification with numerical clipping protection.
- Several apparent additional `playTTS()` definitions in the source are inside block comments; the inspected source has one active definition plus its normal declaration context. Do not mistake historical commented-out alternatives for duplicate compiled functions.
- The speaker initialization contains a suspicious call to `i2s_driver_install(I2S_SPEAKER_PORT, NULL, 0, NULL)` followed by another installation using the actual configuration, without first uninstalling the driver.

## 4. Findings requiring attention

### A. vBot1 speaker-driver initialization — priority

The legacy ESP-IDF I2S API expects a valid configuration pointer for `i2s_driver_install()`. Passing `NULL` is not a valid driver-presence check. The subsequent install also attempts installation without explicitly resolving the driver's existing state.

Neither project's speaker initialization consistently checks the return values of driver installation, pin configuration, or clock configuration. A success message therefore does not prove initialization succeeded.

Before changing this code, verify the actual Arduino-ESP32/ESP-IDF versions and applicable I2S API. Then use one explicit, error-checked initialization sequence appropriate to that version.

### B. TTS stream termination — both projects

Both projects use a loop conditioned on `http.connected()` and `stream->available() > 0`. The number of bytes immediately available is not necessarily the total amount of audio remaining. A temporary network gap may cause the loop to terminate before the entire response has been consumed.

Review the HTTP response-length semantics and stream timeout behavior. Handle short reads, temporary lack of data, end-of-stream, and errors explicitly. Check the return status and actual byte count of each `i2s_write()` call.

### C. WAV parsing — both projects

Both implementations assume that a WAV file consists of a 44-byte header followed immediately by PCM audio. This is not guaranteed for every valid WAV file because additional RIFF chunks may precede the audio data.

vBot1's check that all 44 bytes were read is an improvement over vBot0, but it does not resolve the structural assumption. A robust implementation should parse the RIFF/WAVE chunks and validate the audio format, sample rate, channel count, and bit depth before playback.

### D. vBot1 amplification

vBot1 multiplies signed 16-bit samples by 5 and clamps values to the representable range. Clamping prevents numeric overflow but does not prevent audible clipping; peaks beyond the range are flattened.

Evaluate the actual output level and signal peaks before choosing a gain. Prefer a conservative configurable gain or peak-aware scaling over arbitrary amplification.

### E. Error handling and protocol validation

The inspected TTS code does not check `i2s_write()` results. Earlier inspection also found unchecked JSON deserialization in the Groq response-handling path in both projects. Validate JSON parse success, the presence and type of expected response fields, HTTP status codes, and unexpected or malformed response bodies before use.

### F. Credentials

Both prototypes reference `GROQ_API_KEY`, and their source contains authorization-header construction. Credential-pattern scans returned matching line numbers but deliberately withheld matching source content. Those results do not establish whether any actual secret has been embedded.

Before committing either folder, confirm that local credential headers remain excluded from Git, check for tracked secrets without printing their values, and ensure that public documentation contains no keys or tokens. Do not remove local configuration files or expose credentials during the audit.

## 5. Verification status and workflow constraints

These findings are based on read-only source inspection, not a successful build, flash, or end-to-end runtime test. No claim is made that the projects compile or that audio playback is currently reliable.

The user builds and flashes firmware locally. Do not build, flash, delete, move, or rewrite project files without explicit authorization. Preserve both newer prototypes and the historical archive; prefer minimal, targeted changes after confirming the framework and API assumptions.

## 6. Immediate next steps

1. Complete the folder reorganization and check the resulting Git status.
2. Update the root README to document the new primary-project/archive layout.
3. Inspect each project's PlatformIO configuration and lock down the actual framework and library versions used.
4. Audit credential exclusions and tracked files before staging.
5. Review the I2S initialization, HTTP stream termination, WAV parsing, and error handling as separate changes.
6. Build and test locally only when the user chooses to proceed, recording the exact project and environment tested.
