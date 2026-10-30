# Onurb

**Onurb** is a wrist-worn, push-to-talk voice companion: the complete watch system built on the
[Waveshare ESP32-S3-Touch-AMOLED-2.06](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06) smartwatch dev kit.
It covers the firmware, the audio pipeline, Wi-Fi connectivity, the turn API, triggers, power behaviour, and a mock backend for testing.

Press the BOOT button (or, optionally, perform your own trained wrist gesture). Onurb records your voice, sends it over Wi-Fi to a configurable API, and plays back the spoken reply. Audio is strictly record **then** play.

> **Status: specification stage (M0).** The hardware has not been purchased yet. Nothing here has run on a real watch.
> See [`docs/SPEC.md`](docs/SPEC.md) for the full development specification of the watch, including what can be verified with compile/mock tests and which physical acceptance tests are still outstanding.
> See [`docs/AGENT.md`](docs/AGENT.md) for the assistant backend: what answers when the watch opens a chat, how it's deployed, and how it's secured.
> [`brief.md`](brief.md) is the original task brief the project started from.

## System at a glance

| Part | What it does |
|---|---|
| Firmware (ESP-IDF v5.5.5) | State machine, single audio-resource manager on the pinned Waveshare BSP (24 kHz voice profile), Wi-Fi with auto-reconnect, Turn API client |
| Triggers | BOOT button (hold-to-talk or tap-toggle); optional user-trained gesture via the QMI8658 IMU |
| Recording mode | Double-tap BOOT or tap Record on screen to record meetings or memos; compressed chunks are spooled to flash and uploaded in the background |
| Turn API v1 | `POST /v1/turns` (WAV, 24 kHz mono) → sync `200 audio/wav` or async `202` + poll |
| Mock server | Python stdlib server that returns canned/echo audio so the round trip is testable without a real backend |
| Assistant backend | Gateway (EN/PT speech in and out, sessions, inbox, recordings store) in front of a swappable brain. Path A (current): everything goes to Instinct by wacli messaging its number. Path B (alternative): a dedicated Onurb agent that owns context and integrations. |
| Verification | Host unit tests + mock tests + CI firmware build, then a physical acceptance test plan |

## Roadmap

M0 spec → M1 portable core + mock server → M2 firmware build in CI → M3 hardware bring-up → M4 connectivity field tests → M5 power/thermal → M6 recording mode on hardware → M7 trained gesture. Details are in [SPEC §17](docs/SPEC.md#17-milestones).

## Configuration and secrets

All environment-specific values (Wi-Fi, API URL, token) are placeholders set through `idf.py menuconfig`. Never commit real credentials. `sdkconfig` and local overrides are gitignored.

## License

MIT. See [LICENSE](LICENSE).
