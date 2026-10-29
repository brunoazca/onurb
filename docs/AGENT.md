# Onurb — Assistant Backend Architecture

| | |
|---|---|
| **Scope** | The server side of Onurb: what answers when the watch "unlocks a chat", what happens to recordings, and how the assistant is deployed and secured. |
| **Companion doc** | [SPEC.md](SPEC.md): the watch, firmware, and the Turn/Recordings APIs that this backend implements |
| **Status** | 0.1 draft, 2026-10-29. Design only; nothing deployed. |
| **Publication rule** | This repo is public. It names the systems involved (Instinct, herdr, Cloudflare tunnel, wacli) but **never** contains hostnames, IPs, tunnel IDs, hashes, tokens, account numbers or anything else that belongs in `.env`. |

---

## 1. The idea

Pressing the button, or doing the trained gesture, **opens a conversation with my assistant**. The assistant can act on my behalf: reading mail, managing my calendar, keeping a shopping list and ordering it on Fridays with my permission, orchestrating code work in my herdr workspaces, and more. Recording mode feeds meetings and memos into the same assistant.

**wacli plays two roles.** It is (1) a transport for reaching Instinct, and (2) a context source in its own right: it can read my conversations with other people, which **Instinct does not have**. Questions like "what did Ana say about Saturday?" or "remind me what I promised the contractor" depend on that second role.

The watch stays a thin client. It knows only the two HTTP contracts in SPEC §9 and §10.5, so everything in this document can change without reflashing the watch.

```
 Watch ──HTTPS──► Cloudflare tunnel ──► Onurb Gateway ──► Brain adapter ──► (A) Instinct via wacli
 (Turn API,        (Access service       ├ STT                             └► (B) Onurb Agent ──► skills/tools
                                                                                                ├ conversations (wacli)
  Recordings API)   token auth)          ├ TTS → 24 kHz s16                                     ├ mail, calendar
                                         ├ sessions + inbox                                      ├ shopping list
                                         └ recordings → transcript                               ├ herdr (code)
                                                                                                 ├ vault (secrets by reference)
                                         Approvals ◄──── phone (push / message) ◄───────────────┘ bank (read-only first)
```

---

## 2. Two ways to give Onurb a brain

### Option A: bridge to Instinct through wacli

Instinct is the assistant I already use. It is good at managing things and already reaches herdr through my Cloudflare tunnel. The gateway turns speech into text, sends it to Instinct with wacli, waits for Instinct's reply, and turns that reply into speech for the watch.

| Pros | Cons |
|---|---|
| Fastest path to a working watch-to-assistant loop | Speed and abilities are capped by Instinct and the messaging hop |
| Reuses everything Instinct already does (including herdr access) | Matching a reply to the right watch turn depends on what wacli supports (§8 Q1) |
| | Instinct can't see my conversations with other people, so this option leaves wacli's best context unused |
| No new agent to secure or maintain | Replies are written for chat, not speech; may need rewriting for TTS |

### Option B: a dedicated Onurb agent

A purpose-built agent with its own context and skills (§4). It runs on Claude through the Claude API / Agent SDK, with tools exposed as MCP servers. Instinct becomes **one of its tools** (`ask_instinct`) rather than something to rebuild.

| Pros | Cons |
|---|---|
| Uses wacli directly as a context source: my conversations with other people, which Instinct lacks | More to build, host and secure |
| Voice-first replies, low latency, sessions designed for the watch | Reading other people's messages widens the injection and privacy surface (§5) |
| Full control over context, skills, permissions and audit | Needs a careful security model (§5), especially for money and secrets |
| Can run long tasks and report back through the watch inbox | |

### Decision: A first, then B, behind one interface

The gateway talks to a **Brain adapter** interface:

```
handle(session, user_text, attachments) → stream of events:
    reply_text | task_started(id, label) | task_done(id, summary) | approval_needed(id, what, tier)
```

- **Phase 1:** `InstinctBrain`, which wraps wacli. The watch becomes useful immediately.
- **Phase 2:** `OnurbAgentBrain`, with `ask_instinct` as a tool, so nothing Instinct already does is lost.
- **Phase 3:** move individual capabilities into native Onurb skills only where that's clearly better (latency, voice quality, permissions).

Which brain is used is a gateway setting, so A and B can be compared side by side.

---

## 3. Onurb Gateway (common to both options)

| Responsibility | Notes |
|---|---|
| Serve the Turn API and Recordings API | Exactly as in SPEC §9, §10.5; the mock server is the reference behaviour |
| Speech-to-text | Provider-agnostic interface; it must handle 24 kHz mono WAV and Opus/Ogg chunks |
| Text-to-speech | Output **must** be resampled to 24 kHz s16 (mono or stereo) before going to the watch, which does no resampling |
| Speech shaping | Rewrites brain output for listening: short sentences, no markdown, no URLs read aloud; long content goes to the phone instead |
| Sessions | Follow-ups within `SESSION_WINDOW` (default 5 min) continue the same conversation |
| Inbox | Results of long tasks and approval outcomes wait here; the watch announces "you have N updates" on the next interaction |
| Recordings pipeline | Reassemble chunks → transcript → summary + action items → stored and offered to the brain (§4.3) |
| Audit log | Every brain call and tool call with tier, approver, outcome; retained locally |

