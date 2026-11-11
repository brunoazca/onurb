#!/usr/bin/env python3
"""Onurb gateway: the Mac-side bridge between the watch and the assistant.

    watch --POST /v1/turns (WAV)--> STT (whisper.cpp, EN/pt-BR)
                                 -> brain: "echo", "instinct" (wacli -> Instinct's WhatsApp) or "onurb" (own agent,
   claude -p, docs/AGENT_B.md; routes email etc. to Instinct; WhatsApp only after
   the user confirms the preview on the watch)
                                 -> TTS (macOS `say`) -> 24 kHz s16 mono WAV --> watch

Python 3.9+, standard library only. Implements the synchronous form of the
Turn API (docs/SPEC.md §9): POST returns 200 audio/wav. Recordings, transcripts
and replies are kept under gateway/received/ (gitignored).

Instinct mode sends "[onurb] <text>" from the user's own WhatsApp account
(via the already-running `wacli sync --follow`) and polls wacli's local store,
read-only, for Instinct's reply; the reply ends after REPLY_QUIET_S without a
new message (docs/AGENT.md §4.1).
"""
import argparse
import atexit
import signal
import array
import math
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import threading
import time
import uuid

import onurb_agent as agent
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECEIVED = HERE / "received"
DEFAULT_MODEL = HERE / "models" / "ggml-large-v3-turbo-q5_0.bin"
MAX_UPLOAD = 64 * 1024 * 1024     # ~22 min at 24 kHz; the watch PSRAM (~1.5 min) is the real limit
WATCH_RATE = 24000

VOICES = {"pt": "Luciana", "en": "Samantha"}
REPLY_QUIET_S = 4.0
REPLY_TIMEOUT_S = 60.0
POLL_S = 1.0

# One turn in flight at a time: replies are matched to turns by order.
class FifoLock:
    """Turns run one at a time, strictly in arrival order (threading.Lock isn't
    FIFO), so Instinct gets the requests in the order they were recorded."""

    def __init__(self):
        self._cv = threading.Condition()
        self._next = 0          # next ticket to serve
        self._issued = 0

    def locked(self):
        with self._cv:
            return self._issued != self._next

    def __enter__(self):
        with self._cv:
            ticket = self._issued
            self._issued += 1
            self._cv.wait_for(lambda: self._next == ticket)

    def __exit__(self, *exc):
        with self._cv:
            self._next += 1
            self._cv.notify_all()


TURN_LOCK = FifoLock()
CANCELLED = set()            # turn ids the watch cancelled (POST /v1/turns/cancel)


def log(msg):
    print(f"[{dt.datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


# --------------------------------------------------------------------------- WAV

def parse_wav(data):
    """Return (rate, channels, bits, pcm_bytes) for a PCM WAV, skipping unknown chunks."""
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    pos, fmt = 12, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag != 1:
                raise ValueError(f"unsupported WAV format tag {tag}")
            fmt = (rate, ch, bits)
        elif cid == b"data":
            if fmt is None:
                raise ValueError("data chunk before fmt chunk")
            return fmt + (body,)
        pos += 8 + size + (size & 1)
    raise ValueError("no data chunk")


def wav_bytes(rate, channels, pcm):
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16) +
            b"data" + struct.pack("<I", len(pcm)) + pcm)


VOICE_DBFS = -45.0       # 10 ms chunk louder than this = speech close to the watch
MIN_VOICED_S = 0.4       # real requests had >= 0.82 s; ambient noise/music <= 0.1 s


def voiced_seconds(wav_path):
    """Seconds of 10 ms chunks above VOICE_DBFS (raw watch audio). Quiet ambient
    sound is otherwise turned into invented text by whisper ("Música Música")."""
    with wave.open(str(wav_path)) as w:
        rate = w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    chunk, limit, n = rate // 100, 32768 * 10 ** (VOICE_DBFS / 20), 0
    for i in range(0, len(a) - chunk + 1, chunk):
        seg = a[i:i + chunk]
        if math.sqrt(sum(x * x for x in seg) / chunk) > limit:
            n += 1
    return n / 100


def to_16k_mono(src, dst):
    """16 kHz mono s16 WAV (STT input): afconvert on macOS, ffmpeg elsewhere."""
    if shutil.which("afconvert"):
        cmd = ["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", str(src), str(dst)]
    else:
        cmd = ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(src), "-ac", "1", "-ar", "16000",
               "-c:a", "pcm_s16le", str(dst)]
    subprocess.run(cmd, check=True, capture_output=True)


def normalize(src, dst, target_dbfs=-22.0, max_gain_db=24.0):
    """Remove DC and raise quiet watch recordings toward target RMS (capped gain,
    hard clip). Returns the gain applied in dB."""
    rate, ch, bits, pcm = parse_wav(Path(src).read_bytes())
    a = array.array("h")
    a.frombytes(pcm)
    if not a:
        Path(dst).write_bytes(Path(src).read_bytes())
        return 0.0
    mean = sum(a) / len(a)
    rms = math.sqrt(sum((x - mean) ** 2 for x in a) / len(a)) or 1.0
    gain = min(10 ** (max_gain_db / 20), (10 ** (target_dbfs / 20)) * 32768 / rms)
    gain = max(gain, 1.0)
    out = array.array("h", (max(-32768, min(32767, int((x - mean) * gain))) for x in a))
    Path(dst).write_bytes(wav_bytes(rate, ch, out.tobytes()))
    return 20 * math.log10(gain)


# --------------------------------------------------------------------------- STT

SUPPORTED_LANGS = ("en", "pt")


