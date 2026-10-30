# Onurb — Assistant Backend Architecture

| | |
|---|---|
| **Scope** | The server side of Onurb: what answers when the watch "unlocks a chat", what happens to recordings, and how the assistant is deployed and secured. |
| **Companion doc** | [SPEC.md](SPEC.md): the watch, firmware, and the Turn/Recordings APIs that this backend implements |
| **Status** | 0.2 draft, 2026-10-29. Design only; nothing deployed. |
| **Publication rule** | This repo is public. It names the systems involved (Instinct, herdr, Cloudflare tunnel, wacli) but **never** contains hostnames, IPs, phone numbers, tunnel IDs, hashes, tokens, account numbers or anything else that belongs in `.env`. |

---

## 1. The idea

Pressing the button, or doing the trained gesture, **opens a conversation with my assistant**, in **English or Portuguese**. The assistant does what I ask: mail, calendar, a to-do list, a shopping list ordered on Fridays with my permission, orchestrating coding agents in my herdr workspaces, and more. Recording mode captures meetings and memos, which are **always stored**; the assistant only works on them when I ask.

The watch stays a thin client. It knows only the two HTTP contracts in SPEC §9 and §10.5, so everything in this document can change without reflashing the watch.

---

## 2. Two paths: who owns the brain

The two paths split responsibility differently. They are not two steps of one plan.

| | **Path A: Instinct-routed** | **Path B: Onurb agent** |
|---|---|---|
| Who answers | Instinct | A dedicated Onurb agent |
| How it's reached | wacli sends a message to **Instinct's number** (Instinct has no API) | Directly, in-process |
| Context (conversations, mail, calendar, memory) | **Instinct's job.** Onurb holds none. | Onurb's job |
| Integrations (to-do, grocery, code/agent orchestration, mail, calendar…) | **Instinct's job.** Onurb builds none. | Onurb's job (§6) |
| What Onurb builds | Voice terminal: watch ↔ gateway ↔ wacli, speech in/out, recordings storage | All of Path A, plus the agent, skills, permissions and approvals |
| wacli's role | Transport to Instinct only | Context source (my conversations with other people, which Instinct can't see) + messaging |

### Decision: start with Path A

If we follow the Instinct integration, **everything passes through Instinct**: context, integrations, and the additional features (grocery, to-do, agent orchestrator). On this path Onurb deliberately builds none of them. They are requests Instinct either handles or doesn't.

Path B stays documented (§6) as the alternative if Path A hits limits: speed, voice suitability, or access to conversation context. Switching is a deliberate change of ownership, not something added on top. Both paths sit behind the same **brain adapter** in the gateway, so switching never touches the watch:

```
handle(session, user_text, language) → stream of events:
    reply_text | reply_audio | later_message(text)
```

```
 Watch ──HTTPS──► Cloudflare tunnel ──► Onurb Gateway ──► brain adapter ──► Path A: wacli ──► Instinct's number
                  (Access service       ├ STT (EN/PT)                    └► Path B: Onurb agent (§6)
                   token auth)          ├ TTS (EN/PT) → 24 kHz s16
                                        ├ sessions + inbox
                                        └ recordings store + transcripts ◄── read API (agent, on request)
```

---

## 3. Onurb Gateway (both paths)

| Responsibility | Notes |
|---|---|
| Serve the Turn API and Recordings API | Exactly as in SPEC §9, §10.5; the mock server is the reference behaviour |
| Speech-to-text | **English and Portuguese**, language detected per turn. It must handle 24 kHz mono WAV and Opus/Ogg chunks. |
| Text-to-speech | One voice per language, chosen by the reply's detected language. Output **must** be resampled to 24 kHz s16 before going to the watch, which does no resampling. |
| Speech cleanup | Mechanical only: strip markdown, emoji and URLs, and say "sent to your phone" for links and images. No rewriting of content. |
| Reply timing | **A reply plays when it is fully generated** (no streaming playback for now). |
| Sessions | Follow-ups within `SESSION_WINDOW` (default 5 min) count as one conversation |
| Inbox | Messages that arrive after a turn ends (late or proactive replies) wait here; the watch says "you have N updates" on the next interaction |
| Recordings store | §5. Deterministic: stores and transcribes everything, summarises nothing on its own. |
| Audit log | Every turn: timestamps, language, brain path, outcome. Never message bodies. |

