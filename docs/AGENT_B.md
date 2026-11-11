# Onurb agent (Path B) — plan

Status: v0 + WhatsApp approval + background tasks implemented (2026-11-10); see "Implementation". Path A (everything through Instinct) keeps working;
Path B adds Onurb's own agent and lets it hand tasks to Instinct when that is better.

## Goals (from the user)

- Context from the user's Claude chats.
- WhatsApp through wacli (read freely; **send only after approval on the watch**).
- All of the user's email and calendars.
- Instinct-like abilities: deep research, computer use (herdr), etc.
- Route to Instinct when it is the better fit for a task.
- Approval UI on the watch:
  - **WhatsApp**: preview in WhatsApp style — separate bubbles, the contact's circular
    profile picture with the name under it; buttons **Edit** (records audio: "change X"),
    **Confirm**, **Cancel**.
  - **Email**: same pattern, with the email body scrollable.

## Decisions (user, 2026-11-09)

- Runtime: `claude -p` on the user's login.
- **Email → Instinct.** Instinct has the user's personal mailbox, which already receives
  everything from the other accounts, so email tasks are routed to Instinct
  (`route_to_instinct`); Onurb does not connect the other Google accounts. Email sending
  therefore follows Instinct's own confirmation rule; the watch email preview is deferred.
- **No Claude-chats export** for now (not worth it: exports arrive by email, minutes to
  hours later). Local Claude Code transcripts remain available to the agent.
- Calendars: the connected Google account directly; other calendars can be
  shared to it from Google Calendar settings.

## Architecture

```
watch ──audio──▶ gateway ──text──▶ agent (claude -p, headless)
                    ▲                 │ tools: wacli (read-only), Gmail/Calendar/Drive/
                    │                 │ Notion/Granola (claude.ai connectors), web search,
                    │                 │ computer use, route_to_instinct, propose_* (below)
                    │                 ▼
                    └── pending action (WhatsApp / email) ◀── propose_whatsapp / propose_email
watch shows preview ── Confirm ──▶ gateway executes it (agent never sends directly)
                    ── Edit (audio) ──▶ agent revises the draft ──▶ new preview
                    ── Cancel ──▶ dropped
```

- **Runtime**: Claude Code headless (`claude -p`, the user's own login) — already has the
  Gmail, Google Calendar, Drive, Notion and Granola connectors, web search and MCP.
  No API key in the repo.
- **Safety by construction**: the agent runs with `wacli send` and email-send tools
  *disallowed*. It can only *propose* through a small local MCP server in the gateway,
  which stores the pending action. Only a Confirm tap on the watch makes the gateway
  send — the exact previewed content, nothing else. Same rule Instinct agreed to:
  spending money, messaging others, merging/deploying need explicit confirmation.
- **Routing**: a `route_to_instinct(text)` tool sends the request through the existing
  Instinct path (wacli to Instinct's chat) and returns its reply.
- **Context from Claude chats**: claude.ai data export (conversations JSON), indexed
  locally (gitignored) and searchable by the agent. Nothing personal in the repo.
- **Long tasks** (deep research): answered with a short "on it", delivered later
  (phone + a watch inbox; see AGENT.md proposed SPEC changes).

## Watch ↔ gateway protocol (sketch)

- Turn reply may carry `X-Onurb-Action: <id>`; the watch then fetches
  `GET /v1/actions/<id>` → `{kind: "whatsapp"|"email", to: {name, jid|email, avatar_url},
  messages: [..] | {subject, body}}` and the avatar as a small image.
- `POST /v1/actions/<id>/confirm`, `/cancel`, `/edit` (audio body → agent revises,
  returns a new action id).

## Milestones

1. **Agent brain v0** — `--brain onurb`: `claude -p` with read-only tools (wacli read,
   calendar, email read, web search) + `route_to_instinct`; short spoken replies.
2. **WhatsApp approval** — propose tool + pending actions + watch preview
   (bubbles, avatar, name; Edit/Confirm/Cancel).
3. ~~Email approval~~ — email goes through Instinct (see Decisions).
4. ~~Claude-chats export~~ — dropped for now; Claude Code transcripts only.
5. **Deep research / async inbox**, then **computer use (herdr)**.

## Implementation (2026-11-10)

- `gateway/onurb_agent.py`: runs `claude -p` (turns: Opus, resumed session within
  15 min; background tasks: Opus with `--chrome` for computer use; the user wants Opus for both). Read-only
  tools: `wacli --read-only`, calendar, Granola, web, Claude Code transcripts.
  `wacli send` is disallowed.
- `gateway/onurb_mcp.py`: stdio MCP server with `route_to_instinct`, `propose_whatsapp`,
  `start_background_task`, which call the gateway's localhost-only `/internal/*`.
- Gateway (`--brain onurb`): turn replies carry `X-Onurb-Action` when a WhatsApp message
  was proposed. `GET /v1/actions/<id>` (+`/avatar`, 96x96 RGB565), `POST .../confirm`
  (the gateway sends exactly the previewed text), `.../cancel`, `.../edit` (WAV; the agent
  revises and proposes again). `GET /v1/inbox` returns background results as speech.
- Watch: the preview shows after the reply (avatar or initial, bubbles, Edit/Cancel/
  Confirm). Edit records a spoken change (ends on 4 s of silence). An unanswered preview
  comes back after other activity, and is auto-cancelled after 10 min. The inbox is
  polled every 20 s when idle.
- Known gaps: profile photos need `wacli profile picture-info`, which can't run while the
  user's `wacli sync --follow` holds the store, so initials are shown for now. Emoji
  render as boxes in the preview font.

## Code tasks (2026-11-11)

The user: "sim, mas nem me peça permissão. Só no repo do onurb." The agent's `start_code_task`
hands a change to Onurb itself (firmware screens/behavior, gateway, the agent's own prompts) to a
code worker that edits this repo and builds the firmware without asking; a good build is offered
to the watch by OTA, and gateway code changes restart the gateway once the result reached the watch.

- Scope: this repo only (Read/Edit/Write limited to it), `tools/onurb_build.sh`, `python3 -m
  py_compile`, read-only git. Mac only (the toolchain lives there); one at a time.
- No WhatsApp, web or browser in these sessions: nothing read from outside can steer the edits.
- Denied: `firmware/sdkconfig` (secrets), `~/.config`, `gateway/received/`, commit/push/reset/
  checkout, `rm`. Verified that the deny rules block Read and Grep technically.
- The worker follows docs/WATCH_CONSTRAINTS.md before firmware changes.
