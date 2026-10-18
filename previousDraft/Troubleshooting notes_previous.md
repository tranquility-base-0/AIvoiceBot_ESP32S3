# AIvoiceBot ESP32-S3 — LLM-to-TTS Investigation Handoff

**Status:** Static investigation in progress; root cause not established
**Last reviewed:** 2026-10-18
**Repository:** `~/Projects/maker/AIvoiceBot_ESP32S3`
**Branch:** `main` with a dirty working-tree indicator (`main*` observed)
**Hardware testing:** Not currently available

## 1. Investigation constraints

- The user runs terminal commands locally.
- Begin with read-only inspection. Do not edit source, build, flash, commit, or reorganize files without explicit authorization.
- Preserve the original archived sketches and their directory structure.
- Never print or request `secrets.h`, `config.h`, `config1.h`, `config4.h`, or other credential-bearing files.
- Do not infer that a plausible code defect is the confirmed cause of the reported behavior.
- Prefer targeted changes over a broad redesign once evidence identifies the failure.

The local repository has a `.gitignore` covering the credential filenames:

```gitignore
# Local credentials
secrets.h
config.h
config1.h
config4.h
```

The root `secrets.h` exists. Credential-bearing configuration files should remain private. A sanitized `secrets.example.h` may be useful later, but only after the required configuration variables are established.

## 2. Archived sketches

The following `.ino` files were found:

```text
TTStest_xiaoesp32s3_Dgram-main/tts_xiaoesp32s3_deepgram.ino
TTStest_xiaoesp32s3_Dgram0-main/TTStest_xiaoesp32s3_Dgram.ino
TTStest_xiaoesp32s3_Groq-main/TTStest_xiaoesp32s3_Groq/TTStest_xiaoesp32s3_Groq.ino
voicebot_xiaoesp32s3-main/chatbot_xiaoesp32s3_2/chatbot_xiaoesp32s3_2.ino
websocketTest_xiaoesp32s3-main/websocketTest_xiaoesp32s3.ino
websocketTest_xiaoesp32s3-main/websocketTest_xiaoesp32s3/websocketTest_xiaoesp32s3.ino
```

Approximate chronology from the archived material:

- Main chatbot: August 18, 2025
- WebSocket prototype: August 27, 2025
- Groq TTS test: September 10, 2025
- Deepgram TTS tests: November 17, 2025

These are experimental Arduino-style sketches, not a single coherent finished application. The Groq and Deepgram standalone tests are useful references, but neither should be assumed correct without verification.

## 3. Most relevant sketch: integrated WebSocket bot

**File:**

`websocketTest_xiaoesp32s3-main/websocketTest_xiaoesp32s3.ino`

This is the only inspected sketch containing the intended full pipeline:

```text
Microphone / I²S capture
    → Deepgram WebSocket STT
    → utterance-end detection
    → Groq chat completions
    → extract choices[0].message.content
    → Groq text-to-speech
    → WAV bytes
    → I²S speaker output
```

The second, nested WebSocket sketch appears to be an earlier STT-to-LLM version; it does not appear to contain the same TTS implementation.

### Hardware and audio configuration

Integrated bot:

- Microphone I²S port: `I2S_NUM_0`
- Microphone pins: BCLK 6, WS 5, data 4
- Speaker I²S port: `I2S_NUM_1`
- Speaker pins: BCLK 8, WS 7, data 9
- Speaker I²S configured for 24,000 Hz, 16-bit, mono

Relevant definitions include:

```cpp
#define I2S_MIC_PORT I2S_NUM_0
#define I2S_SPEAKER_PORT I2S_NUM_1
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512
```

The microphone's 16 kHz rate and speaker's 24 kHz rate are separate paths; that difference is not inherently a defect. However, the speaker's fixed 24 kHz rate must agree with the TTS audio format.

## 4. LLM response extraction

`sendToGroq(String prompt)` is around lines 120–212.

The request uses:

- Endpoint: `https://api.groq.com/openai/v1/chat/completions`
- Model: `llama-3.3-70b-versatile`
- A system prompt and conversation history
- ArduinoJson serialization

When the response HTTP status is 200, the code reads the body and extracts:

```cpp
const char* response_text =
    responseDoc["choices"][0]["message"]["content"];
```

It then prints the response, adds it to conversation history, and calls:

```cpp
playTTS(String(response_text));
```

### Findings

- The LLM-to-TTS handoff is present in the source.
- JSON deserialization results are not checked before accessing the nested response field.
- The code does not robustly validate that the `choices`, `message`, and `content` fields exist or that content is non-null before passing it to TTS.