### 3.1 Deployment

- **Host:** an always-on machine reachable through the existing Cloudflare tunnel (the one Instinct already uses for herdr), on its own route. Hostnames, tunnel identifiers and Instinct's number live only in the gateway's `.env`.
- **Edge auth:** a Cloudflare Access policy on the gateway route that admits only the watch's service token and my own identity (for the admin UI).
- **App auth:** the per-device bearer token from SPEC §9. Revoking either the Access token or the bearer cuts the watch off. That is the **kill switch** for a lost watch.

### 3.2 Changes this requires in SPEC.md (proposed, not yet applied)

1. `ONURB_EXTRA_HEADERS` Kconfig, so the watch can send the Cloudflare Access service-token headers. Values go in local `sdkconfig` only.
2. `X-Onurb-Session-Id` on `POST /v1/turns`.
3. `GET /v1/inbox` + `POST /v1/inbox/{id}/ack`, so the watch can announce and play late messages.

---

## 4. Path A in detail: Instinct via wacli

### 4.1 Turn flow

1. The watch uploads speech → the gateway transcribes it (EN/PT detected).
2. The gateway sends the text **as a message from my account to Instinct's number** using wacli. The exchange therefore also shows up in my chat with Instinct on the phone, which works as a free history.
3. The gateway collects Instinct's reply: every message from Instinct's number after the send.
4. **End of reply:** no new message from Instinct for `REPLY_QUIET_MS` (default 4 s) after at least one message, or `REPLY_TIMEOUT_S` (default 60 s).
5. Text → TTS in the reply's language → 24 kHz → played on the watch. If Instinct replies with a voice note, it is transcoded and played as-is. Images and links are announced as "sent to your phone", since they're already in the chat.
6. On timeout with no reply, the watch says "Instinct hasn't answered yet". Anything that arrives later goes to the inbox.

**One turn in flight at a time.** Correlation is by order: wacli and Instinct give no request IDs that we know of (§9 Q1). A turn sent while another is open is queued.

Optional, off by default: a short prefix such as `[voice]` so Instinct can keep spoken answers brief. It only formats the request and adds no context. Turn it on only if Instinct's replies prove too long to listen to.

### 4.2 What Path A means for features

Requests like "add milk to the shopping list", "what's on my to-do list", "start an agent on the billing bug" or "order the groceries" are **forwarded to Instinct like any other sentence**. Whether they work depends entirely on Instinct. Onurb adds no to-do store, grocery integration, orchestrator or context of its own.

### 4.3 Path A risks

| Risk | Note |
|---|---|
| **The watch speaks with my full authority to Instinct** | Anything said into the watch arrives as a message from me. Onurb can't add permission tiers on this path, so Instinct's own confirmation rules are what protect money, code and messages. Mitigations: the button/gesture requirement, the kill switch, and the optional `[voice]` tag so Instinct *could* treat watch messages more cautiously. |
| Reply correlation by order | A reply Instinct sends late can be attributed to the next turn. The quiet-period and one-in-flight rules reduce this; exact behaviour is measured in G1. |
| Latency | Two speech hops plus messaging plus Instinct's own processing. Measured in G1; no target promised. |
| Automated messaging | wacli drives a messaging account automatically. Check the messaging platform's terms and rate limits for automated clients. |

---

## 5. Meetings and memos: stored deterministically

Recordings are **always stored by Onurb** on both paths, without any agent involved. An agent only does something with them when I ask.

| Step | Who | Deterministic? |
|---|---|---|
| Receive chunks, verify, reassemble into one audio file | Gateway | Yes |
| Transcribe (EN/PT, timestamps, markers from the watch) | Gateway STT | Yes (no agent) |
| Store audio + transcript + metadata (date, duration, kind, markers, language) | Recordings store on the gateway host | Yes |
| Summaries, action items, answers ("what did we decide about X?") | Agent, **only on request** | No |

