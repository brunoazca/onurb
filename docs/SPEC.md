# Onurb — Development Specification

| | |
|---|---|
| **System** | Onurb — wrist-worn, push-to-talk voice companion |
| **Target hardware** | Waveshare ESP32-S3-Touch-AMOLED-2.06 smartwatch dev kit |
| **Spec version** | 0.1 (draft) — 2026-10-29 |
| **Status** | Pre-hardware. Hardware not yet purchased. Everything here is either *source-verified*, *compile/mock-testable*, or an explicit *physical acceptance test* still outstanding. |

---

## 0. How to read this document

Every technical claim in this spec carries one of these confidence tags:

| Tag | Meaning |
|---|---|
| **[SRC]** | Verified by reading the pinned official source (see §3.1). |
| **[DOC]** | Taken from official vendor/Espressif/Apple documentation; not verified on our hardware. |
| **[UNVERIFIED]** | Plausible, reported, or inferred — must be confirmed before relying on it. |
| **[DECISION]** | A design choice made by this spec. Can be changed by a spec revision. |
| **[PHYS]** | Can only be confirmed on real hardware; listed in §16 physical acceptance tests. |

Normative keywords **MUST / SHOULD / MAY** are used as in RFC 2119.

---

## 1. Product overview

Onurb is the complete watch system: firmware, audio, connectivity, triggers, UI, the APIs it speaks, and the test backend. It has two user-facing modes that share one audio pipeline and never run at the same time:

- **Assistant mode** (§5–§9): a short, push-to-talk exchange with an assistant that replies with speech.
- **Recording mode** (§10): long, uninterrupted capture of meetings or voice memos. It starts and stops from a button or the screen and uploads for later transcription and summary. Nothing is played back.

Assistant mode works like this:

1. The user triggers a turn — by pressing the **BOOT button**, or (optional) by performing a **personally trained wrist gesture**.
2. The watch **records** speech from its onboard microphones.
3. When the turn ends, the watch **uploads** the recording over Wi-Fi (HTTPS) to a configurable API.
4. The API **asynchronously** produces a spoken reply (WAV/PCM).
5. The watch **plays** the reply through its onboard speaker.

Audio is strictly **half-duplex: record, then play**. Onurb never captures and plays at the same time.

### 1.1 Goals

- G1. Reliable push-to-talk capture of ≥10 s of clear speech.
- G2. A round trip to any HTTP(S) endpoint that implements the Onurb Turn API (§9), testable end-to-end against a bundled mock server.
- G3. Robust on-the-go connectivity through an iPhone Personal Hotspot, with automatic reconnection.
- G4. An optional hands-free trigger based on a user-trained gesture (§7).
- G5. Honest, measured numbers for latency, battery and thermal behaviour before any claims are made.
- G6. Meeting/memo recording of ≥60 min that survives Wi-Fi loss and reboots without losing audio (§10).

### 1.2 Non-goals (v1)

- Full-duplex / barge-in conversation, wake-word detection, on-device speech recognition or synthesis.
- Cellular (no 4G/SIM on this board) and 5 GHz Wi-Fi (ESP32-S3 is 2.4 GHz only).
- A rich touch UI. Assistant mode runs with the display off; recording mode needs only a minimal status/stop screen (§8.4, §10.4).
- On-device transcription or summarisation of recordings (server-side only).
- Water resistance (the kit is **not waterproof** [DOC]).
- All-day battery life claims.

---

## 2. Glossary

| Term | Meaning |
|---|---|
| **Turn** | One record → upload → reply → play cycle. Identified by a UUID `turn_id`. |
| **BSP** | Waveshare Board Support Package component for this board. |
| **TDM slot** | Position of a channel in the ES7210's time-division-multiplexed I2S frame. |
| **Physical mic mask** | ES7210 analog input selection (for gain); a different namespace from TDM slot masks. |
| **Audio session** | Exclusive ownership of I2S + ES8311 + ES7210 + PA by one client. |
| **DTW** | Dynamic Time Warping — the template-matching algorithm for gestures. |
| **Mock server** | Local Python HTTP server implementing the Turn API with canned replies. |
| **Recording** | One recording-mode session (meeting or memo). Identified by `recording_id`. |
| **Chunk** | A self-contained, independently decodable 30 s piece of a recording. |
| **Spool** | On-device flash storage for chunks not yet acknowledged by the server. |
| **Marker** | A user-placed bookmark timestamp inside a recording ("this part matters"). |

---

## 3. Hardware platform

### 3.1 Pinned sources [SRC]

| Item | Pin |
|---|---|
| Official repo | `https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06` @ `30f3b23b3d4bb38d32f7d0cddffb66759668a855` (2026-10-20, "refactor: move factory firmware source to examples") |
| BSP component | `examples/factory/components/waveshare__esp32_s3_touch_amoled_2_06`, version **2.0.0**, upstream `Waveshare-ESP32-components` @ `9daffb7168b3c093a330da700988a22092294eef`, path `bsp/esp32_s3_touch_amoled_2_06` |
| BSP dependencies | `esp_codec_dev ~1.5`, `idf >=5.3` |
| ESP-IDF | Official repo README lists **ESP-IDF v5.5.5**; factory app requires `>=5.5,<6.0`. Onurb pins **v5.5.5**. |
| Reference only | xiaozhi-esp32 board file @ `417f52d7` (`main/boards/waveshare/esp32-s3-touch-amoled-2.06/`). Not a dependency. |

The firmware **MUST** consume the BSP pinned by git commit (component-manager `git` + `path` + `version: <sha>`), not a floating registry range, so that the audited audio code is exactly what gets built. Any BSP bump requires re-running the §6.1 audit.

### 3.2 Board facts

| Subsystem | Detail | Tag |
|---|---|---|
| SoC | ESP32-S3, 240 MHz dual core | [SRC] factory sdkconfig |
| Flash / PSRAM | 16 MB QIO flash; octal PSRAM @ 80 MHz | [SRC] factory sdkconfig |
| Speaker codec | ES8311 (DAC), PA enable on BSP_POWER_AMP_IO = GPIO46, active high | [SRC] |
| Mic ADC | ES7210, 4 TDM slots, 3 mics connected (MIC1 FL, MIC2 FR, MIC3 RE; MIC4 NA) | [SRC] |
| Motion | QMI8658 6-axis IMU (accel + gyro) on the BSP I2C bus | [SRC] repo README + examples |
| PMIC | AXP2101 (battery, charging, internal temp sensor) | [DOC] |
| Display | 2.06" AMOLED (SH8601 driver), capacitive touch FT5x06 | [SRC] |
| Battery | Official FAQ recommends 400 mAh; USB-C charging | [DOC] |
| Haptics | No vibration motor confirmed | [UNVERIFIED] — do not depend on one |
| SD card | BSP exposes `bsp_sdcard_mount()`; the official `06_videoplayer` example plays from SD | [SRC]; slot access inside the watch case is [PHYS] |