WHISPER_PROC = {}            # {"p": Popen} of whisper-server, for a clean self-restart
WHISPER_SERVER = None        # "http://127.0.0.1:<port>" once whisper-server is up (model stays loaded)
LANG_CODES = {"portuguese": "pt", "english": "en", "spanish": "es", "russian": "ru", "italian": "it",
              "french": "fr", "german": "de", "galician": "gl", "catalan": "ca", "japanese": "ja"}


def start_whisper_server(model, port=8178):
    """Keep the model in memory: saves the 0.5-6 s load whisper-cli pays per turn."""
    global WHISPER_SERVER
    proc = subprocess.Popen(["whisper-server", "-m", str(model), "--host", "127.0.0.1", "--port", str(port),
                             "-ac", "768", "-bo", "1"],   # ~15 s window (> watch max 10 s): 2x faster, same accuracy
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    for _ in range(60):
        time.sleep(0.5)
        try:
            urllib.request.urlopen(url + "/", timeout=1)
            WHISPER_SERVER = url
            log(f"whisper-server ready ({url}, pid {proc.pid})")
            return proc
        except urllib.error.HTTPError:
            WHISPER_SERVER = url
            return proc
        except OSError:
            if proc.poll() is not None:
                break
    log("whisper-server unavailable - using whisper-cli per turn")
    return None


# Names the user says that whisper otherwise mishears ("instint", "hoarder", "wacky").
STT_VOCAB = "Onurb, Instinct, herdr, wacli, Blackbone, MyNetwork, WhatsApp, Claude."


def _whisper_server(wav16, lang):
    boundary = uuid.uuid4().hex
    fields = {"response_format": "verbose_json", "language": lang, "temperature": "0.0", "prompt": STT_VOCAB}
    body = b"".join(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode()
                    for k, v in fields.items())
    body += (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="in.wav"\r\n'
             f"Content-Type: audio/wav\r\n\r\n").encode() + Path(wav16).read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request(WHISPER_SERVER + "/inference", data=body,
                                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    with urllib.request.urlopen(req, timeout=300) as r:
        d = json.loads(r.read())
    name = (d.get("language") or "").lower()
    code = LANG_CODES.get(name, name[:2] or "xx")
    text = " ".join((d.get("text") or "").split())
    return text, f"auto-detected language: {code}"


SERVER_MAX_S = 14.0           # whisper-server runs with -ac 768 (~15 s window); longer clips would be cut


def _whisper(whisper_bin, model, wav16, lang):
    with wave.open(str(wav16)) as w:
        seconds = w.getnframes() / w.getframerate()
    if WHISPER_SERVER and seconds <= SERVER_MAX_S:
        try:
            return _whisper_server(wav16, lang)
        except (OSError, ValueError) as e:
            log(f"  whisper-server failed ({e}) - falling back to whisper-cli")
    elif WHISPER_SERVER:
        log(f"  {seconds:.0f} s clip: full-context whisper-cli (no length cap)")
    r = subprocess.run([whisper_bin, "-m", str(model), "-f", str(wav16), "-l", lang, "-nt", "--prompt", STT_VOCAB],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"whisper failed: {r.stderr.strip()[-300:]}")
    text = " ".join(line.strip() for line in r.stdout.splitlines() if line.strip())
    return text, r.stderr


def _eleven_key():
    """ELEVENLABS_API_KEY from the env or ~/.config/onurb/elevenlabs.env (never in the repo)."""
    if os.environ.get("ELEVENLABS_API_KEY"):
        return os.environ["ELEVENLABS_API_KEY"]
    f = Path.home() / ".config" / "onurb" / "elevenlabs.env"
    if f.is_file():
        for line in f.read_text().splitlines():
            if line.startswith("ELEVENLABS_API_KEY="):
                return line.split("=", 1)[1].strip()
    return ""


ELEVEN_KEY = _eleven_key()
SCRIBE_MODEL = os.environ.get("ONURB_SCRIBE_MODEL", "scribe_v2")
# Only rare names: common words as keyterms got appended to unfinished sentences
# ("compatível com o" -> "... com o WhatsApp").
SCRIBE_KEYTERMS = ["Onurb", "Instinct", "herdr", "wacli", "Blackbone", "MyNetwork", "Wispr Flow", "Granola"]
SCRIBE_LANGS = {"por": "pt", "pt": "pt", "eng": "en", "en": "en"}


def _scribe_call(wav16, lang3=None):
    boundary = uuid.uuid4().hex
    fields = [("model_id", SCRIBE_MODEL), ("tag_audio_events", "false")]
    fields += [("keyterms", k) for k in SCRIBE_KEYTERMS]
    if lang3:
        fields.append(("language_code", lang3))
    body = b"".join(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode()
                    for k, v in fields)
    body += (f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="in.wav"\r\n'
             f"Content-Type: audio/wav\r\n\r\n").encode() + Path(wav16).read_bytes() + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request("https://api.elevenlabs.io/v1/speech-to-text", data=body,
                                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}",
                                          "xi-api-key": ELEVEN_KEY})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def _scribe(wav16, fallback_lang="pt"):
    """ElevenLabs Scribe (cloud): far better than local whisper on the user's
    names ("Instinct", "herdr") and short clips. Auto language; anything other
    than pt/en is redone forced to fallback_lang."""
    t0 = time.monotonic()
    d = _scribe_call(wav16)
    lang = SCRIBE_LANGS.get((d.get("language_code") or "").lower())
    if lang is None:
        log(f"  Scribe detected [{d.get('language_code')}], redoing as [{fallback_lang}]")
        lang = fallback_lang
        d = _scribe_call(wav16, "por" if lang == "pt" else "eng")
    text = " ".join((d.get("text") or "").split())
    log(f"  Scribe ({time.monotonic() - t0:.1f} s)")
    return text, lang


