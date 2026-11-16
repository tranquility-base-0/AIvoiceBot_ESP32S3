#!/usr/bin/env python3
"""
Track B Host Bridge Server (host_bridge/bridge_server.py)

Exposes ws://<HOST>:8765/ws/audio for Seeed Studio XIAO ESP32-S3 (xiaoEsp32s3vBot2):
  1. Ingests 16000 Hz 16-bit mono (s16le) PCM frames from INMP441 with RMS VAD endpointing.
  2. Transcribes speech via local faster-whisper or Deepgram API proxy.
  3. Streams chat completions from Groq (llama-3.3-70b-versatile, configurable OPENAI_BASE_URL).
  4. Synthesizes sentence chunks via Kokoro-ONNX at 24000 Hz, converts float32 -> int16 (s16le),
     applies optional glitch/espeak-ng DSP hooks, and streams strictly 2-byte-aligned binary PCM
     frames back to the ESP32-S3 for playback on MAX98357A.
"""

from __future__ import annotations

import asyncio
import io
import logging
import os
import random
import re
import shutil
import subprocess
import time
import wave
from pathlib import Path
from typing import AsyncGenerator, List, Dict, Optional

import numpy as np
from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect

# Load environment variables from host_bridge/.env if present
BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("host_bridge")

# =============================================================================
# CONFIGURATION
# =============================================================================
BRIDGE_HOST = os.getenv("BRIDGE_HOST", "0.0.0.0")
BRIDGE_PORT = int(os.getenv("BRIDGE_PORT", "8765"))

STT_BACKEND = os.getenv("STT_BACKEND", "faster_whisper").strip().lower()
WHISPER_MODEL_SIZE = os.getenv("WHISPER_MODEL_SIZE", "base.en")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
DEEPGRAM_API_KEY = os.getenv("DEEPGRAM_API_KEY", "")

MIC_SAMPLE_RATE = 16000
TTS_SAMPLE_RATE = 24000
PCM_CHUNK_BYTES = 2048  # Strictly even byte count (1024 int16 samples per WS frame)

VAD_RMS_THRESHOLD = float(os.getenv("VAD_RMS_THRESHOLD", "350.0"))
VAD_SILENCE_MS = int(os.getenv("VAD_SILENCE_MS", "900"))
VAD_MIN_SPEECH_MS = int(os.getenv("VAD_MIN_SPEECH_MS", "300"))

GROQ_API_KEY = os.getenv("GROQ_API_KEY", "")
OPENAI_BASE_URL = os.getenv("OPENAI_BASE_URL", "https://api.groq.com/openai/v1")
LLM_MODEL = os.getenv("LLM_MODEL", "llama-3.3-70b-versatile")
SYSTEM_PROMPT_FILE = os.getenv("SYSTEM_PROMPT_FILE", "personas/system_prompt.txt")
MAX_HISTORY_TURNS = 6

KOKORO_MODEL_PATH = os.getenv("KOKORO_MODEL_PATH", str(BASE_DIR / "kokoro-v1.0.onnx"))
KOKORO_VOICES_PATH = os.getenv("KOKORO_VOICES_PATH", str(BASE_DIR / "voices-v1.0.bin"))
KOKORO_VOICE = os.getenv("KOKORO_VOICE", "af_heart")
KOKORO_SPEED = float(os.getenv("KOKORO_SPEED", "1.0"))
KOKORO_LANG = os.getenv("KOKORO_LANG", "en-us")

ENABLE_GLITCH_DSP = os.getenv("ENABLE_GLITCH_DSP", "false").strip().lower() in ("1", "true", "yes")
GLITCH_STUTTER_PROB = float(os.getenv("GLITCH_STUTTER_PROB", "0.18"))
GLITCH_ESPEAK_PROB = float(os.getenv("GLITCH_ESPEAK_PROB", "0.10"))


def load_system_prompt() -> str:
    prompt_path = Path(SYSTEM_PROMPT_FILE)
    if not prompt_path.is_absolute():
        prompt_path = BASE_DIR / prompt_path
    if prompt_path.exists():
        lines = [
            line.strip()
            for line in prompt_path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.strip().startswith("#")
        ]
        if lines:
            return " ".join(lines)
    return (
        "You are TARS, an AI assistant designed for exploration missions. "
        "Your personality is dry, witty, pragmatic, and loyal. "
        "Answer in 1-3 sentences optimised for being heard over an audio link."
    )


# =============================================================================
# LAZY SINGLETON ENGINES (STT, LLM, KOKORO-ONNX TTS)
# =============================================================================
_whisper_model = None
_kokoro_engine = None
_openai_client = None