### 3.3 Pin map (from BSP header — the only authoritative source) [SRC]

| Signal | GPIO | BSP macro |
|---|---|---|
| I2C SCL / SDA | 14 / 15 | `BSP_I2C_SCL` / `BSP_I2C_SDA` |
| I2S MCLK | 16 | `BSP_I2S_MCLK` |
| I2S BCLK | 41 | `BSP_I2S_SCLK` |
| I2S LRCK/WS | 45 | `BSP_I2S_LCLK` |
| I2S DOUT (→ ES8311) | 40 | `BSP_I2S_DOUT` |
| I2S DIN (← ES7210) | 42 | `BSP_I2S_DSIN` |
| PA enable | 46 | `BSP_POWER_AMP_IO` |
| LCD CS / PCLK / D0–D3 / RST | 12 / 11 / 4–7 / 8 | `BSP_LCD_*` |
| Touch RST / INT | 9 / 38 | `BSP_LCD_TOUCH_*` |
| BOOT button | 0 | not in BSP (`BSP_CAPS_BUTTONS = 0`) |

Firmware **MUST** reference BSP macros, never duplicate these numbers as literals.

**Known documentation conflict [SRC]:** the factory `bsp_extra/README.md` pin table lists MCLK=42, BCLK=9, DOUT=8, DIN=10. This contradicts the BSP header (and GPIO8/9 are LCD/touch reset in the header). The header wins. Notably, that table claims GPIO10 is ES7210 data-in, which conflicts with the claim that **PWR = GPIO10** (§7.1). Resolved only by the schematic + bring-up test B-03.

---

## 4. System architecture

```
                         ┌────────────────────────── Onurb firmware (ESP32-S3) ──────────────────────────┐
  BOOT (GPIO0) ──► input_button ─┐                                                                         │
                                 ├─► event queue ─► controller (state machine §5) ─┬─► audio_manager ──► BSP ─► ES7210 / ES8311 / PA
  QMI8658 IMU ──► input_gesture ─┘        ▲                                         ├─► turn_client ──► esp_http_client (TLS)
                                          │                                         └─► feedback (tones; display later)
                       wifi_manager ──────┘  (connectivity events)                                             │
                                                                                                               │
                         onurb_core (portable C, no IDF deps): wav, tdm, backoff, dtw, vad, fsm tables ───────┘
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                   │ HTTPS  POST /v1/turns (audio/wav)
                                   ▼
             Onurb Turn API (§9) ── real backend (out of scope)  OR  mock_server/ (Python, stdlib only)
```

### 4.1 Firmware components

| Component | Responsibility | IDF deps |
|---|---|---|
| `onurb_core` | Pure C: WAV encode/parse, TDM slot extraction & downmix, backoff schedule, DTW matcher, energy VAD, state-transition table. **Host-compilable and unit-tested.** | none |
| `onurb_audio` | Single audio-resource manager (§6.4). Only component allowed to touch BSP audio. | BSP, esp_codec_dev |
| `onurb_wifi` | STA connect, reconnect with backoff, connectivity events (§8). | esp_wifi, esp_netif |
| `onurb_turn` | Turn API client: streaming upload, sync/async reply, retries (§9). | esp_http_client, esp-tls, crt bundle |
| `onurb_input` | BOOT button driver; QMI8658 sampler + gesture engine (§7). | driver/gpio, i2c (via BSP) |
| `onurb_feedback` | Tones. | onurb_audio |
| `onurb_recorder` | Recording mode: encoder, chunker, flash spool, background chunk uploader (§10). | onurb_audio, onurb_core, LittleFS |
| `onurb_ui` | Minimal LVGL screens: home (Record button), recording (timer, marker count, Stop), screen-sleep policy (§10.4). | BSP display/touch, LVGL |
| `main` | `app_main`, controller task, Kconfig, wiring. | all |

### 4.2 Tasks

| Task | Core | Prio | Stack | Notes |
|---|---|---|---|---|
| controller | 1 | 6 | 4 KB | Owns state machine; consumes event queue. |
| audio | 1 | 8 | 4 KB | Capture loop (10 ms reads) / playback loop. |
| turn_net | 0 | 5 | 8 KB | HTTP(S); TLS buffers in PSRAM. |
| button | 1 | 7 | 2 KB | 10 ms poll, debounce. |
| imu | 1 | 5 | 4 KB | 100 Hz sampling + gesture engine (only if enabled). |
| rec_encode | 1 | 7 | 16 KB | Recording mode only: PCM → encoder → chunk files. |
| rec_upload | 0 | 4 | 8 KB | Drains the spool whenever an IP is available, in either mode. |
| ui | 0 | 3 | 6 KB | LVGL; only runs while the display is awake. |
| Wi-Fi/lwIP | 0 | IDF default | — | Event handlers only post events; no blocking work. |

All inter-task communication goes through the controller's event queue. No component calls another component's blocking API from an event handler.

### 4.3 Memory budget (PSRAM)

| Buffer | Size | Notes |
|---|---|---|
| Capture (mono, 24 kHz, 16-bit) | 48 000 B/s × `ONURB_MAX_RECORD_S` (default 30 s) = 1.44 MB | Allocated once at boot. |
| Reply audio | `ONURB_MAX_REPLY_BYTES` (default 2 MB ≈ 43 s mono) | Allocated once at boot. |
| TDM read scratch | 4 ch × 2 B × 240 frames = 1 920 B | Internal RAM, DMA-capable. |
| Gesture templates | 5 × 200 samples × 6 axes × 2 B = 12 KB | Plus 2.5 s ring buffer (3 KB). |
| TLS | ~40 KB | `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`. |
| Recording PCM ring | 2 s × 48 000 B = 96 KB | Recording mode reuses the capture buffer region; assistant and recording modes never coexist. |

Recording mode never holds a whole recording in RAM. A one-hour raw recording at 48 000 B/s is about 173 MB, more than 20× the PSRAM. It is compressed and spooled to flash in chunks (§10.3).

---

## 5. Device behaviour — state machine

### 5.1 States

| State | Description |
|---|---|
| `BOOT` | Init NVS, BSP I2C, audio session, Wi-Fi, inputs. |
| `IDLE` | Waiting for a trigger. Wi-Fi maintained or dozing per idle policy (§8.3). |
| `ARMED` | (Gesture only) gesture matched; capturing and waiting for speech confirmation (§7.4). |
| `RECORDING` | Capturing to the PSRAM buffer. |
| `UPLOADING` | Waiting for connectivity (bounded) and streaming the WAV. |
| `AWAITING_REPLY` | Waiting for sync body or polling an async job. |
| `PLAYING` | Playing the reply. |
| `TEACH` | Gesture enrollment (§7.3). |
| `REC_LONG` | Recording mode active (§10). Assistant triggers and gesture detection are suspended. |
| `REC_FINALIZING` | Encoder flushing the last chunk, writing the manifest, queueing `complete`. |
| `ERROR` | Plays an error tone with a code, then returns to `IDLE`. |