def transcribe(wav_path, model, whisper_bin, fallback_lang="pt"):
    """Return (text, language). Auto-detects, but only English and pt-BR are
    spoken to Onurb: short or noisy clips can be misdetected (e.g. as Russian),
    so anything else is re-transcribed in fallback_lang."""
    with tempfile.TemporaryDirectory() as tmp:
        wav16 = Path(tmp) / "in16k.wav"
        loud = Path(tmp) / "norm.wav"
        gain_db = normalize(wav_path, loud)
        if gain_db >= 1.0:
            log(f"  input gain +{gain_db:.0f} dB")
        # whisper.cpp wants 16 kHz mono s16.
        to_16k_mono(loud, wav16)
        if ELEVEN_KEY:
            try:
                return _scribe(wav16, fallback_lang)
            except Exception as e:  # noqa: BLE001 - offline / quota / API change: local whisper
                log(f"  ElevenLabs STT failed ({str(e)[:120]}) - using local whisper")
        text, err = _whisper(whisper_bin, model, wav16, "auto")
        m = re.search(r"auto-detected language:\s*([a-z]{2})", err)
        lang = m.group(1) if m else fallback_lang
        if lang not in SUPPORTED_LANGS:
            log(f"  detected [{lang}], re-transcribing as [{fallback_lang}]")
            lang = fallback_lang
            text, _ = _whisper(whisper_bin, model, wav16, lang)
    return text, lang


# --------------------------------------------------------------------------- TTS

URL_RE = re.compile(r"https?://\S+")
EMOJI_RE = re.compile("[\U0001F000-\U0001FAFF☀-➿️]")


# whisper's usual output for silence / noise; never forward these to the brain
SILENCE_RE = re.compile(r"^\W*(\[[^\]]*\]|\([^)]*\)|obrigad[oa]|thank you|thanks for watching|legendas? .*|tchau|you)?\W*$", re.I)


def is_silence(text):
    return bool(SILENCE_RE.match(text or ""))


def speakable(text):
    """Mechanical cleanup only (docs/AGENT.md §3): no rewriting of content."""
    text = URL_RE.sub("", text)
    text = EMOJI_RE.sub("", text)
    text = re.sub(r"[*_`#>|]+", "", text)
    text = re.sub(r"^\s*[-•]\s+", "", text, flags=re.M)
    return re.sub(r"\s+", " ", text).strip()


def available_voices():
    out = subprocess.run(["say", "-v", "?"], capture_output=True, text=True).stdout
    return {line.split("  ")[0].strip() for line in out.splitlines() if line.strip()}


def empty_wav(rate=None):
    """A valid 0-sample WAV: the watch shows the text and plays nothing."""
    rate = rate or WATCH_RATE
    return struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36, b"WAVE", b"fmt ", 16, 1, 1, rate, rate * 2, 2, 16,
                       b"data", 0)


def synthesize(text, lang, out_wav, voices):
    """Text -> 24 kHz s16 mono WAV for the watch."""
    voice = VOICES.get(lang, VOICES["en"])
    args = ["say"]
    if voice in voices:
        args += ["-v", voice]
    with tempfile.TemporaryDirectory() as tmp:
        aiff = Path(tmp) / "reply.aiff"
        subprocess.run(args + ["-o", str(aiff), text], check=True, capture_output=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{WATCH_RATE}", "-c", "1", str(aiff), str(out_wav)],
                       check=True, capture_output=True)


# --------------------------------------------------------------------------- brains

def brain_echo(text, lang):
    if not text:
        return "Não ouvi nada." if lang == "pt" else "I didn't hear anything."
    return ("Você disse: " if lang == "pt" else "You said: ") + text


def wacli_json(args):
    r = subprocess.run(["wacli"] + args + ["--json"], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"wacli {' '.join(args[:2])} failed: {r.stderr.strip()[-300:]}")
    return json.loads(r.stdout)