def get_whisper_model():
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel

        logger.info(
            "Loading faster-whisper model '%s' (device=%s, compute=%s)...",
            WHISPER_MODEL_SIZE,
            WHISPER_DEVICE,
            WHISPER_COMPUTE_TYPE,
        )
        _whisper_model = WhisperModel(
            WHISPER_MODEL_SIZE,
            device=WHISPER_DEVICE,
            compute_type=WHISPER_COMPUTE_TYPE,
        )
    return _whisper_model


def get_kokoro_engine():
    global _kokoro_engine
    if _kokoro_engine is None:
        from kokoro_onnx import Kokoro

        logger.info(
            "Loading Kokoro-ONNX model=%s voices=%s...",
            KOKORO_MODEL_PATH,
            KOKORO_VOICES_PATH,
        )
        _kokoro_engine = Kokoro(KOKORO_MODEL_PATH, KOKORO_VOICES_PATH)
    return _kokoro_engine


def get_openai_client():
    global _openai_client
    if _openai_client is None:
        from openai import AsyncOpenAI

        _openai_client = AsyncOpenAI(
            api_key=GROQ_API_KEY,
            base_url=OPENAI_BASE_URL,
        )
    return _openai_client


# =============================================================================
# SPEECH-TO-TEXT (FASTER-WHISPER OR DEEPGRAM PROXY)
# =============================================================================
def _pcm16_to_wav_bytes(pcm_bytes: bytes, sample_rate: int = MIC_SAMPLE_RATE) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return buf.getvalue()


async def transcribe_utterance(pcm_bytes: bytes) -> str:
    """Transcribe 16 kHz s16le mono PCM bytes using faster-whisper or Deepgram."""
    if len(pcm_bytes) < 2:
        return ""

    if STT_BACKEND == "deepgram":
        import httpx

        if not DEEPGRAM_API_KEY:
            logger.error("STT_BACKEND=deepgram but DEEPGRAM_API_KEY is not set.")
            return ""

        wav_payload = _pcm16_to_wav_bytes(pcm_bytes, MIC_SAMPLE_RATE)
        url = "https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true&punctuate=true"
        headers = {
            "Authorization": f"Token {DEEPGRAM_API_KEY}",
            "Content-Type": "audio/wav",
        }
        async with httpx.AsyncClient(timeout=15.0) as client:
            resp = await client.post(url, content=wav_payload, headers=headers)
            resp.raise_for_status()
            data = resp.json()
            return (
                data.get("results", {})
                .get("channels", [{}])[0]
                .get("alternatives", [{}])[0]
                .get("transcript", "")
                .strip()
            )

    # Default: local faster-whisper
    def _run_whisper() -> str:
        model = get_whisper_model()
        samples_i16 = np.frombuffer(pcm_bytes, dtype=np.int16)
        samples_f32 = samples_i16.astype(np.float32) / 32768.0
        segments, _ = model.transcribe(
            samples_f32,
            beam_size=1,
            language="en",
            vad_filter=True,
        )
        return " ".join(seg.text.strip() for seg in segments).strip()

    return await asyncio.to_thread(_run_whisper)


# =============================================================================
# GROQ LLM STREAMING SENTENCE GENERATOR
# =============================================================================
SENTENCE_SPLIT_RE = re.compile(r"(?<=[.!?])\s+")


async def stream_llm_sentences(
    user_text: str,
    history: List[Dict[str, str]],
) -> AsyncGenerator[str, None]:
    """Stream tokens from Groq LLM and yield complete sentences as soon as ready."""
    client = get_openai_client()
    messages: List[Dict[str, str]] = [
        {"role": "system", "content": load_system_prompt()},
        *history[-MAX_HISTORY_TURNS:],
        {"role": "user", "content": user_text},
    ]

    stream = await client.chat.completions.create(
        model=LLM_MODEL,
        messages=messages,
        temperature=0.7,
        max_tokens=180,
        stream=True,
    )

    buffer = ""
    full_reply_parts: List[str] = []

    async for chunk in stream:
        delta = chunk.choices[0].delta.content if chunk.choices else None
        if not delta:
            continue
        buffer += delta
        parts = SENTENCE_SPLIT_RE.split(buffer)
        if len(parts) > 1:
            for sentence in parts[:-1]:
                cleaned = sentence.strip()
                if cleaned:
                    full_reply_parts.append(cleaned)
                    yield cleaned
            buffer = parts[-1]

    tail = buffer.strip()
    if tail:
        full_reply_parts.append(tail)
        yield tail

    assistant_reply = " ".join(full_reply_parts).strip()
    if assistant_reply:
        history.append({"role": "user", "content": user_text})
        history.append({"role": "assistant", "content": assistant_reply})
        if len(history) > MAX_HISTORY_TURNS:
            del history[:-MAX_HISTORY_TURNS]