### 5.1 How an agent reaches recordings (decision pending: §9 Q4)

| Option | How | Trade-off |
|---|---|---|
| **R1 (recommended): store + read API** | The store exposes a read-only `recordings` API on its own Cloudflare tunnel route, behind Access. Instinct already uses the tunnel, so when I ask it about a meeting, it fetches the transcript itself. | Onurb stays the system of record. Instinct needs to be taught to use the endpoint. |
| **R2: forward to the agent** | When a recording completes, the gateway also sends the transcript (as a file) to Instinct's number via wacli, and Instinct stores it. | No change to Instinct needed, but every meeting becomes a large chat message and storage depends on Instinct. The local copy is still kept. |

---

## 6. Path B in detail: the Onurb agent (alternative)

Only relevant if we switch ownership to Onurb. On this path Onurb owns context and integrations, and Instinct is no longer required.

### 6.1 Nature

- **One orchestrator agent** with a consistent personality and memory, calling **skills** (tool bundles + instructions). Sub-agents only for long background jobs such as code orchestration.
- Runs on Claude via the Claude API / Agent SDK, with tools as MCP servers.
- **Model per latency budget:** a fast model for spoken turns and a stronger one for long tasks. Current family IDs are `claude-haiku-5-5`, `claude-sonnet-5-5` and `claude-opus-5-5`; choose by measurement.
- **Context, fetched on demand:** a persistent profile, the conversation with Onurb, mail/calendar items, **my conversations with other people via wacli**, the to-do list, and stored recordings (§5) when relevant.
- Replies in the language I spoke (EN/PT).

### 6.2 Permission tiers

| Tier | Meaning | How it's authorised |
|---|---|---|
| **T0 Read** | Look things up | Automatic |
| **T1 Reversible write** | Low-impact, easy to undo | Automatic, logged, mentioned in the reply |
| **T2 Confirm** | Acts in my name, hard to undo | The watch reads back the action; I confirm with a button press within 10 s |
| **T3 Out-of-band** | Money, credentials, production systems | Approval on the **phone** with the details; never from the watch alone |
| **T4 Never** | Not exposed to the agent at all | — |

### 6.3 Skill catalogue

| Skill | Actions → tier |
|---|---|
| **To-do list (native)** | Add/complete/list/reorder by voice → T1. Onurb's own store is the source of truth. Optional Apple sync (§6.4). |
| **Conversations (wacli)** | Search/summarise my chats → T0 · draft → T1 · send as me → T2 with readback (1:1, known contacts) or T3 (groups, new contacts, money, addresses, attachments) |
| **Calendar** | Read → T0 · create/move my events → T2 · invite others → T2 |
| **Mail** | Search/summarise → T0 · draft → T1 · send → T2 or T3 (new recipients, attachments) |
| **Shopping list** | Add/remove by voice → T1 · Friday: build the cart → T1; place the order → **T3 phone approval** with total and items |
| **Agent orchestrator (herdr)** | Status/logs/read → T0 · start an agent task in a workspace → T2 · merge/deploy/delete/production → T3 or T4 |
| **Recordings** | Search transcripts, summarise, extract action items (on request) → T0/T1 |
| **Passwords / vault** | Reveal → **T4** · use a credential inside a tool → T3, and the agent only handles a reference |
| **Banking** | Read-only balances/transactions → T0 · payments → T3 with cap, or T4 until proven safe |

### 6.4 To-do sync with Apple (optional)

- **iCloud Notes** has no public API. Syncing would need an always-on Mac driving Notes through Shortcuts/AppleScript, writing the list as a note. That works for viewing, but it's fragile for edits made on both sides.
- **Apple Reminders** is a better fit for a to-do list: real items, completion, due dates, lists. It's also reachable from a Mac through Shortcuts/EventKit.
- **Recommendation:** Onurb's store is the source of truth, with one-way or two-way sync to Reminders if Apple sync is wanted. Notes works only as a read-only mirror. Decision pending (§9 Q5).

### 6.5 Passwords and bank accounts