### 5.2 Transitions

| From | Event | Guard | To | Action |
|---|---|---|---|---|
| IDLE | BTN_DOWN (hold mode) / BTN_TAP (toggle mode) | — | RECORDING | start tone, open capture |
| IDLE | GESTURE_MATCH | gesture enabled | ARMED (speech-confirm on) / RECORDING (off) | start tone, open capture |
| IDLE | BTN_TRIPLE_TAP | gesture enabled | TEACH | — |
| ARMED | VAD_SPEECH | within `ONURB_GESTURE_CONFIRM_MS` | RECORDING | keep pre-roll |
| ARMED | timeout | — | IDLE | discard silently |
| RECORDING | BTN_UP / BTN_TAP / GESTURE_END / VAD end-of-speech (gesture turns) | duration ≥ `ONURB_MIN_RECORD_MS` | UPLOADING | stop tone |
| RECORDING | same | duration < min | IDLE | discard (debounce / accidental) |
| RECORDING | buffer full | — | UPLOADING | "limit" tone |
| UPLOADING | upload ok + 200 audio | — | PLAYING | — |
| UPLOADING | upload ok + 202 | — | AWAITING_REPLY | start polling |
| UPLOADING | no IP within `ONURB_OFFLINE_WAIT_S` | — | ERROR(E_OFFLINE) | — |
| AWAITING_REPLY | 200 audio | — | PLAYING | — |
| AWAITING_REPLY | timeout / 4xx / 5xx | — | ERROR(E_API) | — |
| PLAYING | done | — | IDLE | release PA |
| PLAYING | BTN_DOWN | — | IDLE → RECORDING | stop playback first (no overlap) |
| IDLE | BTN_DOUBLE_TAP / UI_RECORD | spool has ≥ 5 min free | REC_LONG | start tone (before mic opens), create recording |
| IDLE | BTN_DOUBLE_TAP / UI_RECORD | spool too full | ERROR(E_STORAGE) | — |
| REC_LONG | BTN_TAP | — | REC_LONG | add marker (silent, shown on screen) |
| REC_LONG | BTN_DOUBLE_TAP / UI_STOP / max duration / battery ≤ `ONURB_REC_MIN_BATTERY_PCT` / spool full | — | REC_FINALIZING | — |
| REC_FINALIZING | manifest written | — | IDLE | stop tone (after mic closes); upload continues in background |
| BOOT | spool contains an unfinished recording | — | IDLE | recover: close the last valid chunk, mark the recording `interrupted`, queue upload |
| any | fatal audio error | — | ERROR(E_AUDIO) → reinit audio session | in REC_LONG: finalize what is already spooled first |

A recording **MAY** start while offline: the turn is buffered, a reconnect is kicked immediately (§8.2), and the upload proceeds once an IP is obtained, up to `ONURB_OFFLINE_WAIT_S` (default 20 s). Only one turn is ever in flight; there is no persistent queue in v1.

### 5.3 Feedback tones (v1) [DECISION]

| Tone | Meaning |
|---|---|
| Rising two-note | Recording started |
| Falling two-note | Recording stopped, sending |
| Soft tick every 3 s | Still waiting for reply (≥ 3 s) |
| Low triple | Error (count of pulses = error class: 1 offline, 2 API, 3 audio, 4 reply format) |
| Three ascending | Teach mode step / success |

Tones are generated at 24 kHz stereo and played through the audio manager *before* the mic opens or *after* it closes — never overlapping capture.

---

## 6. Audio pipeline

### 6.1 BSP audit findings (pinned commit) [SRC]

- `bsp_audio_init_voice_24k()` configures one I2S controller as:
  - **TX:** standard Philips, 16-bit, **stereo**, 24 000 Hz, MCLK ×256, DIN unused.
  - **RX:** TDM Philips, 16-bit, slots 0–3, `total_slot = 4`, 24 000 Hz, MCLK ×256, `bclk_div = 8`, DOUT unused.
  - TX and RX **share MCLK/BCLK/LRCK**. `bsp_audio_init_tx_std_rx_tdm()` returns `ESP_ERR_INVALID_ARG` if TX and RX sample rates differ.
- **Trap 1 — legacy fallback:** `bsp_audio_codec_speaker_init()` / `bsp_audio_codec_microphone_init()` call `bsp_audio_init(NULL)` if no data interface exists yet. That is the **legacy mono, duplex, 16-bit, 22 050 Hz** standard-I2S mode, and in that mode the ES7210 is created with `mic_selected = 0`. Onurb **MUST** call `bsp_audio_init_voice_24k()` *before* either codec init, and **MUST** assert success.
- **Trap 2 — 16 kHz default in factory glue:** the factory `bsp_extra_codec_init()` opens playback at `CODEC_DEFAULT_SAMPLE_RATE = 16000` on top of the 24 kHz bus. Onurb **MUST NOT** use `bsp_extra` (factory-local code) and **MUST NOT** open any stream at a rate other than the bus rate.
- **TDM slot order** (factory `bsp_extra` README): slot0 = MIC1, slot1 = MIC3/reference, slot2 = MIC2, slot3 = MIC4. [SRC for the claim; PHYS to confirm — test B-02.]
- **Physical mic masks** (BSP header): MIC1_FL = bit0, MIC2_FR = bit1, MIC3_RE = bit2, MIC4_NA = bit3; connected = MIC1|MIC2|MIC3.
- Factory voice clients (Xiaozhi, SpecAnalyzer) open the mic with `channel = 4`, `channel_mask = 0xF` (all slots), 16-bit, 24 kHz, and apply 24 dB ADC gain to the selected physical mics. Speaker is opened stereo, 16-bit, 24 kHz.
- Known integration risk: upstream issues #10 and #20 reported recording/init problems; #20's fix involved frame/channel duplication and a simplified clock; #10 recommended BSP/ESP-IDF init. [DOC] → Onurb uses the BSP init path verbatim and a single session owner.

### 6.2 Capture format [DECISION]

| Parameter | Value |
|---|---|
| Bus | 24 000 Hz, 16-bit (fixed by BSP profile) |
| Device read | 4 interleaved TDM slots, 240 frames (10 ms) per read = 1 920 B |
| Gain | 24 dB on physical MIC1\|MIC2 (configurable, `ONURB_MIC_GAIN_DB`) |
| Channel derivation | `ONURB_MIC_SOURCE`: `MIC1` (slot 0, default) · `MIC2` (slot 2) · `AVG12` ((slot0+slot2)/2, saturating) |
| Upload format | **WAV, PCM s16le, 24 000 Hz, mono** |
| DC removal | Optional 1st-order high-pass (~80 Hz), default on |
| Pre-roll | Gesture turns keep 300 ms before speech onset |

