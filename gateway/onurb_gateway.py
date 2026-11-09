#!/usr/bin/env python3
"""Onurb gateway: the Mac-side bridge between the watch and the assistant.

    watch --POST /v1/turns (WAV)--> STT (whisper.cpp, EN/pt-BR)
                                 -> brain: "echo" or "instinct" (wacli -> Instinct's WhatsApp)
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
import array
import math
import datetime as dt
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECEIVED = HERE / "received"
DEFAULT_MODEL = HERE / "models" / "ggml-large-v3-turbo-q5_0.bin"
MAX_UPLOAD = 4 * 1024 * 1024
WATCH_RATE = 24000

VOICES = {"pt": "Luciana", "en": "Samantha"}
REPLY_QUIET_S = 4.0
REPLY_TIMEOUT_S = 60.0
POLL_S = 1.0

# One turn in flight at a time: replies are matched to turns by order.
TURN_LOCK = threading.Lock()


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


def _whisper(whisper_bin, model, wav16, lang):
    r = subprocess.run([whisper_bin, "-m", str(model), "-f", str(wav16), "-l", lang, "-nt"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"whisper failed: {r.stderr.strip()[-300:]}")
    text = " ".join(line.strip() for line in r.stdout.splitlines() if line.strip())
    return text, r.stderr


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
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", str(loud), str(wav16)],
                       check=True, capture_output=True)
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


def brain_instinct(text, lang, jid):
    if not text:
        return "Não ouvi nada." if lang == "pt" else "I didn't hear anything."
    started = dt.datetime.now(dt.timezone.utc) - dt.timedelta(seconds=2)
    r = subprocess.run(["wacli", "send", "text", "--to", jid, "--message", f"[onurb] {text}"],
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


# --------------------------------------------------------------------------- HTTP

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
        out = RECEIVED / "imu" / f"{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}_cap{cap}.csv"
        out.parent.mkdir(exist_ok=True)
        out.write_bytes(body)
        samples = body.count(b"\n")
        log(f"imu capture #{cap}: {samples} samples -> {out.name}")
        self._send(200, b'{"ok":true}')

    def do_GET(self):
        if self.path == "/healthz":
            self._send(200, b'{"ok":true}')
        else:
            self._error(404, "not found")

    def do_POST(self):
        cfg = self.server.cfg
        if self.path == "/v1/imu-log":
            return self._imu_log()
        if self.path != "/v1/turns":
            return self._error(404, "not found")
        if cfg.token and self.headers.get("Authorization") != f"Bearer {cfg.token}":
            return self._error(401, "bad token")
        length = int(self.headers.get("Content-Length") or 0)
        if not 44 <= length <= MAX_UPLOAD:
            return self._error(413, f"bad length {length}")
        data = self.rfile.read(length)
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
        tilt = self.headers.get("X-Onurb-Tilt", "")
        log(f"turn {turn[:8]}: {len(pcm) / (2 * rate):.1f} s @ {rate} Hz" + (f", tilt {tilt}" if tilt else ""))

        with TURN_LOCK:
            try:
                t0 = time.monotonic()
                text, lang = transcribe(base.with_suffix(".in.wav"), cfg.model, cfg.whisper, cfg.fallback_lang)
                t1 = time.monotonic()
                log(f"  heard [{lang}] ({t1 - t0:.1f} s): {text!r}")
                if cfg.brain == "instinct":
                    reply = brain_instinct(text, lang, cfg.instinct_jid)
                else:
                    reply = brain_echo(text, lang)
                t2 = time.monotonic()
                reply = speakable(reply) or ("Ok." if lang != "pt" else "Certo.")
                log(f"  reply ({t2 - t1:.1f} s): {reply!r}")
                out = base.with_suffix(".reply.wav")
                synthesize(reply, lang, out, cfg.voices)
                t3 = time.monotonic()
                base.with_suffix(".txt").write_text(f"lang: {lang}\nheard: {text}\nreply: {reply}\n")
                log(f"  tts {t3 - t2:.1f} s, total {t3 - t0:.1f} s")
            except Exception as e:  # noqa: BLE001 - report any pipeline failure to the watch
                return self._error(500, str(e))

        body = out.read_bytes()
        if cfg.play_local:
            subprocess.Popen(["afplay", str(out)])
        self._send(200, body, "audio/wav", {"X-Onurb-Language": lang})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--brain", choices=["echo", "instinct"], default="echo")
    ap.add_argument("--instinct-jid", default=os.environ.get("ONURB_INSTINCT_JID", ""),
                    help="Instinct's WhatsApp JID (or env ONURB_INSTINCT_JID; never commit it)")
    ap.add_argument("--token", default=os.environ.get("ONURB_API_TOKEN", ""))
    ap.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    ap.add_argument("--whisper", default="whisper-cli")
    ap.add_argument("--fallback-lang", choices=["pt", "en"], default="pt",
                    help="language used when detection returns anything other than en/pt")
    ap.add_argument("--play-local", action="store_true", help="also play each reply on this Mac")
    cfg = ap.parse_args()

    if cfg.brain == "instinct" and not cfg.instinct_jid:
        sys.exit("--brain instinct needs --instinct-jid or ONURB_INSTINCT_JID")
    if not cfg.model.exists():
        sys.exit(f"whisper model not found: {cfg.model}")
    RECEIVED.mkdir(exist_ok=True)
    cfg.voices = available_voices()

    srv = ThreadingHTTPServer((cfg.host, cfg.port), Handler)
    srv.cfg = cfg
    log(f"Onurb gateway on {cfg.host}:{cfg.port}, brain={cfg.brain}, play_local={cfg.play_local}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