# =============================================================================
# GLITCH / DSP POST-PROCESSING HOOK (OPTIONAL MAX HEADROOM / ESPEAK-NG EFFECT)
# =============================================================================
def _synthesize_espeak_burst_24k(word: str) -> Optional[np.ndarray]:
    """Optionally synthesize a short robotic espeak-ng burst resampled to 24 kHz int16."""
    espeak_bin = shutil.which("espeak-ng") or shutil.which("espeak")
    if not espeak_bin:
        return None
    try:
        proc = subprocess.run(
            [espeak_bin, "-s", "190", "-p", "35", "--stdout", word],
            capture_output=True,
            timeout=2.0,
            check=False,
        )
        if proc.returncode != 0 or not proc.stdout:
            return None
        with wave.open(io.BytesIO(proc.stdout), "rb") as wf:
            src_rate = wf.getframerate()
            raw = wf.readframes(wf.getnframes())
        samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
        if src_rate != TTS_SAMPLE_RATE and len(samples) > 1:
            duration = len(samples) / float(src_rate)
            target_len = int(duration * TTS_SAMPLE_RATE)
            x_old = np.linspace(0.0, 1.0, num=len(samples))
            x_new = np.linspace(0.0, 1.0, num=target_len)
            samples = np.interp(x_new, x_old, samples)
        return np.clip(samples, -32768, 32767).astype(np.int16)
    except Exception as exc:
        logger.debug("espeak-ng burst skipped: %s", exc)
        return None


def apply_glitch_dsp(
    pcm_int16: np.ndarray,
    sentence_text: str = "",
    sample_rate: int = TTS_SAMPLE_RATE,
) -> np.ndarray:
    """
    Optional post-processing DSP hook (enabled via ENABLE_GLITCH_DSP=true in .env).
    Injects Max Headroom-style buffer stutters (repeated 60-90 ms slices) and optional
    espeak-ng robotic syllable bursts into the 24000 Hz s16le stream.
    """
    if not ENABLE_GLITCH_DSP or len(pcm_int16) < sample_rate // 10:
        return pcm_int16

    out = pcm_int16.copy()

    # 1. Buffer stutter / repeat slice
    if random.random() < GLITCH_STUTTER_PROB:
        slice_len = int(sample_rate * random.uniform(0.055, 0.095))
        max_start = max(1, len(out) - slice_len - 1)
        start = random.randint(0, max_start)
        stutter_slice = out[start : start + slice_len]
        repeats = random.randint(2, 4)
        stuttered = np.tile(stutter_slice, repeats)
        out = np.concatenate([out[:start], stuttered, out[start:]])

    # 2. Optional robotic espeak-ng burst on last word
    words = [w.strip(".,!?*\"'") for w in sentence_text.split() if w.strip(".,!?*\"'")]
    if words and random.random() < GLITCH_ESPEAK_PROB:
        burst = _synthesize_espeak_burst_24k(words[-1])
        if burst is not None and len(burst) > 0:
            out = np.concatenate([out, burst])

    return out