No resampling happens on the device. Any change of rate requires a full `bsp_audio_deinit()` + re-init with matched TX/RX, and is out of scope for v1.

### 6.3 Playback format

- The speaker stream is opened **stereo, 16-bit, 24 000 Hz**.
- Accepted replies: `audio/wav` (RIFF/WAVE, `fmt` PCM tag 1, 16-bit, **24 000 Hz**, 1 or 2 channels; unknown chunks skipped) or `audio/L16;rate=24000;channels=1|2` (raw big-endian per RFC 2586 — converted to LE).
- Mono replies are duplicated to both channels.
- Any other rate, bit depth or codec → error tone class 4 (`E_REPLY_FORMAT`), logged with the parsed header. The request advertises acceptable formats (§9.2) so a compliant server never sends these.
- Volume is configurable (`ONURB_VOLUME`, default 70). The PA is enabled only for the duration of playback/tones.

### 6.4 Audio resource manager (single owner) [DECISION]

- Exactly one module (`onurb_audio`) owns I2S, both codec handles and the PA.
- API: `audio_session_init()`, `audio_capture_start/read/stop()`, `audio_play(buf, len, fmt)`, `audio_tone(id)`, `audio_session_reset()`.
- Internally a mutex + state (`IDLE | CAPTURING | PLAYING`). `CAPTURING` and `PLAYING` are mutually exclusive; a request in the wrong state returns `ESP_ERR_INVALID_STATE` (never blocks, never overlaps).
- Both codec devices are opened once with the factory-proven parameters (§6.1). Capture is gated by whether reads are issued; playback is gated by writes + PA. On any codec error: close both, `bsp_audio_deinit()`, re-run the init sequence (max 3 attempts, then `E_AUDIO` until reboot).

### 6.5 WAV header (upload)

44-byte canonical header: `RIFF`, size, `WAVE`, `fmt ` (16, PCM=1, ch=1, 24000, byte rate 48000, block align 2, bits 16), `data`, size. Built by `onurb_core` and covered by host tests.

---

## 7. Triggers

### 7.1 BOOT button (primary, always available)

| Item | Spec |
|---|---|
| GPIO | `ONURB_BUTTON_GPIO`, default **0 (BOOT)**, active low, internal pull-up |
| Sampling | 10 ms poll, 30 ms debounce |
| Mode | `ONURB_BUTTON_MODE`: **HOLD** (hold-to-talk, default) or **TOGGLE** (tap to start, tap to stop) |
| Minimum turn | `ONURB_MIN_RECORD_MS` = 400 ms (shorter = discarded) |
| Double tap | 2 taps < 250 ms each within 600 ms (and no third) → start/stop **recording mode** (§10) |
| Single tap in recording mode | Adds a marker; never stops the recording (prevents accidental stops) |
| Triple tap | 3 taps < 250 ms each within 1.5 s → TEACH (when gesture enabled) |
| Strapping caveat | GPIO0 is a boot strapping pin: holding it at reset enters download mode. Documented in README; harmless in normal use. [DOC] |

**Not used:** GPIO35 (unverified claim — explicitly excluded). **PWR (claimed GPIO10)** is *not* used in v1: holding PWR > 6 s powers the device off [DOC], and its GPIO routing conflicts with the factory README (§3.3). Revisit after B-03.

Screen-off behaviour: v1 never initialises the display, so the button must work with the screen off by construction; confirmed physically by A-BTN-01.

### 7.2 Trained gesture (optional, `ONURB_GESTURE_ENABLE`, default off)

**What the sensor can and cannot do:** the QMI8658 measures motion and orientation only. There is no proximity sensor, so "watch is close to my mouth" cannot be detected directly. A plain wrist raise is nearly identical to checking the time. Therefore the user **trains a distinctive gesture** (recommended: double wrist flick or twist-twist, then raise).

### 7.3 Enrollment (TEACH)

1. Triple-tap BOOT → teach tone. (The earlier "hold BOOT 3 s" idea conflicts with hold-to-talk, so triple-tap replaces it.)
2. For each of `ONURB_GESTURE_REPS` (default 5) repetitions: cue tone → user performs gesture → the segment is captured automatically (motion-energy onset → settle) or ends at 2 s.
3. Validation: reject a repetition whose DTW distance to the median of the others exceeds 2.5× the median pairwise distance; request a replacement (max 3 extra).
4. Store templates + threshold in NVS (namespace `onurb_gest`, versioned blob with CRC). Success tone.
5. Re-running TEACH overwrites. A long triple-tap cancels.

### 7.4 Detection

| Stage | Spec |
|---|---|
| Sampling | 100 Hz, accel ±8 g, gyro ±512 dps, 6 axes, int16 |
| Pre-processing | Gravity-removed accel via 1st-order high-pass + raw gravity direction; per-axis z-normalisation using enrollment statistics |
| Gate | DTW only runs when gyro magnitude rose above `ONURB_GESTURE_ONSET_DPS` (default 120) and settled within 2 s — keeps CPU cost and false triggers low |
| Matcher | DTW, Sakoe-Chiba band 15 %, 6-D Euclidean local cost; distance = min over templates, length-normalised |
| Threshold | `mean_intra + k·std_intra`, `k = ONURB_GESTURE_K` (default 2.0), computed at enrollment |
| Refractory | 3 s after any trigger or turn |
| Speech confirmation | `ONURB_GESTURE_SPEECH_CONFIRM` (default **on**): open capture immediately (pre-roll kept), require VAD speech within `ONURB_GESTURE_CONFIRM_MS` (1 500 ms) or silently discard |
| End of turn | Gesture-started turns end on VAD end-of-speech (`ONURB_VAD_HANGOVER_MS` = 900 ms), a BOOT tap, or max length |
| VAD (v1) | Energy-based: frame RMS vs adaptive noise floor + 9 dB, 30 ms frames, 3-frame onset. Upgrade path: ESP-SR VAD. |
| Sampling mode | Polling over I2C (QMI8658 interrupt routing to a GPIO is **[UNVERIFIED]**). If an INT pin is confirmed: use wake-on-motion to cut idle power. |

### 7.5 Data-logging mode (future, not required for v1)

`ONURB_IMU_LOG_ENABLE`: streams labelled IMU traces to `POST /v1/imu-log` on the mock server, so a small on-device classifier (Edge Impulse / TFLite Micro) could later replace DTW if accuracy is insufficient. The `onurb_input` interface (`gesture_engine_feed()` → `GESTURE_MATCH` event) is designed so the matcher is swappable.

---

## 8. Connectivity

### 8.1 Wi-Fi station