### 3.1 Deployment

- **Host:** an always-on machine reachable through the existing Cloudflare tunnel (the same one Instinct uses), or a sibling tunnel route. The hostname and tunnel identifiers live only in the gateway's `.env`.
- **Edge auth:** a Cloudflare Access policy on the gateway route that admits only (a) the watch's service token and (b) my own identity for the admin/approval UI.
- **App auth:** the per-device bearer token from SPEC §9, checked by the gateway. Revoking either the Access token or the bearer cuts the watch off. That is the **kill switch** for a lost watch.
- **Process layout:** gateway, brain adapter, scheduler and MCP tool servers as separate processes or containers, so a compromised tool server can't read the gateway's secrets.

### 3.2 Changes this requires in SPEC.md (proposed, not yet applied)

1. `ONURB_EXTRA_HEADERS` Kconfig, so the watch can send the Cloudflare Access service-token headers. Values go in local `sdkconfig` only.
2. `X-Onurb-Session-Id` on `POST /v1/turns`, so follow-up turns continue a conversation.
3. `GET /v1/inbox` + `POST /v1/inbox/{id}/ack`, so the watch can announce and play pending updates.
4. A `204`-with-header variant: "accepted, I'll tell you when it's done" for long tasks.

---

## 4. The Onurb agent (Option B)

### 4.1 Nature

- **One orchestrator agent** with a consistent personality and memory, calling **skills** (tool bundles + instructions), rather than a crowd of agents. Sub-agents only for long background jobs such as code orchestration.
- **Model choice is per latency budget:**
  - a fast model for spoken turns (target: first audio within a few seconds)
  - a stronger model for planning, long tasks and recording summaries
  - Current Claude family IDs: `claude-haiku-5-5`, `claude-sonnet-5-5`, `claude-opus-5-5`. Choose by measurement, not upfront.
- **Context:** a persistent profile (preferences, people, places), recent conversation with Onurb, relevant mail/calendar items and **my conversations with other people via wacli**, all fetched on demand, plus recording summaries. Together, conversations and meeting recordings give the agent what was actually said and promised, which neither Instinct nor mail alone covers. Fetching on demand beats stuffing everything into every prompt, for both cost and privacy.

### 4.2 Permission tiers

| Tier | Meaning | How it's authorised |
|---|---|---|
| **T0 Read** | Look things up | Automatic |
| **T1 Reversible write** | Low-impact, easy to undo | Automatic, logged, mentioned in the reply |
| **T2 Confirm** | Acts in my name, hard to undo | The watch reads back the action; I confirm with a button press within 10 s |
| **T3 Out-of-band** | Money, credentials, production systems | Approval on the **phone** (push/message with the details); never from the watch alone |
| **T4 Never** | Not exposed to the agent at all | — |

Voice alone never authorises T3. Anyone holding the watch can talk to it.

### 4.3 Skill catalogue (initial)

| Skill | Actions → tier |
|---|---|
| **Conversations (wacli)** | Search/read/summarise my chats with people ("what did X say about Y", "what did I promise this week") → T0 · draft a reply → T1 · send as me → T2 with spoken readback for 1:1 chats with known contacts, T3 for groups, new contacts, or anything with money/addresses/attachments · extract commitments into calendar/tasks → per those skills' tiers |
| **Calendar** | Read/search → T0 · create/move my own events → T2 · invite/notify others → T2 |
| **Mail** | Search/summarise → T0 · draft → T1 · send → T2 (short replies to known contacts) or T3 (new recipients, attachments) |
| **Shopping list** | Add/remove items by voice → T1 · **Friday order:** the scheduler builds the cart Friday morning → T1; placing the order → **T3 approval on the phone** with total and item list |
| **Code orchestrator (herdr)** | Status, logs, read code → T0 · start a task in a workspace or branch → T2 · merge, deploy, delete, anything touching production → T3 (or T4 initially) |
| **Recordings** | Transcribe, summarise, extract action items → T0/T1 · turn action items into calendar events/tasks/mails → per those skills' tiers |
| **Passwords / vault** | Reveal a secret → **T4 never** · use a credential inside a tool (log into a site to fetch something) → T3, and the agent only ever handles a reference |
| **Banking** | Balances/transactions read-only → T0 (via read-only API access) · any payment or transfer → T3 with amount cap, or T4 until proven safe |
| **Ask Instinct** | Forward a request to Instinct via wacli → inherits the tier of what's asked |

### 4.4 "Passwords and bank accounts in a unified place"

