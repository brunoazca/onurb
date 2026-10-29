# Task for coding agent (Claude Code) - new isolated herdr workspace

Build a prototype firmware + mock-server project for the Waveshare ESP32-S3-Touch-AMOLED-2.06 smartwatch dev kit. Hardware is NOT purchased yet; this is a compile-and-mock prototype with explicit physical-acceptance tests listed as outstanding.

## Desired device flow (async, not simultaneous audio)
1. User presses/holds a physical button. Documented buttons: BOOT = GPIO0 (preferred), PWR = GPIO10 (note: holding PWR >6 s powers the device off). Do NOT use GPIO35 - that was an unverified claim.
2. Firmware records audio from the onboard mics (ES7210 ADC).
3. On release (or tap-to-start/tap-to-stop), firmware HTTPS-POSTs the recording to a configurable API endpoint over Wi-Fi.
4. API asynchronously returns audio (WAV/PCM).
5. Firmware plays the reply through the onboard speaker (ES8311).

## Technical constraints
- Base: official ESP-IDF + Waveshare BSP from https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06 .
- IMPORTANT audio-pipeline note: do not hardcode mono 16 kHz from an old demo. Audit of the current factory BSP shows ES8311 stereo I2S and ES7210 four-slot TDM sharing clocks (bsp_audio_init_tx_std_rx_tdm, bsp_audio_init_voice_24k); TX/RX rates must match. Inspect and pin the actual official BSP commit/profile and channel layout before any WAV conversion - do not mix a legacy 16 kHz path with the current 24 kHz path. Treat all of this as source orientation and verify against the pinned BSP in the implementation.
- Reference only (not a service requirement): xiaozhi board support for this exact board: https://github.com/78/xiaozhi-esp32/blob/417f52d7/main/boards/waveshare/esp32-s3-touch-amoled-2.06/esp32-s3-touch-amoled-2.06.cc
- Known integration risk (GitHub issues #10 and #20 on the official repo): recording/init problems reported; #20 author fixed recording Apr 30 with frame/channel duplication adjustment and simplified clock; #10 closed recommending BSP/ESP-IDF initialization. So: use official BSP init, a single audio-resource manager, and do NOT assume simultaneous mic capture + speaker playback - the flow is strictly record THEN play.
- Wi-Fi 2.4 GHz only; no 4G/SIM. On-the-go route is iPhone Personal Hotspot (WPA2, 2.4 GHz via "Maximize Compatibility"). Include hotspot reconnect logic.
- All environment-specific values (Wi-Fi SSID/password, API endpoint URL, API auth token) must be config placeholders (Kconfig/menuconfig or a config header). NO real secrets, no prod credentials, no Instinct endpoint.
- Include a mock API server (simple local HTTP server) that receives the POST and returns a canned WAV, so the round trip is testable without the real endpoint.

## Deliverables
- Compilable ESP-IDF firmware project (or clearly staged code if toolchain unavailable) + mock server + README.
- README must clearly separate: (a) what is compile/mock-tested, vs (b) physical acceptance tests still outstanding on real hardware:
  clear 10 s capture; mock API round-trip + playback; iPhone hotspot reconnect after phone locked 5 min and 30 min; reconnect after range loss; latency and battery; button behavior with screen off.

## Wi-Fi reconnect clarification (verified 2026-10-26)
- ESP32 firmware CAN reconnect automatically: handle WIFI_EVENT_STA_DISCONNECTED and call esp_wifi_connect() with retry/backoff, then re-establish the HTTP/HTTPS connection after getting a new IP. Ref: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/wifi-driver/station-scenarios.html
- Implement: saved SSID/password in config, auto-reconnect with backoff, recreate sockets after reconnect.
- The open question is NOT the watch firmware - it is whether the iPhone keeps offering the hotspot after idle/lock. Apple: "When you're not using the hotspot, your devices automatically disconnect to save battery life" (https://support.apple.com/guide/iphone/share-your-internet-connection-iph45447ca6/ios). Do not promise either way; keep it in physical acceptance tests.

## Power and thermal constraints (official FAQ, verified 2026-10-26)
Source: https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.06/FAQ
- Official FAQ recommends a 400 mAh cell; manufacturer reference runtimes: ~1 h full display, 3-4 h display off, ~6 h low power. These are NOT measured runtimes for this project's Wi-Fi + audio protocol. Do not promise all-day use.
- Not waterproof.
- With-battery kit includes a rechargeable cell (may need fitting/connection); USB-C charging.
- Thermal: internal AXP2101 sensor measured 46 C at 20 C ambient with Wi-Fi STA+AP while charging 30 min (3-4 C lower when discharging). Internal sensor only - not skin/case temperature, not evidence of burn risk.
- Add to physical acceptance tests: measure actual battery life in the record->send->receive->play cycle; measure idle sleep tradeoffs (display off, Wi-Fi maintain vs reconnect); measure case/thermal comfort in sustained Wi-Fi + audio use while charging and discharging.

## Optional trigger: user-trained gesture (added 2026-10-29)
Source check: the official repo README lists a QMI8658 six-axis IMU (accelerometer + gyroscope) as the board's motion sensor; Waveshare examples using it: examples/esp-idf/04_Immersive_block, examples/arduino/examples/04_LVGL_QMI8658_ui. It shares the BSP I2C bus. Its interrupt-pin wiring to a GPIO is NOT verified; default to polling until confirmed against the schematic.

- Goal: start recording by performing a user-chosen, distinctive movement instead of (or in addition to) pressing the button. The BOOT button stays as the reliable fallback trigger. Make the gesture trigger a Kconfig option.
- Limitation to state plainly: the IMU senses motion and orientation only, not distance; there is no proximity sensor, so "watch is close to my mouth" cannot be detected directly. A plain wrist raise is too close to checking the time. Recommend a distinctive gesture (e.g. double wrist flick or twist-twist, then raise).
- Approach (no ML pipeline): on-device template enrollment + dynamic time warping (DTW) matching.
  - Teach mode: e.g. hold BOOT ~3 s, audible chime, user performs the gesture ~5 times; store each accel+gyro trace (~1-2 s at ~100 Hz) as templates in NVS/flash. Re-teachable at any time.
  - Detection: sliding-window DTW of live IMU data against the templates; match threshold auto-calibrated from the spread between the enrolled repetitions.
  - Optional (default on): speech confirmation - after a gesture match, keep the recording only if voice activity is detected within ~1.5 s; otherwise discard silently.
  - Feedback: short chime through the speaker before the mic opens (do not assume a vibration motor; not verified). This keeps the strict record-then-play rule.
- Future path, not required now: an IMU data-logging mode that streams labelled traces to the mock server, so a small on-device model (Edge Impulse / TFLite Micro) could be trained later if DTW is not reliable enough.
- Compile/mock-testable: the gesture DTW/classifier code must be portable C with host unit tests using synthetic IMU traces.
- Add to physical acceptance tests: enrollment flow on a real wrist; true-detection rate for the trained gesture; false-trigger rate over a normal day (drinking, face touching, checking the time, walking); left vs right wrist; battery cost of continuous IMU sampling (polling vs wake-on-motion, if an interrupt pin exists).