- **One vault, but the agent doesn't hold the keys.** A password manager with service accounts (e.g. 1Password or Bitwarden Secrets Manager), limited to a dedicated `onurb-agent` vault holding only what its tools need.
- Tools fetch secrets at execution time. The model sees references, never values, and values never reach prompts, logs or replies.
- Banking: official read-only access first. Any payment goes through the bank's own approval on the phone on top of T3.

### 6.6 Path B security

| Threat | Control |
|---|---|
| Someone else uses the watch | Device token + Access; T3 needs the phone; kill switch |
| Prompt injection via mail, invites, web, code comments | Untrusted data. After reading it, T2/T3 in that turn need an approval showing the exact action. |
| Inbound messages as an attack channel (anyone can message me) | Chat content via wacli is untrusted data; senders can never instruct the agent |
| Impersonation (sending as me) | T2/T3 only; readback of exact text and recipient; no auto-replies |
| Third-party privacy | Fetch only what's needed; no message bodies in logs; short cache retention; never in this repo or fixtures |
| Secret leakage | Vault references; `.env` never committed; CI secret scan |
| Runaway automation (Friday order) | Scheduled jobs prepare, I approve; spend caps; dry-run mode |

---

## 7. Phases and exit criteria

**Path A (current plan)**

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **G0** | Gateway skeleton (the mock server grown up): EN/PT STT and TTS, sessions, inbox, audit log | Simulator holds a 3-turn spoken conversation in each language with a stub brain |
| **G1** | Path A: wacli → Instinct's number; Cloudflare tunnel route with Access | Real requests answered by Instinct from the watch in EN and PT; correlation errors and latency measured over 50 turns |
| **G2** | Recordings store: reassembly, EN/PT transcripts, metadata, plus R1 read API or R2 forwarding | A 60-min meeting is stored and transcribed with no agent involved; Instinct can answer a question about it when asked |

**Path B (only if we switch)**

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **B1** | Onurb agent: to-do (native), calendar, mail (T0–T2), conversations read-only | Side-by-side comparison with Path A, including questions only conversation context can answer |
| **B2** | Conversation sends with readback; to-do Apple sync | 20 sends with zero wrong-recipient/wrong-text incidents; an injection message from a second phone triggers nothing |
| **B3** | Phone approvals (T3); Friday shopping order | One real order placed only after phone approval |
| **B4** | Agent orchestrator over herdr | Start, monitor and report an agent task by voice; nothing merged without T3 |
| **B5** | Vault; read-only banking | Balance answered; zero secret values in logs (scan) |

---

## 8. Decisions log

| Date | Decision |
|---|---|
| 2026-10-29 | Instinct has no API. Path A reaches it by wacli messaging Instinct's number. |
| 2026-10-29 | On Path A everything passes through Instinct (context and all integrations, including grocery, to-do and agent orchestrator). Onurb builds none of it. |
| 2026-10-29 | Languages: English and Portuguese, detected per turn. |
| 2026-10-29 | Meetings and memos are always stored deterministically; agents act on them only on request. |
| 2026-10-29 | Replies play once fully generated (no streaming playback for now). |
| 2026-10-29 | A native to-do list is wanted (Path B skill), possibly linked to Apple. |

## 9. Open questions

| # | Question |
|---|---|
| Q1 | wacli as transport: can it give message IDs or receipts, does Instinct reply in one message or several, can it send/receive voice notes and files, and what's the round-trip time? This decides how reliable reply correlation is (§4.1). |
| Q2 | Portuguese: Brazilian or European? This picks the TTS voice and STT model. |
| Q3 | Where does the gateway run: the same machine as Instinct, or separately? |
| Q4 | Recordings access: R1 (Onurb stores + read API that Instinct calls) or R2 (also forward transcripts to Instinct to store)? (§5.1) |
| Q5 | To-do Apple link: Reminders (recommended), Notes (read-only mirror), or none? Is an always-on Mac available for the sync? Only relevant on Path B. |
| Q6 | Path B only: mail/calendar providers, grocery service (API or browser automation?), bank(s) and read-only API availability, wacli's history depth and read-only mode as a context source. |