- 2.4 GHz STA only. SSID/password from Kconfig (`ONURB_WIFI_SSID`, `ONURB_WIFI_PASSWORD`), placeholders by default.
- Auth threshold WPA2-PSK; PMF capable, not required. Country code configurable.
- Target profile: **iPhone Personal Hotspot** with *Maximize Compatibility* enabled (forces 2.4 GHz) [DOC].
- `WIFI_PS_MIN_MODEM` while idle; `WIFI_PS_NONE` during an active turn (latency).

### 8.2 Reconnection (per Espressif station scenarios [DOC])

- On `WIFI_EVENT_STA_DISCONNECTED`: post event, schedule `esp_wifi_connect()` via a one-shot `esp_timer` (never block in the handler).
- Backoff: 1, 2, 4, 8, 16, 32, 60 s (cap), ±20 % jitter; **never gives up**; reset on `IP_EVENT_STA_GOT_IP`.
- `IP_EVENT_STA_LOST_IP` is treated as disconnected for turn purposes.
- **Kick:** a user trigger while disconnected resets backoff and attempts immediately.
- **Socket hygiene:** `onurb_turn` creates a fresh `esp_http_client` per turn and destroys it at the end; an IP change during a turn aborts the transfer and retries once after the new IP (idempotent via `turn_id`, §9.4). No socket survives a reconnect.
- Disconnect reason codes are logged and counted (diagnostics: `reason`, `rssi`, uptime).

### 8.3 Idle policy (to be decided by measurement — A-PWR-02)

| Policy | Behaviour | Expected trade-off |
|---|---|---|
| `STAY` (default) | Remain associated with modem sleep | Lowest latency; higher idle drain; depends on iPhone keeping hotspot up |
| `DOZE` | Disconnect after `ONURB_IDLE_DISCONNECT_S`, reconnect on trigger (record meanwhile) | Lower idle drain; adds reconnect time (seconds) to first turn |

**Open question, not a firmware question:** whether the iPhone keeps offering the hotspot after the phone is idle/locked. Apple: *"When you're not using the hotspot, your devices automatically disconnect to save battery life."* [DOC]. Onurb makes **no promise either way**; measured by A-NET-01/02.

### 8.4 Display

Assistant mode keeps the AMOLED off. Recording mode adds the minimal UI defined in §10.4. A fuller status face (Wi-Fi, battery, pending uploads) is a later milestone and must not change the audio or input contracts.

---

## 9. Onurb Turn API v1

The real backend is out of scope; this contract is what the firmware and mock server implement.

### 9.1 Create a turn

```
POST {ONURB_API_BASE_URL}/v1/turns
Authorization: Bearer {ONURB_API_TOKEN}          (omitted if token empty)
Content-Type: audio/wav
Content-Length: <44 + pcm bytes>
Accept: audio/wav, audio/L16;rate=24000;channels=1, audio/L16;rate=24000;channels=2
X-Onurb-Turn-Id: <uuid v4>                       (idempotency key)
X-Onurb-Device-Id: <efuse MAC, hex>
X-Onurb-Firmware: <semver>+<git sha>
X-Onurb-Trigger: button | gesture
<WAV body: PCM s16le, 24 000 Hz, mono>
```

### 9.2 Responses

| Status | Meaning | Device action |
|---|---|---|
| `200` + `audio/wav` or `audio/L16` | Synchronous reply | Play |
| `202` + `Location: /v1/turns/{id}/reply` + optional `Retry-After` | Accepted, reply pending | Poll |
| `204` | Accepted, no spoken reply | Short ack tone → IDLE |
| `400` / `413` / `415` | Bad/too-large/unsupported audio | ERROR(API) |
| `401` / `403` | Auth | ERROR(API), logged distinctly |
| `429` / `503` | Busy | Retry once honouring `Retry-After` (≤ 10 s), else ERROR |
| other `5xx` / transport failure | — | Retry once (idempotent), else ERROR |

### 9.3 Poll for reply

```
GET {Location}           (same auth headers)
→ 202  still pending (Retry-After honoured; default interval 700 ms)
→ 200  audio body
→ 204  no spoken reply
→ 404/410  expired/unknown → ERROR(API)
```

Total wait bounded by `ONURB_REPLY_TIMEOUT_S` (default 60 s).

### 9.4 Semantics

- `X-Onurb-Turn-Id` makes `POST` idempotent: a server receiving a repeated id **SHOULD** return the existing job instead of creating a new one.
- Max upload: `ONURB_MAX_RECORD_S` × 48 000 + 44 bytes. Max reply: `ONURB_MAX_REPLY_BYTES`; larger → ERROR(reply format) after aborting the read.
- TLS: `https://` uses the ESP-IDF certificate bundle (`esp_crt_bundle_attach`). Plain `http://` is permitted **only** when `ONURB_ALLOW_PLAIN_HTTP=y` (intended for the LAN mock server) and logs a warning at every turn.

---

## 10. Recording mode (meetings and memos)

### 10.1 Behaviour

- **Start:** double-tap BOOT, or tap **Record** on the home screen. A start tone plays *before* the mic opens, so it is not in the recording.
- **During:** the display shows a red dot, elapsed time and marker count, then sleeps after `ONURB_UI_SLEEP_S` (default 10 s). Touching the screen wakes it. A single BOOT tap adds a **marker**. Assistant triggers and gestures are suspended because the audio session is busy.
- **Stop:** double-tap BOOT, or wake the screen and tap **Stop**. The Stop button needs a 1 s press-and-hold so a sleeve brushing the screen can't end a meeting. A stop tone plays *after* the mic closes.
- **Automatic stop:** max duration (`ONURB_REC_MAX_MIN`, default 120), battery at `ONURB_REC_MIN_BATTERY_PCT` (default 10 %), or spool full. Each finalizes cleanly, keeping everything recorded so far.
- **Afterwards:** chunks upload in the background, and the server transcribes and summarises (§10.5). The watch plays nothing back.

### 10.2 Audio and encoding

| Parameter | Value |
|---|---|
| Capture | Same BSP voice profile as assistant mode: 24 kHz, 16-bit, 4 TDM slots (§6.1) |
| Channel | `ONURB_REC_MIC_SOURCE`, default `AVG12` (both front mics; better for a room than one mic) |
| Gain | `ONURB_REC_GAIN_DB`, default 30 dB (far-field); tuned by A-REC-01 |
| Codec | **Opus**, 24 kHz mono, VOIP application, 24 kbit/s, 20 ms frames, in Ogg, via Espressif `esp_audio_codec` [DECISION; availability and CPU cost at 24 kHz are [UNVERIFIED] until M2 builds it] |
| Fallback codec | IMA ADPCM WAV (4:1, 12 kB/s), portable C in `onurb_core`, host-tested |
| Chunking | 30 s per chunk, each a self-contained file (Ogg Opus or ADPCM WAV), so one lost or corrupt chunk never breaks the others |
| Data rate | Opus ≈ 3 kB/s ≈ **11 MB/h**; ADPCM ≈ 12 kB/s ≈ 43 MB/h |