def brain_instinct(text, lang, jid, note=""):
    if not text:
        return "Não ouvi nada." if lang == "pt" else "I didn't hear anything."
    started = dt.datetime.now(dt.timezone.utc) - dt.timedelta(seconds=2)
    r = subprocess.run(["wacli", "send", "text", "--to", jid, "--message", f"[onurb] {note}{text}"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"wacli send failed: {r.stderr.strip()[-300:]}")
    log("sent to Instinct, waiting for reply")

    seen, parts, last_new = set(), [], None
    deadline = time.monotonic() + REPLY_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(POLL_S)
        res = wacli_json(["--read-only", "messages", "list", "--chat", jid, "--from-them",
                          "--after", started.strftime("%Y-%m-%dT%H:%M:%SZ"), "--asc", "--limit", "20"])
        for m in (res.get("data") or {}).get("messages") or []:
            if m.get("MsgID") in seen or m.get("FromMe"):
                continue
            seen.add(m.get("MsgID"))
            body = m.get("Text") or m.get("MediaCaption") or ""
            if m.get("MediaType") and not body:
                body = "Enviei algo no seu celular." if lang == "pt" else "I sent something to your phone."
            if body:
                parts.append(body)
                last_new = time.monotonic()
        if last_new and time.monotonic() - last_new >= REPLY_QUIET_S:
            return " ".join(parts)
    if parts:
        return " ".join(parts)
    return ("O Instinct ainda não respondeu." if lang == "pt" else "Instinct hasn't answered yet.")


INSTINCT_FOLLOW_S = 15 * 60
_follow_gen = [0]


def follow_instinct(turn, jid, sent_at, gateway):
    """After route_to_instinct: for INSTINCT_FOLLOW_S, collect Instinct's later
    messages (after its first reply) and pass each batch to the agent, which
    finishes the user's request in that thread (result -> watch inbox). A newer
    route_to_instinct takes over (Instinct's chat has no per-request threads)."""
    _follow_gen[0] += 1
    gen = _follow_gen[0]
    agent.instinct_pending(turn, True)

    def run():
        seen, parts, last_new = set(), [], None
        res = wacli_json(["--read-only", "messages", "list", "--chat", jid, "--from-them",
                          "--after", sent_at.strftime("%Y-%m-%dT%H:%M:%SZ"), "--asc", "--limit", "50"])
        for m in (res.get("data") or {}).get("messages") or []:
            seen.add(m.get("MsgID"))          # the first reply, already returned to the agent
        deadline = time.monotonic() + INSTINCT_FOLLOW_S
        try:
            while time.monotonic() < deadline and gen == _follow_gen[0]:
                time.sleep(5)
                res = wacli_json(["--read-only", "messages", "list", "--chat", jid, "--from-them",
                                  "--after", sent_at.strftime("%Y-%m-%dT%H:%M:%SZ"), "--asc", "--limit", "50"])
                for m in (res.get("data") or {}).get("messages") or []:
                    if m.get("MsgID") in seen or m.get("FromMe"):
                        continue
                    seen.add(m.get("MsgID"))
                    body = m.get("Text") or m.get("MediaCaption") or ""
                    if m.get("MediaType") and not body:
                        body = "(Instinct sent a file/media to the user's phone)"
                    if body:
                        parts.append(body)
                        last_new = time.monotonic()
                if parts and time.monotonic() - last_new >= 10:
                    text, parts = " ".join(parts), []
                    log(f"  Instinct follow-up for {turn[:8]}: {text[:200]!r}")
                    if agent.continue_after_instinct(turn, text, gateway):
                        return                # request finished
            agent.instinct_gave_up(turn, superseded=gen != _follow_gen[0])
        finally:
            agent.instinct_pending(turn, False)

    threading.Thread(target=run, daemon=True, name=f"instinct-{turn[:8]}").start()


UPSTREAM = os.environ.get("ONURB_UPSTREAM", "").rstrip("/")   # server mode: the Mac's gateway
UPSTREAM_UP = threading.Event()


def watch_upstream():
    """Every 10 s: is the Mac's gateway answering? (/v1/health, 2 s timeout)"""
    while True:
        try:
            with urllib.request.urlopen(UPSTREAM + "/v1/health", timeout=2) as r:
                up = r.status == 200
        except OSError:
            up = False
        if up != UPSTREAM_UP.is_set():
            log(f"Mac gateway {'UP - forwarding watch requests to it' if up else 'DOWN - handling requests here'}")
            (UPSTREAM_UP.set if up else UPSTREAM_UP.clear)()
        time.sleep(10)


# --------------------------------------------------------------------------- firmware

def firmware_image(path):
    """The app .bin, only if complete: ESP images end with SHA-256 of the rest
    (also what the watch compares against its running image)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if len(data) < 64 or data[0] != 0xE9 or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        return None
    return data


# --------------------------------------------------------------------------- HTTP

REPLY_TEXT_MAX = 400          # chars shown on the watch


class Handler(BaseHTTPRequestHandler):
    server_version = "OnurbGateway/0.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body=b"", ctype="application/json", headers=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code, reason):
        log(f"-> {code} {reason}")
        self._send(code, json.dumps({"error": reason}).encode())

    def _imu_log(self):
        """Store a tap-to-capture IMU trace (CSV) sent over Wi-Fi (SPEC §7.5)."""
        cfg = self.server.cfg
        if cfg.token and self.headers.get("Authorization") != f"Bearer {cfg.token}":
            return self._error(401, "bad token")
        length = int(self.headers.get("Content-Length") or 0)
        if not 0 < length <= 512 * 1024:
            return self._error(413, f"bad length {length}")
        body = self.rfile.read(length)
        cap = re.sub(r"[^0-9A-Za-z_-]", "", self.headers.get("X-Onurb-Capture", "x"))[:16]
        label = self.headers.get("X-Onurb-Label", "gesture")
        folder = "imu_negative" if label == "other" else "imu"   # unlabelled (old firmware) = gesture
        out = RECEIVED / folder / f"{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}_cap{cap}.csv"
        out.parent.mkdir(exist_ok=True)
        out.write_bytes(body)
        samples = body.count(b"\n")
        log(f"imu capture #{cap} [{label}]: {samples} samples -> {folder}/{out.name}")
        self._send(200, b'{"ok":true}')

    def _gesture_feedback(self):
        """Store a trained-gesture match + the user's green/red verdict with its
        raw IMU segment, for retraining (tools/gesture_train.py)."""
        cfg = self.server.cfg
        if cfg.token and self.headers.get("Authorization") != f"Bearer {cfg.token}":
            return self._error(401, "bad token")
        length = int(self.headers.get("Content-Length") or 0)
        if not 0 < length <= 512 * 1024:
            return self._error(413, f"bad length {length}")
        body = self.rfile.read(length)
        label = self.headers.get("X-Onurb-Label", "")
        if label not in ("correct", "wrong", "timeout", "rejected", "accepted"):
            return self._error(400, f"bad label {label!r}")
        detail = re.sub(r"[^0-9A-Za-z=;./-]", "", self.headers.get("X-Onurb-Detail", ""))[:96]
        out = RECEIVED / "gesture_feedback" / f"{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}_{label}.csv"
        out.parent.mkdir(exist_ok=True)
        if label == "wrong":   # same window posted earlier as "accepted" (dismissed turn): relabel it
            for old_f in sorted(out.parent.glob("*_accepted.csv"))[-30:]:
                if old_f.read_bytes() == body:
                    out = old_f.with_name(old_f.name.replace("_accepted", "_wrong"))
                    old_f.rename(out)
                    old_f.with_suffix(".txt").unlink(missing_ok=True)
                    log(f"  {old_f.name} -> {out.name} (dismissed: false trigger)")
                    break
        out.write_bytes(body)
        (out.with_suffix(".txt")).write_text(f"label: {label}\ndetail: {detail}\n")
        who = {"rejected": "rule rejected", "accepted": "GESTURE -> turn"}.get(label, f"user says {label.upper()}")
        log(f"mouth pose entry ({detail}): {who} -> {out.name}")
        self._send(200, b'{"ok":true}')

    def _speak(self, reply, lang, base, headers=None):
        """200 audio/wav with the reply text for the screen (X-Onurb-Reply-Text).
        Spoken audio only when the watch says it has somewhere to play it
        (X-Onurb-Audio-Out: bt, a Bluetooth headset) or ONURB_TTS=1; otherwise
        an empty WAV (the user reads the screen; no TTS cost, no macOS `say`)."""
        cfg = self.server.cfg
        reply = speakable(reply) or ("Ok." if lang != "pt" else "Certo.")
        want_audio = self.headers.get("X-Onurb-Audio-Out", "").lower() == "bt" or os.environ.get("ONURB_TTS") == "1"
        if want_audio:
            out = base.with_suffix(".reply.wav")
            synthesize(reply, lang, out, cfg.voices)
            if cfg.play_local:
                subprocess.Popen(["afplay", str(out)])
            body = out.read_bytes()
        else:
            body = empty_wav()
        shown = urllib.parse.quote(reply[:REPLY_TEXT_MAX], safe=" ,.;:!?'()-")
        h = {"X-Onurb-Language": lang, "X-Onurb-Reply-Text": shown}
        h.update(headers or {})
        self._send(200, body, "audio/wav", h)

    def _authorized(self):
        cfg = self.server.cfg
        return not cfg.token or self.headers.get("Authorization") == f"Bearer {cfg.token}"

    def _internal(self):
        """Onurb's MCP tools (onurb_mcp.py) -> gateway. Localhost only."""
        if self.client_address[0] not in ("127.0.0.1", "::1"):
            return self._error(403, "internal")
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        cfg, turn = self.server.cfg, body.get("turn", "")
        if self.path == "/internal/instinct":
            log(f"  agent -> Instinct: {body.get('text', '')!r}")
            sent_at = dt.datetime.now(dt.timezone.utc) - dt.timedelta(seconds=2)
            reply = brain_instinct(body.get("text", ""), "pt", cfg.instinct_jid)
            log(f"  Instinct -> agent: {reply!r}")
            # Instinct often acks first ("vou procurar...") and answers minutes later:
            # keep listening and hand the real answer back to the agent's thread.
            follow_instinct(turn, cfg.instinct_jid, sent_at, f"http://127.0.0.1:{cfg.port}")
            return self._send(200, json.dumps({"reply": reply}).encode())
        if self.path == "/internal/propose_whatsapp":
            r = agent.propose_whatsapp(body.get("to", ""), body.get("messages") or [], turn)
            return self._send(200, json.dumps(r).encode())
        if self.path == "/internal/background":
            tid = agent.start_task(body.get("task", ""), turn, f"http://127.0.0.1:{cfg.port}")
            return self._send(200, json.dumps({"task_id": tid}).encode())
        if self.path == "/internal/propose_purchase":
            return self._send(200, json.dumps(agent.propose_purchase(body, turn)).encode())
        if self.path == "/internal/code":
            tid, err = agent.start_code_task(body.get("task", ""), turn, f"http://127.0.0.1:{cfg.port}")
            return self._send(200, json.dumps({"task_id": tid, "error": err}).encode())
        if self.path == "/internal/tasks":
            return self._send(200, json.dumps({"tasks": agent.list_tasks()}).encode())
        if self.path == "/internal/tasks/cancel":
            return self._send(200, json.dumps({"result": agent.cancel_task(body.get("task_id", ""))}).encode())
        self._error(404, "not found")

    def _action(self, method):
        """Watch side of a proposed WhatsApp message: preview, avatar, confirm/cancel/edit."""
        if not self._authorized():
            return self._error(401, "bad token")
        m = re.fullmatch(r"/v1/actions/([0-9a-f]{12})(?:/(avatar|confirm|cancel|edit))?", self.path)
        if not m:
            return self._error(404, "not found")
        aid, sub = m.group(1), m.group(2)
        if method == "GET" and sub is None:
            a = agent.action_public(aid)
            return self._send(200, json.dumps(a, ensure_ascii=False).encode()) if a else self._error(404, "no action")
        if method == "GET" and sub == "avatar":
            with agent.LOCK:
                raw = (agent.ACTIONS.get(aid) or {}).get("avatar")
            return self._send(200, raw, "application/octet-stream") if raw else self._error(404, "no avatar")
        if method == "POST" and sub == "confirm":
            ok, detail = agent.confirm(aid)
            return self._send(200 if ok else 409, json.dumps({"ok": ok, "detail": detail}).encode())
        if method == "POST" and sub == "cancel":
            self.rfile.read(int(self.headers.get("Content-Length") or 0))
            return self._send(200, json.dumps({"ok": agent.cancel(aid)}).encode())
        if method == "POST" and sub == "edit":
            cfg = self.server.cfg
            data = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            base = RECEIVED / f"{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}_edit_{aid}"
            base.with_suffix(".in.wav").write_bytes(data)
            if voiced_seconds(base.with_suffix(".in.wav")) < MIN_VOICED_S:
                log(f"  edit {aid}: no speech")
                return self._send(204)
            with TURN_LOCK:
                text, lang = transcribe(base.with_suffix(".in.wav"), cfg.model, cfg.whisper, cfg.fallback_lang)
                log(f"  edit {aid}: heard [{lang}] {text!r}")
                if is_silence(text):
                    return self._send(204)
                turn = uuid.uuid4().hex
                reply, new = agent.run_edit(aid, text, turn, f"http://127.0.0.1:{cfg.port}")
            log(f"  edit {aid}: reply {reply!r}, new action {new}")
            return self._speak(reply, lang, base, {"X-Onurb-Action": new} if new else None)
        self._error(405, "method")

    def _inbox(self):
        """Background-task results waiting for the watch (polled when idle)."""
        if not self._authorized():
            return self._error(401, "bad token")
        item = agent.inbox_pop()
        if item is None:
            return self._send(204)
        text, lang, tid, thread, action = item
        log(f"inbox -> watch (task {tid}" + (f", thread {thread[:8]}" if thread else "") + f"): {text!r}")
        base = RECEIVED / f"{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}_task_{tid}"
        st = agent.THREADS.get(thread, {}).get("state", "done")
        h = {"X-Onurb-Thread": thread, "X-Onurb-Thread-State": st} if thread else {}
        if action:
            h["X-Onurb-Action"] = action             # WhatsApp preview proposed after the turn
        self._speak(text, lang, base, h or None)

    def _cancel(self):
        """The user cancelled while the turn was processing. Not under TURN_LOCK:
        it must land while that turn's handler is running."""
        cfg = self.server.cfg
        if cfg.token and self.headers.get("Authorization") != f"Bearer {cfg.token}":
            return self._error(401, "bad token")
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        turn = re.sub(r"[^0-9a-f]", "", self.headers.get("X-Onurb-Turn-Id", ""))[:32]
        if turn:
            CANCELLED.add(turn)
            log(f"turn {turn[:8]}: CANCEL requested by the watch")
        self._send(200, b'{"ok":true}')

    def _proxied(self, method):
        """Server mode (docs/SERVER.md): the Mac's gateway handles watch requests
        while it's up (ONURB_UPSTREAM, over Tailscale); this one only when it isn't.
        Returns True if the request was answered by the upstream."""
        if not UPSTREAM or not UPSTREAM_UP.is_set() or not self.path.startswith("/v1/"):
            return False
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if method == "POST" and n else None
        hop = {"host", "content-length", "connection", "cf-access-client-id", "cf-access-client-secret",
               "cf-access-jwt-assertion", "cf-connecting-ip", "cf-ray", "cf-ipcountry", "cf-visitor", "cdn-loop",
               "x-forwarded-for", "x-forwarded-proto"}
        hdrs = {k: v for k, v in self.headers.items() if k.lower() not in hop}
        req = urllib.request.Request(UPSTREAM + self.path, data=body, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=320) as r:
                status, rh, data = r.status, r.headers, r.read()
        except urllib.error.HTTPError as e:
            status, rh, data = e.code, e.headers, e.read()
        except OSError as e:
            if body is not None:          # the body is consumed: can't fall back for this one
                log(f"upstream (Mac) failed mid-request {self.path}: {e}")
                UPSTREAM_UP.clear()
                self._send(502, b'{"error":"upstream failed"}')
                return True
            log(f"upstream (Mac) unreachable ({e}); handling {self.path} here")
            UPSTREAM_UP.clear()
            return False
        self.send_response(status)
        for k, v in rh.items():
            if k.lower() in ("content-type",) or k.lower().startswith("x-onurb-"):
                self.send_header(k, v)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        return True

    def do_GET(self):
        if self.path in ("/healthz", "/v1/health"):
            return self._send(200, b'{"ok":true}')
        if self._proxied("GET"):
            return
        if self.path in ("/v1/firmware/info", "/v1/firmware/image"):
            return self._firmware()
        if self.path.startswith("/v1/actions/"):
            return self._action("GET")
        if self.path == "/v1/inbox":
            return self._inbox()
        self._error(404, "not found")

    def _firmware(self):
        """Wi-Fi updates (firmware/main/ota.c): serve the latest built image."""
        cfg = self.server.cfg
        if cfg.token and self.headers.get("Authorization") != f"Bearer {cfg.token}":
            return self._error(401, "bad token")
        image = firmware_image(cfg.firmware)
        if image is None:
            return self._error(404, "no complete firmware image")
        digest = image[-32:].hex()
        running = re.sub(r"[^0-9a-f]", "", self.headers.get("X-Onurb-Running", ""))[:64]
        status = re.sub(r"[^0-9A-Za-z_=;.]", "", self.headers.get("X-Onurb-Status", ""))[:256]
        boot = re.sub(r";up=\d+s|;batt=-?\d+|;mv=\d+|;chg=\d|;usb=\d|;jobs=\d+|;spool=\d+|;scr_\w+=\d+|;iram_kb=\d+|;iblk_kb=\d+", "", status)   # changes every check
        b = dict(re.findall(r"(batt|mv|chg|usb|jobs|spool)=(-?\d+)", status))
        if b:   # battery log (power work, 2026-11-10): drain per hour awake vs dozing
            upm = re.search(r"up=(\d+)s", status)
            bat = RECEIVED / "battery.csv"
            new = not bat.exists()
            with open(bat, "a") as f:
                if new:
                    f.write("time,percent,mv,charging,usb,uptime_s\n")
                f.write(f"{dt.datetime.now():%Y-%m-%d %H:%M:%S},{b.get('batt')},{b.get('mv')},{b.get('chg')},"
                        f"{b.get('usb')},{upm.group(1) if upm else ''}\n")
            if b.get("batt") != getattr(self.server, "watch_batt", None):
                self.server.watch_batt = b.get("batt")
                log(f"watch battery {b.get('batt')}% ({b.get('mv')} mV" + (", charging" if b.get("chg") == "1" else "")
                    + f"; {b.get('jobs', '?')} requests in flight, {b.get('spool', '?')} queued offline)")
        scr = dict(re.findall(r"scr_(dtap|rej|hold)=(\d+)", status))
        if scr and scr != getattr(self.server, "watch_scr", None):   # what lights the screen (since boot)
            self.server.watch_scr = scr
            log(f"watch screen since boot: {scr.get('dtap')} double taps, {scr.get('rej')} touches rejected "
                f"(resting/flicker), {scr.get('hold')} lit by the firmware")
        up = re.search(r"up=(\d+)s", status)
        prev_up = getattr(self.server, "watch_up", None)
        if up:
            self.server.watch_up = int(up.group(1))
        if up and prev_up is not None and int(up.group(1)) < prev_up:
            log(f"watch REBOOTED (uptime {prev_up} s -> {up.group(1)} s) [{status}]")
        seen = (running, boot)
        if self.path.endswith("/info") and running and seen != getattr(self.server, "watch_seen", None):
            self.server.watch_seen = seen
            log(f"watch is running {running[:12]}" + (" (up to date)" if running == digest else f", offered {digest[:12]}")
                + (f" [{status}]" if status else ""))
        if self.path.endswith("/info"):
            return self._send(200, json.dumps({"digest": digest, "size": len(image)}).encode())
        log(f"firmware {digest[:12]} ({len(image) // 1024} KB) -> watch")
        self._send(200, image, "application/octet-stream")

    def do_POST(self):
        cfg = self.server.cfg
        if self._proxied("POST"):
            return
        if self.path == "/v1/imu-log":
            return self._imu_log()
        if self.path == "/v1/gesture-feedback":
            return self._gesture_feedback()
        if self.path == "/v1/turns/cancel":
            return self._cancel()
        if self.path.startswith("/internal/"):
            return self._internal()
        if self.path.startswith("/v1/actions/"):
            return self._action("POST")
        if self.path != "/v1/turns":
            return self._error(404, "not found")
        if cfg.token and self.headers.get("Authorization") != f"Bearer {cfg.token}":
            return self._error(401, "bad token")
        length = int(self.headers.get("Content-Length") or 0)
        if not 44 <= length <= MAX_UPLOAD:
            return self._error(413, f"bad length {length}")
        t_up = time.monotonic()
        data = self.rfile.read(length)
        t_up = time.monotonic() - t_up
        try:
            rate, ch, bits, pcm = parse_wav(data)
        except ValueError as e:
            return self._error(415, str(e))
        if bits != 16 or ch != 1:
            return self._error(415, f"need 16-bit mono, got {bits}-bit {ch}ch")

        turn = self.headers.get("X-Onurb-Turn-Id") or uuid.uuid4().hex
        stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
        base = RECEIVED / f"{stamp}_{turn[:8]}"
        base.with_suffix(".in.wav").write_bytes(data)
        queued = self.headers.get("X-Onurb-Queued") == "1"
        note = ""
        if queued:   # recorded while the watch was offline: no audio back, nothing shown
            try:
                at = dt.datetime.fromtimestamp(int(self.headers.get("X-Onurb-Recorded-At", "0")))
            except ValueError:
                at = None
            if at and at.year > 2024:
                note = f"(recorded offline at {at:%H:%M}{'' if at.date() == dt.date.today() else f' on {at:%d/%m}'}, sent late) "
            else:
                note = "(recorded offline, sent late) "
        tilt = self.headers.get("X-Onurb-Tilt", "")
        thread = re.sub(r"[^0-9a-f]", "", self.headers.get("X-Onurb-Thread", ""))[:32] or None
        rec = re.sub(r"[^0-9a-z=;._]", "", self.headers.get("X-Onurb-Rec", ""))[:48]
        log(f"turn {turn[:8]}: {len(pcm) / (2 * rate):.1f} s @ {rate} Hz, body {t_up:.2f} s"
            + (f", tilt {tilt}" if tilt else "") + (f", rec {rec}" if rec else "") + (" [queued]" if queued else ""))

        if TURN_LOCK.locked():
            log(f"  turn {turn[:8]} waits for the previous turn (one at a time keeps replies in order)")
        voiced = voiced_seconds(base.with_suffix(".in.wav"))
        if voiced < MIN_VOICED_S:   # nothing said close to the watch: no STT, nothing to Instinct
            log(f"  only {voiced:.2f} s above {VOICE_DBFS:.0f} dBFS - no speech, dismissed")
            return self._send(204) if not queued else self._send(200, json.dumps({"ok": True, "queued": True}).encode())
        with TURN_LOCK:
            if turn in CANCELLED:   # cancelled while waiting behind another turn
                CANCELLED.discard(turn)
                log(f"  turn {turn[:8]} cancelled while queued - not transcribed, nothing sent")
                return self._send(204)
            try:
                t0 = time.monotonic()
                text, lang = transcribe(base.with_suffix(".in.wav"), cfg.model, cfg.whisper, cfg.fallback_lang)
                t1 = time.monotonic()
                if turn in CANCELLED:
                    CANCELLED.discard(turn)
                    log(f"  cancelled before sending - heard {text!r}, nothing sent to the brain")
                    return self._send(204)
                log(f"  heard [{lang}] ({t1 - t0:.1f} s): {text!r}")
                if is_silence(text):   # nothing said: the watch just goes back to idle, no reply
                    log(f"  no speech ({text!r}) - dismissed, nothing sent to the brain")
                    return self._send(204)
                action = None
                if thread:
                    log(f"  answer in thread {thread[:8]}")
                if cfg.brain == "onurb":
                    reply, action, state = agent.run_turn(text, lang, turn, f"http://127.0.0.1:{cfg.port}",
                                                          note, thread)
                elif cfg.brain == "instinct":
                    reply = brain_instinct(text, lang, cfg.instinct_jid, note)
                    state = "open" if reply.rstrip().endswith("?") else "done"
                elif cfg.brain == "offline":   # server without its own agent yet (docs/SERVER.md)
                    reply = ("O Mac está desligado e o servidor ainda não tem o agente; tente quando o Mac ligar."
                             if lang == "pt" else "The Mac is off and the server has no agent yet; try when the Mac is on.")
                    state = "done"
                else:
                    reply = brain_echo(text, lang)
                    state = "done"
                t2 = time.monotonic()
                if turn in CANCELLED:
                    CANCELLED.discard(turn)
                    log(f"  cancelled after sending - reply dropped: {reply!r}")
                    return self._send(204)
                if queued:
                    log(f"  queued turn {note.strip()}: reply ({t2 - t1:.1f} s) kept on the phone: {reply!r}")
                    base.with_suffix(".txt").write_text(f"lang: {lang}\nqueued: {note}\nheard: {text}\nreply: {reply}\n")
                    return self._send(200, json.dumps({"ok": True, "queued": True}).encode())
                log(f"  reply ({t2 - t1:.1f} s): {reply!r}" + (f" + WhatsApp preview {action}" if action else ""))
                base.with_suffix(".txt").write_text(f"lang: {lang}\nheard: {text}\nreply: {reply}\n")
            except Exception as e:  # noqa: BLE001 - report any pipeline failure to the watch
                return self._error(500, str(e))
        # Reply text for the screen (percent-encoded), the thread (what was heard, its
        # state: done / open = waiting for the user / working = background task) and
        # the pending WhatsApp preview, if any.
        h = {"X-Onurb-Thread": thread or turn, "X-Onurb-Thread-State": state,
             "X-Onurb-Heard": urllib.parse.quote(text[:REPLY_TEXT_MAX], safe=" ,.;:!?'()-")}
        if action:
            h["X-Onurb-Action"] = action
        log(f"  thread {(thread or turn)[:8]}: {state}")
        self._speak(reply, lang, base, h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--brain", choices=["echo", "instinct", "onurb", "offline"], default="echo")
    ap.add_argument("--instinct-jid", default=os.environ.get("ONURB_INSTINCT_JID", ""),
                    help="Instinct's WhatsApp JID (or env ONURB_INSTINCT_JID; never commit it)")
    ap.add_argument("--token", default=os.environ.get("ONURB_API_TOKEN", ""))
    ap.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    ap.add_argument("--whisper", default="whisper-cli")
    ap.add_argument("--fallback-lang", choices=["pt", "en"], default="pt",
                    help="language used when detection returns anything other than en/pt")
    ap.add_argument("--firmware", type=Path, default=HERE.parent / "firmware" / "build" / "onurb.bin",
                    help="app image offered to the watch for Wi-Fi updates")
    ap.add_argument("--play-local", action="store_true", help="also play each reply on this Mac")
    cfg = ap.parse_args()

    if cfg.brain in ("instinct", "onurb") and not cfg.instinct_jid:
        sys.exit(f"--brain {cfg.brain} needs --instinct-jid or ONURB_INSTINCT_JID")
    if not cfg.model.exists():
        if not ELEVEN_KEY:
            sys.exit(f"whisper model not found: {cfg.model} (and no ElevenLabs key for cloud STT)")
        log(f"no whisper model ({cfg.model}): cloud STT only, no local fallback")
        cfg.model = None
    RECEIVED.mkdir(exist_ok=True)
    try:
        cfg.voices = available_voices()       # macOS `say`; absent on a Linux server
    except (OSError, subprocess.SubprocessError):
        cfg.voices = set()

    if cfg.model and shutil.which("whisper-server"):
        wproc = start_whisper_server(cfg.model)
        WHISPER_PROC["p"] = wproc
        if wproc:   # don't leave the model-holding child behind when the gateway is stopped/restarted
            atexit.register(wproc.terminate)
            signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    def restart_self():
        """After a code task changed the gateway's code: re-exec with the same argv/env."""
        if cfg.model and WHISPER_PROC.get("p"):
            WHISPER_PROC["p"].terminate()
        os.execv(sys.executable, [sys.executable] + sys.argv)
    agent.RESTART_HOOK = restart_self
    if UPSTREAM:
        threading.Thread(target=watch_upstream, daemon=True, name="upstream").start()
    srv = ThreadingHTTPServer((cfg.host, cfg.port), Handler)
    srv.cfg = cfg
    log(f"Onurb gateway on {cfg.host}:{cfg.port}, brain={cfg.brain}, play_local={cfg.play_local}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