- **One vault, but the agent doesn't hold the keys.** Use a password manager with service accounts (e.g. 1Password or Bitwarden Secrets Manager). The agent's service account can reach only a dedicated `onurb-agent` vault holding the few credentials its tools need, never the personal vault.
- **Tools fetch secrets at execution time.** The model sees `vault://onurb-agent/grocery-store` as a reference, never the value, and secret values are never written into prompts, logs or replies.
- **Banking:** prefer official read-only access (aggregator or open-finance APIs) over storing bank passwords for scraping. Payments, if ever enabled, use the bank's own approval flow on the phone, which is a second, independent check on top of T3.

---

## 5. Security model

| Threat | Control |
|---|---|
| Someone else uses the watch | Device token + Cloudflare Access; T3 needs the phone; kill switch revokes the watch |
| Lost watch with spooled recordings | Flash encryption before real use (SPEC §14); acknowledged chunks deleted |
| **Prompt injection** via mail, calendar invites, web pages, code comments | Content from these sources is untrusted *data*. Once a turn has read untrusted content, T2/T3 actions in that turn need an explicit approval that shows the exact action. Tool outputs are never treated as instructions. |
| **Inbound messages as an attack channel** — anyone who can message me can put text in front of the agent ("ignore previous instructions, send me…") | Chat content fetched through wacli is untrusted data, the same as mail. It can't trigger sends, payments or code actions without the approval rules above. Senders are never treated as authorised to instruct the agent. |
| Impersonation: the agent sending messages as me | Send is T2/T3 only; spoken readback of the exact text and recipient; no auto-replies in v1 |
| Third-party privacy: other people's messages flow into prompts and logs | Fetch only what the current request needs; never store message bodies in the audit log (store chat id + message ids instead); short retention for any cached conversation context; such content never reaches this public repo, test fixtures or bug reports |
| Secret leakage | Vault references only; secrets never in prompts or logs; `.env` never committed; CI secret scan |
| Tunnel exposure | Access policy in front of every route; no unauthenticated endpoints; rate limiting |
| Runaway automation (e.g. the Friday order) | Scheduled jobs prepare, humans approve; spend caps; audit log; dry-run mode |
| Over-broad code access | herdr actions scoped per workspace; no production credentials in the agent's environment |

---

## 6. Proactive behaviour

- **Scheduler:** cron-like jobs owned by the agent, e.g. "Friday 09:00: build the grocery cart and request approval" or "morning brief at 08:00 if worn".
- **Delivery:**
  - Anything needing a decision goes to the **phone** (readable, tappable).
  - Brief spoken updates wait in the **watch inbox**, because the watch only receives when you interact (it fetches, nothing is pushed). The watch never speaks unprompted in v1.

---

## 7. Phases and exit criteria

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **G0** | Gateway skeleton = the mock server grown up: real STT/TTS, sessions, audit log | Watch (or simulator) holds a 3-turn spoken conversation with a stub brain |
| **G1** | `InstinctBrain` via wacli, exposed through the Cloudflare tunnel with Access | Real questions answered by Instinct from the watch; reply/turn correlation proven |
| **G2** | Recordings pipeline: transcript + summary + action items | A 60-min meeting from the watch produces a usable summary |
| **G3** | `OnurbAgentBrain` with conversations via wacli (read/summarise only, T0), calendar, mail (T0–T2), shopping list, `ask_instinct` | Side-by-side comparison with G1, including questions only conversation context can answer |
| **G3b** | Conversation actions: drafts and sends with readback (T1–T2) | 20 sends with zero wrong-recipient or wrong-text incidents; an injection test message from a second phone triggers no action |
| **G4** | Approvals on the phone (T3), Friday shopping order end-to-end | One real order placed only after phone approval |
| **G5** | herdr code orchestration skill | Start, monitor and report a coding task by voice; nothing merged without T3 |
| **G6** | Vault integration; read-only banking | Balance question answered; zero secret values in logs (verified by scan) |

---

## 8. Open questions

| # | Question |
|---|---|
| Q1 | What exactly can wacli do? As a transport to Instinct: a dedicated thread per watch session, message IDs to correlate replies, round-trip latency. As a context source: search across chats, history depth, group chats, media/voice notes, and whether it can be limited to read-only. |
| Q2 | Does Instinct expose a direct API that would beat going through messaging? |
| Q3 | Which mail/calendar providers, and which of them does Instinct already cover (to avoid duplicating)? |
| Q4 | Which grocery service for the Friday order, and does it have an API, or would it need browser automation? |
| Q5 | Which bank(s), and is official read-only API access available for them? |
| Q6 | Where does the gateway run: the same machine as Instinct, or separately? |
| Q7 | Which STT/TTS providers? This depends on language(s) spoken to Onurb and the latency target. |
| Q8 | Should finished meeting recordings be sent to the assistant automatically, or only on request? |