### 10.3 Spool (store-and-forward)

- Every chunk is written to flash **before** any upload attempt. A chunk is deleted only after the server acknowledges it (2xx).
- Storage: a LittleFS partition `spool` of about 8 MB on the 16 MB flash, which holds about **40 min of Opus** (about 10 min of ADPCM). If an SD card is mounted, it is used instead and allows much longer recordings (§3.2).
- Layout: `/spool/<recording_id>/manifest.json` (kind, start time, codec, chunk count, markers, state) plus `NNNNN.ogg`.
- Writes are atomic: each chunk goes to `.tmp`, is fsynced, then renamed. The manifest is rewritten after each chunk. After a power loss, the boot-time recovery (§5.2) keeps every complete chunk.
- When the spool is full while recording, the recording finalizes. When it's full at start, the watch refuses to start if less than 5 min is free.
- Upload policy `ONURB_REC_UPLOAD`:
  - `LIVE` (default): upload while recording whenever Wi-Fi is up.
  - `DEFER`: keep Wi-Fi off during the recording to save battery, then upload afterwards.
- Either way the uploader retries with backoff and resumes after reboot.

### 10.4 Minimal UI

| Screen | Content |
|---|---|
| Home (on wake) | Clock, Wi-Fi/battery icons, big **Record** button, "N recordings pending upload" |
| Recording | Red dot, elapsed time, marker count, **Stop** (hold 1 s) |
| Finalizing | "Saved — uploading in background" for 3 s, then sleep |

The display is off whenever no screen is needed. Touch wake behaviour with the screen off is tested in A-REC-04.

### 10.5 Recordings API v1

```
POST {ONURB_API_BASE_URL}/v1/recordings
  JSON {"recording_id": "<uuid>", "kind": "meeting|memo", "started_at": "<ISO-8601>",
        "codec": "opus/ogg|adpcm/wav", "sample_rate": 24000}
  → 201 (idempotent on recording_id)

PUT  /v1/recordings/{id}/chunks/{seq}            Content-Type: audio/ogg | audio/wav
  X-Onurb-Chunk-Start-Ms: <offset from recording start>
  → 201 / 200 if already stored (idempotent on seq)

POST /v1/recordings/{id}/complete
  JSON {"chunks": N, "duration_ms": D, "markers_ms": [...], "state": "complete|interrupted"}
  → 202 (server transcribes/summarises asynchronously)
```

Same auth, device headers, TLS rules and retry policy as the Turn API (§9). What happens to a finished recording, e.g. a transcript, summary and action items sent to the assistant, is the backend's job and is outside this firmware spec.

### 10.6 Consent

Recording other people is regulated differently by jurisdiction and often requires their consent. Onurb shows a visible recording indicator whenever the screen is awake. Getting consent is the user's responsibility; the firmware does not try to enforce it.

---

## 11. Mock server

Location `mock_server/`. Python ≥ 3.9, **standard library only**.

| Feature | Spec |
|---|---|
| Endpoints | `POST /v1/turns`, `GET /v1/turns/{id}/reply`, `POST /v1/recordings`, `PUT /v1/recordings/{id}/chunks/{seq}`, `POST /v1/recordings/{id}/complete`, `GET /healthz`, `POST /v1/imu-log` (future) |
| Recordings | Stores chunks under `mock_server/received/recordings/<id>/`, and on `complete` concatenates them into one file and writes a JSON manifest with markers. `--drop-chunk-rate` simulates lost acknowledgements to exercise spool retries. |
| Validation | Parses WAV; rejects non-PCM / wrong rate / wrong channels with 415 and a JSON reason |
| Storage | Saves each upload to `mock_server/received/<timestamp>_<turn_id>.wav` (gitignored) — this is how capture quality (A-AUD-01) is reviewed |
| Reply modes | `--reply tone` (canned 24 kHz chime + short phrase-like tone pattern), `--reply echo` (plays your own recording back — doubles as a capture check), `--reply file PATH` (must be 24 kHz s16 WAV) |
| Async | `--mode sync` (200) or `--mode async` (202 + poll, `--delay` seconds) |
| Fault injection | `--token` (require bearer), `--fail-rate 0.2` (random 503), `--latency-ms` |
| Idempotency | Repeated `X-Onurb-Turn-Id` returns the same job |
| Device simulator | `tools/simulate_device.py` — builds a WAV exactly like the firmware (shared test vectors with `onurb_core`) and runs a full turn incl. polling; `--recording 10m` simulates a recording-mode session with chunking, markers, a mid-session disconnect and resume |

---

## 12. Configuration (Kconfig `Onurb` menu)

All environment-specific values are placeholders. **No real secrets, production credentials or production endpoints are ever committed.** Local values go in `firmware/sdkconfig` / `sdkconfig.local` (gitignored) via `idf.py menuconfig`.

| Key | Default | Notes |
|---|---|---|
| `ONURB_WIFI_SSID` | `"CHANGE_ME_SSID"` | |
| `ONURB_WIFI_PASSWORD` | `"CHANGE_ME_PASSWORD"` | |
| `ONURB_WIFI_COUNTRY` | `"01"` | world-safe |
| `ONURB_API_BASE_URL` | `"http://192.168.1.100:8080"` | mock server on LAN |
| `ONURB_API_TOKEN` | `""` | |
| `ONURB_ALLOW_PLAIN_HTTP` | `y` | prototype only; `n` for any real endpoint |
| `ONURB_BUTTON_GPIO` | `0` | |
| `ONURB_BUTTON_MODE` | `HOLD` | `HOLD` / `TOGGLE` |
| `ONURB_MIN_RECORD_MS` | `400` | |
| `ONURB_MAX_RECORD_S` | `30` | |
| `ONURB_MIC_SOURCE` | `MIC1` | `MIC1` / `MIC2` / `AVG12` |
| `ONURB_MIC_GAIN_DB` | `24` | |
| `ONURB_VOLUME` | `70` | 0–100 |
| `ONURB_MAX_REPLY_BYTES` | `2097152` | |
| `ONURB_REPLY_TIMEOUT_S` | `60` | |
| `ONURB_OFFLINE_WAIT_S` | `20` | |
| `ONURB_IDLE_POLICY` | `STAY` | `STAY` / `DOZE` |
| `ONURB_IDLE_DISCONNECT_S` | `300` | DOZE only |
| `ONURB_GESTURE_ENABLE` | `n` | |
| `ONURB_GESTURE_REPS` | `5` | |
| `ONURB_GESTURE_K` | `2.0` (stored ×10) | |
| `ONURB_GESTURE_ONSET_DPS` | `120` | |
| `ONURB_GESTURE_SPEECH_CONFIRM` | `y` | |
| `ONURB_GESTURE_CONFIRM_MS` | `1500` | |
| `ONURB_VAD_HANGOVER_MS` | `900` | |
| `ONURB_IMU_LOG_ENABLE` | `n` | future |
| `ONURB_REC_ENABLE` | `y` | recording mode |
| `ONURB_REC_MAX_MIN` | `120` | |
| `ONURB_REC_MIC_SOURCE` | `AVG12` | |
| `ONURB_REC_GAIN_DB` | `30` | |
| `ONURB_REC_CODEC` | `OPUS` | `OPUS` / `ADPCM` |
| `ONURB_REC_UPLOAD` | `LIVE` | `LIVE` / `DEFER` |
| `ONURB_REC_MIN_BATTERY_PCT` | `10` | needs AXP2101 reading (M5); ignored until then |
| `ONURB_UI_SLEEP_S` | `10` | |