These are verified static weaknesses. They could cause failures when the API returns an error body, malformed response, or unexpected structure, but there is no evidence yet that they explain the observed problem.

## 5. Active integrated TTS implementation

The first active `playTTS(String text)` is around lines 217–271.

There are five textual occurrences of `void playTTS(String text)` in the file, approximately at lines 217, 274, 321, 391, and 455. The later occurrences are inside a block comment containing accumulated experimental implementations. The grep count alone does **not** establish a duplicate-function compilation error.

The active function sends a Groq TTS request with these parameters:

```cpp
doc["model"] = "playai-tts";
doc["input"] = text;
doc["voice"] = "Atlas-PlayAI";
doc["response_format"] = "wav";
```

It reads 44 bytes from the response stream as a presumed WAV header, then processes the remaining bytes as signed 16-bit PCM. It multiplies every sample by 10 and clamps it to the 16-bit range before calling `i2s_write()`.

### Verified concerns

1. **Fixed WAV-header assumption.** It reads 44 bytes without checking the number of bytes read or validating the header. WAV files can contain extra chunks, so the data section is not guaranteed to start at byte 44.

2. **No inspection of the returned audio format.** The active function does not read the WAV sample rate, bit depth, channel count, or data size.

3. **Fixed speaker sample rate.** `configureI2S_TX()` configures 24 kHz mono 16-bit playback. The TTS request does not explicitly specify a sample rate.

4. **Potentially premature stream termination.** The loop condition is:

   ```cpp
   while (http.connected() && (stream->available() > 0))
   ```

   `available()` reports currently available bytes, not necessarily whether the entire response has been consumed. The loop may exit during a temporary lull and truncate audio, depending on the stream behavior.

5. **Unchecked I²S write.** The return value from `i2s_write()` is ignored, as is the reported `bytes_written` count. There is no diagnostic evidence that I²S accepted the requested audio.

6. **Aggressive amplification.** Multiplication by `10.0f` can cause extensive clipping and severe distortion. It may degrade audio quality, but does not by itself establish the cause of silence.

7. **No bounded failure recovery.** The function does not robustly handle a short header read, truncated body, or a failed I²S configuration. Some of these conditions can result in silent or incomplete playback.

8. **Static buffer comment is unverified.** The source comments describe a static buffer as a fix for stack overflow:

   ```cpp
   static uint8_t buffer[BUFFER_SIZE * 2];
   ```

   The buffer is indeed static, but no evidence establishes that stack overflow was the original cause. Do not treat the comment as a verified diagnosis.

### Current interpretation

A mismatch between the returned WAV sample rate and the fixed 24 kHz speaker configuration is a credible hypothesis for incorrect playback speed or pitch. It does not fully explain complete silence. The Groq response, header handling, I²S initialization, and playback loop remain separate failure points.

## 6. I²S initialization and diagnostics

`configureI2S_TX()` is around lines 647–683.

It:

- Uninstalls the speaker I²S driver.
- Installs a TX driver configured for 24 kHz, 16-bit, mono.
- Sets the speaker pins.
- Calls `i2s_set_clk()` with the same 24 kHz mono configuration.

The return values from `i2s_driver_uninstall()`, `i2s_driver_install()`, `i2s_set_pin()`, and `i2s_set_clk()` are not checked in the inspected code.

The function prints:

```text
Speaker I2S Configured (Final Version).
```

That message is not proof that the driver installed or configured successfully, because the return codes are ignored.

The active TTS path logs a failure HTTP status but has no logging for:

- The response content type or length
- Header-read length or WAV fields
- Number of audio bytes received
- Number of bytes accepted by I²S
- I²S error codes
- Whether the stream ended normally or prematurely

No actual serial logs from a failing hardware run have been collected in this investigation.

## 7. Standalone Groq TTS test

**File:**

`TTStest_xiaoesp32s3_Groq-main/TTStest_xiaoesp32s3_Groq/TTStest_xiaoesp32s3_Groq.ino`

Function: `playGroqTTS(String text)`, around lines 70–186.

Request parameters:

```cpp
doc["model"] = "playai-tts";
doc["input"] = text;
doc["voice"] = "Fritz-PlayAI";
doc["response_format"] = "wav";
```

The standalone test differs from the integrated bot in several important ways:

- It searches for `RIFF` before reading the rest of a 44-byte header.
- It extracts the sample rate, bit depth, channel count, and data size.
- It reconfigures I²S using the extracted audio parameters.
- It checks the `i2s_write()` result and increments playback progress by `bytes_written`.
- It does not use the integrated bot's ×10 amplification.

Its I²S setup function uses the parsed sample rate and bit depth, selects mono or stereo from the parsed channel count, and sets the same speaker pins. Its startup configuration uses 48 kHz mono as a temporary default.

### Limitations in the standalone test

This is a useful comparison, not a proven-good reference implementation.

- It assumes a 44-byte WAV header after finding `RIFF`.
- It does not robustly validate `WAVE`, `fmt `, or `data` chunks.
- Its loop waits until the declared `dataSize` has been played, but has no proper bounded timeout or recovery if the connection ends early or I²S repeatedly fails.
- It trims odd byte counts under a 16-bit-sample assumption, even though bit depth is read dynamically.
- It does not check all I²S setup calls for errors.
- Its `stream->available()` checks and subsequent reads need careful review for timeout and truncation behavior.

The main useful difference is that the standalone implementation attempts to adapt playback to the WAV header rather than assuming the format.

## 8. Other prototypes

### Deepgram standalone TTS

`TTStest_xiaoesp32s3_Dgram-main/tts_xiaoesp32s3_deepgram.ino`

- Sends fixed text to Deepgram's `/v1/speak` endpoint.
- Requests `encoding=linear16` and `audio/wav`.
- Manually constructs the JSON request body.
- Uses HTTP streaming and assumes a 44-byte WAV header before treating the rest as PCM16.

Potential concerns include JSON escaping, assumptions about WAV layout, incomplete handling of stream termination, and possible loss of byte alignment when a read returns an odd number of bytes. These are static concerns only.

### Main chatbot prototype

`voicebot_xiaoesp32s3-main/chatbot_xiaoesp32s3_2/chatbot_xiaoesp32s3_2.ino`

This is a fixed-text Deepgram TTS test, not the integrated LLM bot. It sends a hard-coded sentence and attempts to play the HTTP stream using `AudioGeneratorMP3`.

The request does not explicitly request MP3 encoding in the inspected code, so the decoder's compatibility with the actual response format is uncertain. It should not be confused with the Groq-integrated path.

## 9. Prioritized next steps

Proceed read-only until the user authorizes a change.

### Step 1 — Establish the actual symptom

Ask for the exact observed behavior, if not already known:

- Does the bot print a valid LLM response?
- Does it print that TTS playback started and finished?
- Is playback silent, distorted, too fast/slow, or truncated?
- Does it hang after starting playback?
- Is the HTTP status 200 or an error?

Avoid treating all these symptoms as the same bug.

### Step 2 — Inspect the complete active TTS path and setup

Review the exact active function and I²S configuration, including whether any other code reinstalls or reconfigures the speaker driver around microphone capture and playback. Keep the review focused on active code, not commented-out experiments.

### Step 3 — Identify response-format facts

Check the current official Groq TTS documentation if API parameter support or defaults are material. Do not assume that the service returns 24 kHz simply because the ESP32 is configured for 24 kHz. If the service supports an explicit sample-rate parameter, verify its exact accepted values and semantics before recommending it.

### Step 4 — Define minimum useful diagnostics

If authorized, propose a small diagnostic patch that records:

- HTTP status and content type/length, where available
- Exact number of header bytes read
- Validated WAV fields
- Total audio bytes read
- Total bytes reported written by I²S
- I²S setup/write error codes
- Normal completion versus timeout/truncated stream

Do not log API keys, authorization headers, or other secrets.

### Step 5 — Fix only the failure established by evidence

Possible fixes may include validating the LLM response before calling TTS, validating the WAV format and data section, matching the I²S clock to the actual audio format, handling stream termination, or checking I²S errors. Do not transplant the standalone implementation wholesale or combine all possible fixes into one unverified redesign.

## 10. Current conclusion

The source contains a complete intended STT → LLM → TTS path, but the active integrated TTS function is less robust than the standalone Groq test. The highest-value leads are:

1. Unvalidated LLM response extraction.
2. Unvalidated 44-byte WAV-header assumption.
3. Fixed 24 kHz speaker configuration without checking the returned WAV format.
4. Incomplete stream-end detection.
5. Unchecked I²S initialization and write results.
6. ×10 amplification, which can cause clipping.

**No root cause has been confirmed.** No source changes, builds, flashes, or commits were performed during this investigation. The next agent should continue with read-only verification and obtain actual failure logs or hardware observations before implementing a fix.
