# LLM-to-TTS Investigation Notes

**Project:** AIvoiceBot_ESP32S3
**Status:** Preliminary static review; no hardware tests performed
**Reviewed file:** `TTStest_xiaoesp32s3_Dgram-main/tts_xiaoesp32s3_deepgram.ino`

## 1. Purpose and scope

The immediate goal is to diagnose the unfinished LLM-to-TTS path on the ESP32-S3. The standalone Deepgram sketch is a useful reference because it sends a fixed text string to Deepgram, receives synthesized audio over HTTPS, and plays it through I²S.

This review examined the `speakText()` function only. No source files were modified, and no firmware was built or flashed. The standalone test's actual hardware behavior has not yet been confirmed.

## 2. Observations and potential failure points

### A. JSON request construction — high-priority investigation

The request body is assembled using string concatenation:

```cpp
String requestBody = String("{\"text\": \"") + text + "\"}";
```

This does not escape JSON-special characters in `text`. Quotes, backslashes, and literal control characters such as newlines can make the resulting JSON invalid.

**Relevance:** LLM-generated text can contain punctuation, quotations, and line breaks that a short hardcoded test sentence does not exercise.

**Suggested investigation:** Replace manual JSON construction with ArduinoJson or another proper JSON serializer. Check serialization errors and ensure the generated request body is valid JSON. Compare the outgoing text with the LLM response, without logging API credentials.

### B. WAV header handling — format assumption

The code assumes the WAV header is exactly 44 bytes:

```cpp
const size_t wavHeaderSize = 44;
```

It skips those bytes before treating the remaining data as PCM16 audio. A WAV file can contain additional chunks, so its audio payload does not universally begin at byte 44.

The request specifies `encoding=linear16` while also sending `Accept: audio/wav`. The code comments assume that the response is WAV-wrapped PCM, but the actual response format has not been verified.

**Suggested investigation:** Confirm Deepgram's actual response format and headers for the selected model and request parameters. If the response is WAV, locate the `data` chunk correctly rather than assuming a fixed header size. If the response is raw linear16 PCM, do not skip a WAV header.

### C. HTTP stream completion — possible premature termination

The loop is controlled by:

```cpp
while (http.connected()) {
    int len = stream->readBytes(buffer, sizeof(buffer));
    if (len <= 0) break;
}
```

Connection state alone does not establish whether the complete response body has been consumed. A read may also time out or return no bytes as the connection closes.

**Suggested investigation:** Inspect the HTTP response's content-length and transfer-encoding behavior, then implement a bounded read strategy appropriate to the actual response. Distinguish successful end-of-body from timeout, incomplete response, and transport error. Ensure the HTTP client is always closed cleanly.

### D. PCM16 sample alignment — chunk-boundary handling

The code discards a final byte when a received chunk has an odd length:

```cpp
if ((procLen & 1) != 0) {
    procLen -= 1;
}
```

PCM16 samples consist of two bytes. An HTTP read boundary can occur between those bytes, so discarding the last byte can corrupt a sample and misalign the following data.

**Suggested investigation:** Carry an unmatched byte forward to the next read, preserving sample alignment across the entire response.

### E. Audio format and I²S configuration — verify together

The sketch treats the audio as 16-bit little-endian mono PCM and duplicates each sample into left and right channels before calling `i2s_write()`.

This is a reasonable mono-to-stereo conversion if the assumptions are correct. However, the configured I²S sample rate must match the actual decoded audio sample rate. The relevant configuration is in `setupI2S()`, which should be reviewed alongside the response format.

The code also contains a chunk-size guard that truncates PCM data if the stereo buffer cannot hold it. That should be checked to ensure the configured buffer is large enough and that truncation is not occurring during normal operation.

## 3. Most useful diagnostic split

The next step depends on whether the standalone Deepgram sketch works:

- **Standalone test works:** Compare the main bot's LLM response handling, JSON serialization, TTS request, and response playback with the standalone test.
- **Standalone test fails:** Diagnose the Deepgram HTTP response and PCM/I²S pipeline before investigating LLM integration.
- **Simple text works, but some LLM responses fail:** Prioritize JSON escaping, response length, and stream handling.

## 4. Recommended investigation order

1. Inspect the main voicebot sketch's LLM response parsing and the code that passes text to TTS.
2. Inspect the Groq experiment to understand its request/response format and identify what differs from the main bot.
3. Verify the Deepgram response encoding and WAV-versus-raw-PCM behavior.
4. Fix JSON serialization and stream/sample-boundary handling as warranted by the source review.
5. Review I²S sample rate, channel format, and buffer sizes.
6. Only then plan targeted hardware tests that distinguish request errors, incomplete responses, and playback errors.

## 5. Current assessment

The manual JSON construction is a particularly relevant integration risk because generated LLM text is less predictable than the hardcoded sentence. The WAV-header assumption, stream termination, and odd-byte handling are additional concrete weaknesses worth investigating.

None of these has been confirmed as the root cause. There are no hardware results yet, and the standalone test's success or failure remains unknown.

**Constraint:** Preserve the existing sketches and ZIP archives while investigating. Do not consolidate or rewrite the TTS pipeline until the main bot and test variants have been compared.