---

## 13. Power and thermal

Source: official Waveshare FAQ [DOC], verified 2026-10-26.

- Recommended cell: 400 mAh. Manufacturer reference runtimes: ~1 h full display, 3–4 h display off, ~6 h low power. **These are not Onurb measurements** and do not reflect the Wi-Fi + audio duty cycle. Onurb makes **no all-day claim**.
- With-battery kit includes a cell (may need fitting); USB-C charging.
- Thermal: AXP2101 internal sensor read 46 °C at 20 °C ambient (Wi-Fi STA+AP, charging, 30 min); 3–4 °C lower discharging. Internal sensor only — not skin/case temperature and not evidence of burn risk.
- Recording mode is the longest continuous load: mic + encoder for up to 2 h, plus Wi-Fi with `LIVE` upload. `DEFER` exists so Wi-Fi can stay off during the meeting. Nothing is promised until A-REC-03 is measured.
- Firmware levers: display off except recording UI, modem sleep while idle, `WIFI_PS_NONE` only during turns, PA off outside playback, IMU off unless gesture enabled, CPU DFS (`CONFIG_PM_ENABLE`, 80–240 MHz) once stable.
- Telemetry (logged per turn): battery voltage/percent and AXP2101 temperature, if AXP2101 access is added (M5).

---

## 14. Security and privacy

- No secrets in git; `sdkconfig`, `sdkconfig.local`, `.env`, `mock_server/received/` are gitignored. CI fails if `sdkconfig.defaults` contains non-placeholder Wi-Fi/API values (simple grep check).
- HTTPS with the certificate bundle for any non-mock endpoint; plain HTTP gated behind an explicit Kconfig flag.
- Bearer token stored in flash (NVS/Kconfig) in plaintext in v1. Upgrade path: flash encryption + NVS encryption before any real-user deployment.
- Recordings live only in RAM on the device and are discarded after upload; nothing is persisted on the watch.
- The mock server stores recordings locally for review; operators must treat them as personal data and delete them after testing.
- Device ID is the eFuse MAC; no other identifiers are sent.
- Recording-mode chunks **are** persisted on flash until acknowledged. That is the one exception to RAM-only audio. Spooled audio is plaintext until flash encryption is enabled, so a lost watch can expose unsent meetings. Acknowledged chunks are deleted immediately.

---

## 15. Repository layout

```
onurb/
├── README.md
├── LICENSE
├── docs/
│   └── SPEC.md                 ← this document
├── firmware/                   ESP-IDF v5.5.5 project
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults      (PSRAM octal, 16 MB flash, mbedTLS ext alloc, placeholders only)
│   ├── partitions.csv
│   ├── main/                   app_main, controller, Kconfig.projbuild, idf_component.yml (BSP pinned by git sha)
│   └── components/
│       ├── onurb_core/         portable C (host-tested)
│       ├── onurb_audio/
│       ├── onurb_wifi/
│       ├── onurb_turn/
│       ├── onurb_input/
│       ├── onurb_feedback/
│       ├── onurb_recorder/
│       └── onurb_ui/
├── host_tests/                 CMake + plain C test runner for onurb_core
├── mock_server/                server.py, assets/, tests/
├── tools/                      simulate_device.py, wav_inspect.py
└── .github/workflows/          ci.yml
```

---

## 16. Verification plan

### 16.1 (a) Compile / mock-tested — runnable without hardware

| ID | Test | How |
|---|---|---|
| C-01 | `onurb_core` unit tests: WAV build/parse (incl. odd chunks, truncation, wrong rate), TDM slot extraction & AVG12 saturation, backoff sequence & jitter bounds, energy VAD on synthetic signals, DTW (identical/time-warped/unrelated traces), threshold calibration, state-transition table | `host_tests/` with system gcc/clang, sanitizers on |
| C-02 | Mock server tests: sync, async+poll, 204, auth failure, 415 on bad WAV, idempotent turn id, fault injection | `python -m unittest` |
| C-03 | Device simulator end-to-end against the mock server, using the same WAV vectors as C-01 | `tools/simulate_device.py` |
| C-04 | **Firmware compiles** under ESP-IDF v5.5.5 for `esp32s3`, with gesture on and off | CI job in `espressif/idf:v5.5.5` container |
| C-05 | Secret-placeholder check on `sdkconfig.defaults` | CI grep |
| C-06 | Recording logic: ADPCM encode/decode round-trip, chunk manifest updates, spool recovery from a truncated `.tmp`, uploader retry/idempotency, double-tap vs triple-tap classification | `host_tests/` |
| C-07 | Recordings API against the mock server, including dropped acknowledgements and resume | simulator `--recording` |

Passing (a) proves logic and buildability only. It does **not** prove audio quality, pin mapping, codec init, Wi-Fi behaviour, latency, battery or gesture accuracy.

### 16.2 Hardware bring-up tests (first power-on)

| ID | Test | Pass criterion |
|---|---|---|
| B-01 | Boot log shows `voice_24k` profile, both codecs created, no legacy 22 050 Hz init | log assertion |
| B-02 | **TDM slot order:** tap/scratch near each mic in turn; dump 4-slot RMS | MIC1→slot0, MIC2→slot2, MIC3→slot1 (or update §6.1) |
| B-03 | PWR key routing vs schematic (GPIO10 conflict) | documented outcome |
| B-04 | QMI8658 present on I2C (address, WHO_AM_I); INT pin routing from schematic | documented outcome |
| B-05 | PA polarity and tone audibility at volume 70 | audible, no clipping |

### 16.3 (b) Physical acceptance tests — outstanding

Pass criteria marked *(proposed)* are initial targets to be confirmed after first measurements.