# =============================================================================
# KOKORO-ONNX 24000 HZ TTS SYNTHESIS (FLOAT32 -> S16LE)
# =============================================================================
def _resample_float32(samples: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    if orig_sr == target_sr or len(samples) <= 1:
        return samples
    duration = len(samples) / float(orig_sr)
    target_len = int(round(duration * target_sr))
    x_old = np.linspace(0.0, 1.0, num=len(samples), endpoint=False)
    x_new = np.linspace(0.0, 1.0, num=target_len, endpoint=False)
    return np.interp(x_new, x_old, samples).astype(np.float32)


def synthesize_sentence_s16le(sentence: str) -> bytes:
    """Synthesize sentence via Kokoro-ONNX at 24000 Hz and return s16le PCM bytes."""
    kokoro = get_kokoro_engine()
    samples_f32, sample_rate = kokoro.create(
        sentence,
        voice=KOKORO_VOICE,
        speed=KOKORO_SPEED,
        lang=KOKORO_LANG,
    )
    samples_f32 = np.asarray(samples_f32, dtype=np.float32).flatten()
    if sample_rate != TTS_SAMPLE_RATE:
        samples_f32 = _resample_float32(samples_f32, sample_rate, TTS_SAMPLE_RATE)

    # Convert float32 [-1.0, 1.0] -> int16 s16le
    pcm_int16 = np.clip(samples_f32 * 32767.0, -32768.0, 32767.0).astype("<i2")
    pcm_int16 = apply_glitch_dsp(pcm_int16, sentence_text=sentence, sample_rate=TTS_SAMPLE_RATE)
    raw_bytes = pcm_int16.tobytes()

    # Enforce strict 2-byte alignment
    if len(raw_bytes) & 1:
        raw_bytes = raw_bytes[:-1]
    return raw_bytes


# =============================================================================
# FASTAPI APPLICATION & WEBSOCKET ENDPOINT (/ws/audio)
# =============================================================================
app = FastAPI(title="AIvoiceBot ESP32-S3 Host Bridge (Track B)")


@app.get("/health")
async def health_check():
    return {
        "status": "ok",
        "stt_backend": STT_BACKEND,
        "llm_model": LLM_MODEL,
        "tts_sample_rate": TTS_SAMPLE_RATE,
        "glitch_dsp": ENABLE_GLITCH_DSP,
    }


@app.websocket("/ws/audio")
async def websocket_audio_endpoint(websocket: WebSocket):
    await websocket.accept()
    client_host = websocket.client.host if websocket.client else "unknown"
    logger.info("ESP32-S3 audio terminal connected from %s", client_host)

    history: List[Dict[str, str]] = []
    utterance_buf = bytearray()
    carry_byte: Optional[bytes] = None
    is_speaking_user = False
    speech_start_ms = 0.0
    last_voice_ms = 0.0

    async def process_and_stream_turn(pcm_utterance: bytes):
        t0 = time.monotonic()
        transcript = await transcribe_utterance(pcm_utterance)
        if not transcript or len(transcript.strip()) < 2:
            logger.info("Empty/short STT transcript ignored.")
            return

        logger.info("[STT %.0f ms] User: %s", (time.monotonic() - t0) * 1000.0, transcript)

        await websocket.send_text("TTS_START")
        try:
            async for sentence in stream_llm_sentences(transcript, history):
                logger.info("[LLM->TTS] Synthesizing: %s", sentence)
                pcm_s16le = await asyncio.to_thread(synthesize_sentence_s16le, sentence)
                # Stream 24000 Hz s16le binary frames in even-sized chunks
                for offset in range(0, len(pcm_s16le), PCM_CHUNK_BYTES):
                    chunk = pcm_s16le[offset : offset + PCM_CHUNK_BYTES]
                    if len(chunk) & 1:
                        chunk = chunk[:-1]
                    if chunk:
                        await websocket.send_bytes(chunk)
                        # Pace slightly below real-time burst ceiling so ESP32 TCP buffer never drops
                        await asyncio.sleep(0.01)
        finally:
            await websocket.send_text("TTS_END")
            logger.info("[TTS] Playback stream complete.")

    try:
        while True:
            message = await websocket.receive()
            if message.get("type") == "websocket.disconnect":
                break

            data_bytes = message.get("bytes")
            if data_bytes:
                # Enforce 2-byte sample alignment on incoming mic frames
                if carry_byte is not None:
                    data_bytes = carry_byte + data_bytes
                    carry_byte = None
                if len(data_bytes) & 1:
                    carry_byte = data_bytes[-1:]
                    data_bytes = data_bytes[:-1]
                if not data_bytes:
                    continue

                samples = np.frombuffer(data_bytes, dtype=np.int16).astype(np.float32)
                rms = float(np.sqrt(np.mean(samples * samples))) if len(samples) > 0 else 0.0
                now_ms = time.monotonic() * 1000.0

                if rms >= VAD_RMS_THRESHOLD:
                    if not is_speaking_user:
                        is_speaking_user = True
                        speech_start_ms = now_ms
                        logger.debug("VAD speech start (RMS=%.1f)", rms)
                    last_voice_ms = now_ms
                    utterance_buf.extend(data_bytes)
                elif is_speaking_user:
                    utterance_buf.extend(data_bytes)
                    if (now_ms - last_voice_ms) >= VAD_SILENCE_MS:
                        speech_dur = last_voice_ms - speech_start_ms
                        captured = bytes(utterance_buf)
                        utterance_buf.clear()
                        is_speaking_user = False

                        if speech_dur >= VAD_MIN_SPEECH_MS:
                            await process_and_stream_turn(captured)
                        else:
                            logger.debug("Ignored short noise burst (%.0f ms)", speech_dur)

            text_msg = message.get("text")
            if text_msg:
                cmd = text_msg.strip()
                if cmd == "FLUSH" and len(utterance_buf) > 0:
                    captured = bytes(utterance_buf)
                    utterance_buf.clear()
                    is_speaking_user = False
                    await process_and_stream_turn(captured)

    except WebSocketDisconnect:
        logger.info("ESP32-S3 audio terminal disconnected (%s)", client_host)
    except Exception as exc:
        logger.exception("Error in /ws/audio session: %s", exc)


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(
        "bridge_server:app",
        host=BRIDGE_HOST,
        port=BRIDGE_PORT,
        reload=False,
    )
