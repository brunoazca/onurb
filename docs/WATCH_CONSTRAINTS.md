# Watch constraints (read before changing firmware)

Hard limits of the ESP32-S3-Touch-AMOLED-2.06 build and the mistakes they already caused.
Each item: the rule, then what went wrong when it was ignored.

## Memory

- **Internal RAM is the scarce resource, not PSRAM.** ~512 KB SRAM, most taken by Wi-Fi, the
  display DMA buffer (410x20 px x 2 B = 16 KB), drivers and task stacks. PSRAM (8 MB) has
  plenty free. Check `iram_kb` / `iblk_kb` in the watch status (gateway log, "watch is
  running ... [...]") after any change; keep free internal RAM **> 40 KB** and the largest
  block **> 16 KB**.
  - 2026-11-11: internal RAM down to 18 KB (largest block 10 KB). Effects: `wifi:m f null`
    (Wi-Fi driver malloc failures, "online" but no traffic) and "no memory for the update task".
- **Big buffers go to PSRAM** (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`), never `static`
  arrays or stack arrays. Exceptions that must stay internal: I2S/LCD DMA buffers.
- **LVGL uses the C allocator** (`CONFIG_LV_USE_CLIB_MALLOC=y`), not its builtin 64 KB pool,
  which sat in internal RAM. Wi-Fi/lwIP buffers are allowed in PSRAM
  (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`).
- **Task stacks are internal RAM.** Don't raise a stack "to be safe": every KB comes out of
  the same small pool as Wi-Fi. A task that touches flash (OTA writes, SPIFFS spool) can't
  have its stack in PSRAM.

## Stacks and HTTPS

- **Anything that may talk HTTPS needs ~8 KB of stack.** At home the gateway is plain HTTP
  (light); away from home the watch uses the remote URL through Cloudflare = TLS, whose
  handshake needs several KB of stack. Code paths that worked for weeks at home overflow on
  mobile data.
  - 2026-11-10: the update check ran HTTPS on the **main task (3.5 KB)** -> "stack overflow
    in task main" every ~5 min, reboots on mobile data and mid-turn.
- Current sizes: main 10 KB (gesture/action posts run there), turn/inbox/ota/spool workers
  8 KB, system event task 4 KB (Wi-Fi scan/join handlers run there; 2.3 KB was too small for
  the multi-network code), esp_timer 4 KB.
- **Network work belongs in a worker task**, not the main loop. The update check runs inside
  the `ota` task for this reason.
- **Test both paths** after touching networking: local (home Wi-Fi) and remote (stop the Mac
  gateway for 2 min, or use the phone hotspot). Watch the serial log for `stack overflow`,
  `Guru Meditation`, `m f null`.
- Stack buffers in functions on the main task: keep them small or `static`.

## Power

- Battery is small; the screen and the radio are the costs. Measured 2026-11-10: ~12 %/h with
  the screen held on.
- **Don't light the screen for background work.** Processing keeps it on only 12 s after the
  user speaks; the reply relights it (with haptics). OTA downloads and restarts run dark if
  the screen is off.
- **Wi-Fi dozes** (radio off) after 120 s idle with the screen off and not on USB; peeks for
  the inbox every 10 min. Wakes on gesture, button, screen on or real activity -- **not on
  plain wrist motion** (on the arm it fired constantly; each wake = reconnect + 2 more min of
  radio). Queued offline recordings don't keep the radio on; only an active send does.
- **Update checks: hourly** (`CONFIG_ONURB_OTA_CHECK_MIN=60`), plus one when the user wakes
  the watch / opens the screen, at most every 15 min. Battery telemetry rides on these checks.
- Wi-Fi rescans when no known network is in range back off 30 s -> 5 min.
- Any new periodic network activity must justify its radio time.

## Touch / screen

- The capacitive panel produces phantom contacts (skin, sleeve, charging noise). While dark, a
  shield takes all touches; only a real double tap (each press <= 250 ms, 60-450 ms apart,
  same spot) wakes it. Counters `scr_dtap` / `scr_rej` / `scr_hold` in the status show what
  lit the screen.

## OTA and stuck states

- A firmware bug that blocks OTA can only be fixed by USB or a reboot -- keep the bypasses:
  "busy" for > 15 min checks anyway; a request with no answer for 7 min is dropped.
- A build that crashes on the network path can't download its own fix over that path. When
  in doubt, flash over USB (`idf.py -p /dev/cu.usbmodem* flash`).
- The serial log (`gateway/received/watch_serial.log`, captured while on USB) is the only
  crash record: core dump to flash is off.

## Audio

- The ESP32-S3 has **no Classic Bluetooth** (no A2DP headsets); reply audio is off by default.
- I2S DMA is small (6x240 frames); heavy work in the audio loop stalls recording.