| ID | Test | Method | Pass criterion |
|---|---|---|---|
| A-AUD-01 | **Clear 10 s capture** | Speak a fixed passage at arm's length / at mouth; review saved WAV on mock server | Intelligible, no dropouts, no clipping at normal voice, no channel-duplication artefacts (issue #20) |
| A-RT-01 | **Mock API round trip + playback** | Mock in `echo` and `tone` modes, sync and async | 20/20 turns play correctly |
| A-NET-01 | **iPhone hotspot reconnect after phone locked 5 min** | Lock phone, wait 5 min, trigger turn | Record outcome; *(proposed)* turn completes ≤ 15 s after trigger |
| A-NET-02 | **iPhone hotspot reconnect after phone locked 30 min** | As above, 30 min | Record outcome honestly; no promise either way |
| A-NET-03 | **Reconnect after range loss** | Walk out of range ≥ 2 min, return | Auto-reconnect without reboot; next turn succeeds |
| A-LAT-01 | **Latency** | Timestamps: release → upload done → first reply byte → playback start; mock with 0 ms delay, LAN and hotspot | Report p50/p95; *(proposed)* release→playback ≤ 2.5 s on LAN with mock |
| A-PWR-01 | **Battery life in record→send→receive→play cycle** | Scripted turn every N minutes from full charge to shutdown, 400 mAh cell | Report hours and turns; no claim beyond measured |
| A-PWR-02 | **Idle trade-offs** | Display off; `STAY` vs `DOZE`; mA idle and reconnect time | Data to choose default idle policy |
| A-THM-01 | **Case/thermal comfort** | Sustained Wi-Fi + audio, charging and discharging; external thermocouple on case back + AXP2101 reading | Report °C; flag if case exceeds comfortable skin contact *(threshold to be set)* |
| A-BTN-01 | **Button behaviour with screen off** | Hold and toggle modes, 20 presses each, screen off | 20/20 correct starts/stops; no spurious turns |
| A-GES-01 | Gesture enrollment on a real wrist | 5-rep teach flow | Completes; templates persist across reboot |
| A-GES-02 | True-detection rate | 50 deliberate gestures | *(proposed)* ≥ 90 % |
| A-GES-03 | False-trigger rate over a normal day | Wear 8 h: drinking, face touching, checking time, walking, typing | *(proposed)* ≤ 1 accepted false turn/day with speech confirmation on |
| A-GES-04 | Left vs right wrist | Enroll and test on each | Report both |
| A-GES-05 | IMU battery cost | Polling vs wake-on-motion (if INT available) | Report mA delta |
| A-REC-01 | **Meeting intelligibility** | 60 min, 4 people, watch on the table and on the wrist; review the transcript | *(proposed)* every speaker intelligible within 2 m; tune gain/mic source |
| A-REC-02 | **Wi-Fi loss during recording** | Drop the hotspot for 10 min mid-meeting | No missing chunks after reconnect |
| A-REC-03 | **Battery for a 60 min recording** | `LIVE` vs `DEFER`, display asleep | Report % used; decide the default upload policy |
| A-REC-04 | **Start/stop with screen off** | Double-tap BOOT and wake+hold Stop, 20× each | 20/20; no accidental stops from sleeve contact |
| A-REC-05 | **Power loss mid-recording** | Pull the battery or force a reboot at minute 20 | All complete chunks recovered and uploaded; recording marked `interrupted` |
| A-REC-06 | **Spool full** | Record with Wi-Fi off until full | Clean finalize and user indication; nothing corrupted |
| A-REC-07 | **Encoder CPU/thermal** | Opus at 24 kHz for 2 h | No buffer overruns (logged), case temperature reported |

---

## 17. Milestones

| ID | Deliverable | Exit criteria |
|---|---|---|
| **M0** | Spec + public repo | This document merged |
| **M1** | `onurb_core` + host tests + mock server + device simulator | C-01, C-02, C-03 green in CI |
| **M2** | Firmware (button trigger, audio manager, Wi-Fi, Turn client, recorder + spool, minimal UI), staged against pinned BSP | C-04…C-07 green in CI |
| **M3** | Hardware bring-up | B-01…B-05 done; A-AUD-01, A-RT-01, A-BTN-01 pass |
| **M4** | Connectivity field tests | A-NET-01…03, A-LAT-01 measured and documented |
| **M5** | Power/thermal + AXP2101 telemetry | A-PWR-01/02, A-THM-01 measured; idle policy default chosen |
| **M6** | Recording mode on hardware | A-REC-01…07 measured |
| **M7** | Trained gesture trigger | A-GES-01…05 measured |
| **M8** (optional) | Fuller status face; IMU data-logging mode | — |

---

## 18. Risks and open questions

| # | Risk / question | Mitigation |
|---|---|---|
| R1 | Recording/init problems reported upstream (#10, #20) | Pinned BSP, exact factory voice parameters, single owner, bring-up test B-01/B-02 |
| R2 | TDM slot order differs from factory README | B-02 measures it; `ONURB_MIC_SOURCE` is configurable |
| R3 | iPhone stops offering hotspot when idle/locked | Out of firmware control; measured; `DOZE` + record-while-offline reduces impact |
| R4 | Battery life too short for practical use | Measure first; display off, modem sleep, DOZE policy |
| R5 | Gesture false triggers | Distinctive trained gesture, onset gate, refractory, speech confirmation |
| R6 | PWR / GPIO10 routing ambiguity | Not used in v1; B-03 |
| R7 | QMI8658 INT not routed → polling cost | A-GES-05; gesture default off |
| R8 | BSP upstream changes break audio | Git-sha pin; re-audit required on bump |
| R10 | Opus encoder unavailable or too slow at 24 kHz on ESP32-S3 | ADPCM fallback (shorter spool time); SD card extends capacity |
| R11 | Flash wear from continuous spooling | LittleFS wear levelling; ≈11 MB/h is far below endurance for normal use; revisit if A-REC shows heavy use |
| R9 | Reply formats from a real backend don't match 24 kHz | Contract in §9 + `Accept` header; reject clearly rather than play garbage |
| Q1 | Final backend and auth scheme | Turn API v1 is backend-agnostic; bearer token placeholder |
| Q3 | Should a finished recording also be sent to the assistant automatically (summary, action items)? | Backend decision; see [AGENT.md](AGENT.md) |
| Q2 | Should replies be streamed and played progressively? | v2 candidate (chunked `audio/L16`); v1 buffers the whole reply |

---

## 19. References

- Official board repo (pinned): https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06/tree/30f3b23b3d4bb38d32f7d0cddffb66759668a855
- Waveshare FAQ (power, thermal): https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.06/FAQ
- Espressif Wi-Fi station scenarios: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi-driver/station-scenarios.html
- Apple — Share your internet connection from iPhone: https://support.apple.com/guide/iphone/share-your-internet-connection-iph45447ca6/ios
- Reference board support (not a dependency): https://github.com/78/xiaozhi-esp32/blob/417f52d7/main/boards/waveshare/esp32-s3-touch-amoled-2.06/esp32-s3-touch-amoled-2.06.cc
- Upstream issues on recording/init: waveshareteam/ESP32-S3-Touch-AMOLED-2.06 #10, #20
