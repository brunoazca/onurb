# Onurb on a server (Hetzner) — plan

Status (2026-11-10): server up and in front of the watch; the agent still runs on the Mac
(the server proxies to it over Tailscale and answers "Mac off" when it can't). See "Now".
Goal: the watch works anywhere (not only on home Wi-Fi) and Onurb doesn't need the Mac on;
the Mac becomes an optional worker for what only exists there.

## Now

- VPS: Ubuntu 24.04, key-only SSH, ufw (22 only), fail2ban, swap; user `onurb` runs everything.
- Cloudflare tunnel -> `127.0.0.1:8080`; Access service token in front (the watch has it).
- `onurb-gateway` (systemd): server mode. `/v1/*` goes to the Mac's gateway while it answers
  `/v1/health` over Tailscale; otherwise this gateway answers with its own agent
  (`--brain onurb`, Opus, `ONURB_HOST=server`). STT is Scribe, no whisper on the server.
- Computer use: `onurb-display` (Xvfb :1), `onurb-desktop` (openbox + Google Chrome, own
  profile in `~onurb/.config/onurb-chrome`), `onurb-vnc` (x11vnc on localhost:5901 only).
  View/log in: `ssh -L 5901:localhost:5901 <server>` then Screen Sharing to `vnc://localhost:5901`.
- Claude Code logged in for `onurb` (claude.ai account).
- wacli 0.19.0 linked as its own device (QR); `onurb-wacli` runs `wacli sync --follow`, which
  also carries the gateway's sends; reads use `--read-only`. The Mac keeps its own wacli
  while the Mac gateway is the primary (two linked devices).

Remaining (user):
1. ~~Log Claude Code in~~ (done 2026-11-11).
2. ~~Install wacli and link it by QR~~ (done 2026-11-11). Stop the Mac's sync only when the
   server becomes the primary gateway.
3. In the server Chrome (VNC): install Claude in Chrome, sign in, log into the sites that matter.
4. herdr on the Mac from the server: needs Remote Login on the Mac + the server's key.

## Target

```
watch ──HTTPS──▶ Cloudflare (tunnel + Access) ──▶ Hetzner VPS (US, Ashburn)
                                                   ├─ onurb_gateway.py (STT: ElevenLabs Scribe)
                                                   ├─ agent: claude -p (user's login, setup-token)
                                                   ├─ wacli (WhatsApp linked device, its own store)
                                                   ├─ Chrome (virtual display) for computer use
                                                   └─ firmware builds + OTA image
                                                        │ SSH (optional, when the Mac is on)
                                                        ▼
                                                   Mac: herdr agents, Chrome with personal logins,
                                                        Claude Code transcripts
```

- **Hetzner**: a shared-vCPU VPS (2-4 vCPU, 4-8 GB) in Ashburn is enough: the heavy parts
  (STT, the model) are cloud APIs. Encrypted disk, SSH keys only, unattended upgrades, daily
  snapshot of the state directory (wacli store, agent sessions, received/).
- **Cloudflare**: the tunnel gives HTTPS without open ports; Access (service token) in front of
  `/v1/*` so only the watch gets in. `/internal/*` stays localhost-only. Workers/Containers are
  not used as the runtime (no long-lived processes, no WhatsApp session state).

## What has to change

| Piece | Today (Mac) | On the server |
|---|---|---|
| Reply audio | `say` + `afconvert` | **Done**: no TTS by default (empty WAV, text on screen). Only with `X-Onurb-Audio-Out: bt` (future Bluetooth headset; the ESP32-S3 has no Classic BT/A2DP, so this would be a BLE accessory) or `ONURB_TTS=1` |
| Audio conversion for STT | `afconvert` | `ffmpeg` |
| STT fallback | whisper.cpp large-v3-turbo | Scribe only, or a small whisper model as fallback |
| WhatsApp | wacli store on the Mac (+ user's `wacli sync --follow`) | One wacli on the server, re-linked by QR; the Mac's sync stops |
| Contact photos | WhatsApp Desktop cache | `wacli profile picture-info` (no store contention on the server) |
| Agent | `claude -p` on the Mac login | Claude Code on Linux with `claude setup-token`; claude.ai connectors (Calendar, Granola) work the same |
| herdr | local CLI | `herdr --machine mac` over SSH (Mac must be on) |
| Computer use | Claude in Chrome on the user's Chrome | Server Chrome (below) + the Mac's Chrome for personal logins |
| Firmware build/OTA | ESP-IDF on the Mac | ESP-IDF on the server (or CI); same `/v1/firmware/*` |
| Watch config | gateway URL on the LAN, plain HTTP | HTTPS URL + token (Kconfig, sent by OTA before the switch) |

The agent's prompt already says what needs the Mac (`ONURB_HOST=mac|server` in
`gateway/onurb_agent.py`).

## Computer use on the server

Possible: Chrome runs on the server under a virtual display (Xvfb) with a VNC/noVNC view for
the user, with the Claude in Chrome extension, so `--chrome` works as on the Mac. The user logs
into the sites once over VNC; the profile keeps the sessions.

Limits:
- Logins: 2FA and captchas need the user once per site (and again when sessions expire).
- Datacenter IPs: some sites (LinkedIn especially) flag or block logins from cloud IPs; those
  stay on the Mac's Chrome.
- Security: the server then holds logged-in sessions; separate Chrome profile, encrypted disk,
  no password manager on it.
- Alternative without the extension: a Playwright MCP server (scriptable, headless), same login
  limits.

## Steps

1. ~~Reply audio off by default~~ (done).
2. Portable audio conversion (`ffmpeg`), whisper optional.
3. VPS + Cloudflare tunnel/Access; deploy gateway + agent; secrets in `~/.config/onurb/` there.
4. Watch: HTTPS URL + token via OTA, keeping the LAN URL as fallback for one release.
5. Move wacli (re-link by QR), stop the Mac's sync.
6. Mac as worker: SSH key for `herdr --machine`, `ONURB_HOST=server`.
7. Server Chrome + VNC for computer use; log into the sites that matter.
