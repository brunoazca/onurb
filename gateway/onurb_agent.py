"""Onurb's own agent (docs/AGENT_B.md): `claude -p` headless on the user's login.

- Watch turns run a short agent session (fast model) with read-only tools plus
  Onurb's MCP tools (onurb_mcp.py): route_to_instinct, propose_whatsapp,
  start_background_task. Consecutive turns within SESSION_GAP_S share a session.
- WhatsApp is never sent by the agent: propose_whatsapp creates a pending action;
  the watch previews it and the gateway sends exactly that text on Confirm.
- Background tasks (deep research, computer use) run as separate sessions; their
  spoken summary lands in INBOX, which the watch polls.

Stdlib only. Personal data (contacts, avatars, task results) stays under
gateway/received/ (gitignored).
"""
import collections
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
RECEIVED = HERE / "received"
MCP_SERVER = HERE / "onurb_mcp.py"
MYNETWORK_SERVER = HERE.parent / "mynetwork_mcp" / "server.py"   # Blackbone My Network (read-only)

TURN_MODEL = os.environ.get("ONURB_TURN_MODEL", "opus")          # spoken turns (user: must be Opus)
TASK_MODEL = os.environ.get("ONURB_TASK_MODEL", "opus")          # background work: thorough
TURN_TIMEOUT_S = 150
TASK_TIMEOUT_S = 45 * 60
SESSION_GAP_S = 15 * 60
AVATAR_PX = 96
PRODUCT_PX = 120                   # product photo on the watch's purchase preview

# Private settings, never in the repo: shopping.env (gateway/skills/shopping: ship-to, ceiling)
# and notion.env (ONURB_NOTION_HUB: the id of the user's "ONURB" Notion page).
for _env in ("shopping.env", "notion.env"):
    _f = Path.home() / ".config" / "onurb" / _env
    if not _f.exists():
        continue
    for _line in _f.read_text().splitlines():
        _k, _, _v = _line.partition("=")
        if _k.strip() and not _line.lstrip().startswith("#"):
            os.environ.setdefault(_k.strip(), _v.strip().strip('"'))
NOTION_HUB = os.environ.get("ONURB_NOTION_HUB", "")
SHIP_TO = os.environ.get("ONURB_SHIP_TO", "")
SHIP_ZIP = os.environ.get("ONURB_SHIP_ZIP", "")
PURCHASE_MAX_USD = float(os.environ.get("ONURB_PURCHASE_MAX_USD", "100"))
PURCHASE_TOLERANCE_USD = 1.00      # checkout may differ from the confirmed total by at most this

READ_TOOLS = [
    "Bash(wacli --read-only:*)",
    # herdr (the user's terminal workspace of Claude Code agents, on this Mac; skill:
    # gateway/skills/herdr). Agents: full control (read, prompt, answer prompts, start).
    # Plain shell panes: read/split only, never typed into; no close/server/session.
    "Skill", "Bash(herdr agent:*)",
    "Bash(herdr pane list:*)", "Bash(herdr pane get:*)", "Bash(herdr pane read:*)",
    "Bash(herdr pane layout:*)", "Bash(herdr pane split:*)",
    "Bash(herdr workspace list:*)", "Bash(herdr tab list:*)", "Bash(herdr api snapshot:*)",
    "Bash(herdr status:*)",
    "WebSearch", "WebFetch",
    f"Read(/{Path.home()}/.claude/projects/**)", "Grep", "Glob",       # Claude Code transcripts
    "mcp__claude_ai_Google_Calendar__list_calendars", "mcp__claude_ai_Google_Calendar__list_events",
    "mcp__claude_ai_Google_Calendar__search_events", "mcp__claude_ai_Google_Calendar__get_event",
    "mcp__claude_ai_Google_Calendar__suggest_time",
    "mcp__claude_ai_Granola__search_meetings", "mcp__claude_ai_Granola__get_meetings",
    "mcp__claude_ai_Granola__search_transcripts", "mcp__claude_ai_Granola__query_granola_meetings",
    "mcp__mynetwork__search_people", "mcp__mynetwork__get_person", "mcp__mynetwork__list_by_company",
    "mcp__mynetwork__list_by_tag", "mcp__mynetwork__network_overview",
    # Notion (2026-11-11, the user: "enable it to use notion connector as it want and make it the
    # default doc creation"): the whole connector. Docs go under the "ONURB" page (NOTION_HUB).
    "mcp__claude_ai_Notion",
]
ONURB_TOOLS = ["mcp__onurb__route_to_instinct", "mcp__onurb__propose_whatsapp",
               "mcp__onurb__start_background_task", "mcp__onurb__list_background_tasks",
               "mcp__onurb__cancel_background_task", "mcp__onurb__start_code_task",
               "mcp__onurb__propose_purchase", "mcp__onurb__mark_false_trigger", "mcp__onurb__add_to_task"]

# Code tasks (2026-11-11, the user: "sim, mas nem me peça permissão. Só no repo do onurb"):
# edit/build Onurb itself without asking. Scope: this repo only. No WhatsApp, web or browser
# in these sessions, so nothing read from outside can steer the edits. Secrets stay unreadable
# (firmware/sdkconfig holds Wi-Fi, Instinct's chat and the Cloudflare token; ~/.config/onurb).
# One local commit per task (only the files it changed); push stays with the user's sessions.
REPO = HERE.parent
CODE_TOOLS = [
    f"Read(/{REPO}/**)", f"Edit(/{REPO}/**)", f"Write(/{REPO}/**)", "Grep", "Glob",
    "Bash(./tools/onurb_build.sh)", f"Bash({REPO}/tools/onurb_build.sh)",
    "Bash(python3 -m py_compile:*)", "Bash(git status:*)", "Bash(git diff:*)", "Bash(git log:*)",
    "Bash(git add:*)", "Bash(git commit:*)",   # one local commit per task; push stays with the user
    "Bash(ls:*)",
    "mcp__onurb__check_user_messages",         # additions the user records on the task's thread
]
# Background/code tasks read what the user adds on their watch thread mid-run (the turn agent doesn't).
MSG_TOOL = "mcp__onurb__check_user_messages"
MSG_LINE = ("The user may add instructions or information while you work (recorded on the watch, on this "
            "task's thread): call check_user_messages between major steps and once before finishing, and "
            "follow what it returns.")
CODE_NEVER = [
    f"Read(/{REPO}/firmware/sdkconfig)", f"Edit(/{REPO}/firmware/sdkconfig)", f"Write(/{REPO}/firmware/sdkconfig)",
    f"Read(/{REPO}/firmware/sdkconfig.old)", f"Read(/{Path.home()}/.config/**)",
    f"Read(/{REPO}/gateway/received/**)", f"Edit(/{REPO}/gateway/received/**)",
    "Bash(git push:*)", "Bash(git reset:*)", "Bash(git checkout:*)", "Bash(git rebase:*)",
    "Bash(git add -A:*)", "Bash(git add .:*)", "Bash(git commit -a:*)", "Bash(git commit --amend:*)",
    "Bash(rm:*)", "WebSearch", "WebFetch", "Task", "NotebookEdit",
]
CODE_TIMEOUT_S = 40 * 60
# One code agent at a time, in the main repo (2026-11-12, the user: "deve ser um agente mais o
# orchestrator que vai managing tudo"; replaces parallel tasks in git worktrees). The orchestrator
# routes related requests to the running one (add_to_task); new ones wait in CODE_QUEUE (FIFO,
# never refused) and start when the one ahead finishes or is cancelled.
CODE_QUEUE = collections.deque()             # code task ids; [0] is the one running (CODE_TURN below)
WORKTREES = REPO / ".worktrees"              # left over from the parallel tasks: cleaned up once
RESTART_HOOK = None               # set by the gateway: restarts it after a code task changed its code
NEVER = ["Bash(wacli send:*)", "Bash(wacli --read-only send:*)", "Bash(wacli messages send:*)",
         "Write", "Edit", "NotebookEdit", "Task",
         "Bash(herdr pane send-text:*)", "Bash(herdr pane send-keys:*)", "Bash(herdr pane run:*)",
         "Bash(herdr pane close:*)", "Bash(herdr server:*)", "Bash(herdr session:*)"]
# Hard sandbox (2026-11-10): the user's settings run Claude Code in "auto" mode,
# which let background tasks edit files, run builds and read other repos' secrets.
# The agent now runs with dontAsk (anything not in --allowedTools is denied) and
# without the user's own settings/allow-lists.
SANDBOX = ["--permission-mode", "dontAsk", "--setting-sources", "project"]

HOST = os.environ.get("ONURB_HOST", "mac")      # "mac" today; "server" after the move (docs/SERVER.md)
MAC_NOTE = ("""
What needs the user's Mac (you run ON it now, so all of it is local):
- herdr and the agents in it; Chrome with the user's logins (computer use on Blackbone, LinkedIn, etc.);
  Claude Code session transcripts (~/.claude/projects); WhatsApp Desktop's contact photos.
- Claude in Chrome may list two browsers: "macOS" (the user's Chrome, on this Mac) and "Linux" (the server's
  Chrome). Select the macOS one.""" if HOST == "mac" else """
What needs the user's Mac (you run on a server; the Mac is a remote worker that may be off or asleep):
- herdr and its agents: `herdr --machine mac ...` (over SSH). Chrome with the user's personal logins and
  Claude Code session transcripts are only on the Mac.
- Claude in Chrome may list two browsers: select the "Linux" one (this server's Chrome: logged into claude.ai and
  Blackbone). The "macOS" one is the user's own Chrome; use it only for a site that is logged in only there,
  and only if it is connected.
- The server has its own Chrome for computer use (sites it is logged into), WhatsApp (wacli), email via
  Instinct, calendar/Granola connectors, web research: those work without the Mac.
- If a task needs the Mac and it doesn't answer, say so in one sentence and offer to retry later; never
  pretend it worked.""")

TURN_PROMPT = """You are Onurb, the user's wrist voice assistant (a smartwatch). The user spoke the request below; your reply is spoken aloud by a small watch speaker and shown on a tiny screen.

Reply rules:
- ONE short sentence, ideally under 15 words, in the user's language (pt-BR or English). No lists, links, markdown or emoji.
- Never claim you did something you didn't.
- Each request is its own thread on the watch. If your reply asks the user something, or you need a decision or information from them to continue, end it with " [[OPEN]]" (the watch then shows the thread as waiting and lets the user answer it by voice). You decide when a thread is finished: end the reply with " [[DONE]]" when the request is fully handled and nothing more is needed from the user (the thread then leaves the watch; a polite "anything else?" doesn't keep it open). Never use [[DONE]] while you are waiting for the user ([[OPEN]]), a background/code task or Instinct is still working on it, or a WhatsApp/purchase preview you proposed still awaits their confirmation. If you're not sure, just say you're wrapping up and use [[DONE]]: if the user adds something on that thread later, it reopens.

What you can do:
- Read WhatsApp with `wacli --read-only ...` (chats list, messages list --chat <jid>, messages search, contacts search). You can never send with wacli.
- To message someone on WhatsApp, call propose_whatsapp: the watch shows a preview and the user confirms there. Keep the user's voice; split into natural separate bubbles. No emoji (the watch can't show them).
- Email of any kind (reading, searching, replying, sending) goes to Instinct via route_to_instinct: Instinct owns the user's mailbox.
- People in the user's network (Blackbone My Network, their LinkedIn connections): mynetwork search_people / list_by_company / list_by_tag (investor, founder, stealth) / get_person / network_overview. Say names and roles, never ids.
- Notion: read, search, create and edit pages freely. {NOTION_LINE}
- Calendar (read), Granola meeting notes, the user's Claude Code session transcripts under ~/.claude/projects, web search.
- Groceries, to-dos, reminders, or anything Instinct already handles: route_to_instinct, then relay its answer briefly.
- herdr: quick things (what's blocked, answer a prompt, prompt an agent) directly with the herdr skill; watching it over time ("fica olhando") as a background task.
- Anything long (deep research, multi-step browsing, computer use): start_background_task with a complete description, then reply that you're on it.
- "Foi falso positivo" / "alarme falso" / "não era com você" / "o gesto disparou sozinho": call mark_false_trigger (labels the last gesture as a false trigger for retraining), then reply very briefly.
- Buying a physical item ("compra", "pede", "preciso de"): start_background_task telling it to use the `shopping` skill, with the item, quantity, specs and urgency the user gave. The task picks the best option and puts a purchase preview on the watch; only the user's Confirm buys it.
- Changes to Onurb itself (the watch's screens/behavior/firmware, the gateway, your own prompts or tools): start_code_task with a complete spec of what the user wants (their words, the screen or behavior involved, edge cases). Don't ask for confirmation: the user pre-approved code changes to Onurb. Ask only if the request itself is ambiguous. Then say you're on it. There is one code agent, working directly in the Onurb repo, one change at a time: you manage it. If it's busy, a related request goes to it with add_to_task; a separate one goes through start_code_task anyway and waits in line (it starts by itself when the current one ends), so say it's queued. Never refuse a change because the agent is busy.
- You orchestrate your workers: before starting a background or code task, check list_background_tasks. If the new request belongs to one still running or queued (same change or research, a correction, a related detail), give it to that worker with add_to_task instead of starting another, and say so. Start a new task only for separate work.

Names (speech-to-text often mangles them; map them back):
- Instinct ("instint", "instinto", "insting", "in sync"): the user's OTHER AI assistant, which lives on WhatsApp and has the user's mailbox, groceries, to-dos and reminders. "Manda pro Instinct" / "pede pro Instinct" / "manda um Instinct" = call route_to_instinct with the request. Never message Instinct through wacli.
- herdr ("hoarder", "herd", "herder", "Herd"): the user's terminal workspace running many Claude Code agents, on THIS Mac. You control it directly with the `herdr` CLI (no Cloudflare tunnel needed; that is only how Instinct, which runs elsewhere, reaches it): list agents, read their screens, prompt them, answer their permission prompts, start agents. Load the `herdr` skill first for the commands and the user's approval rules (approve read-only lookups such as MCP searches and read-only SQL; never approve writes, deletes, deploys, pushes or sending messages: tell the user).
- wacli ("wacky"): the WhatsApp CLI. Blackbone / My Network: the user's company and its network product. Onurb: you.
{MAC_NOTE}

Never spend money, accept terms, delete things, or send messages/emails yourself. Those need the user's explicit confirmation: use propose_whatsapp, or Instinct, whose own confirmation rule applies."""

TASK_PROMPT = """You cannot write or edit files, run shell commands other than `wacli --read-only` and `herdr` (see the herdr skill), or build/deploy anything; if a task needs that, say so in the result and stop.
Exception: the `shopping` skill may add an item to a cart and open the checkout review page to read the exact total (never place the order); the order is placed only after the user confirms the propose_purchase preview on the watch.
You are Onurb's background worker for the user (Bruno). Do the task thoroughly with the tools you have (web research, reading WhatsApp with `wacli --read-only`, calendar, the browser via Claude in Chrome for computer use such as herdr). Read and navigate freely, but never submit forms, send messages or emails, purchase, accept terms, merge/deploy or delete anything; if the task needs one of those, stop and say what is needed (for WhatsApp, call propose_whatsapp: the user confirms on the watch). Email goes through route_to_instinct.

Names (speech-to-text often mangles them; map them back):
- Instinct ("instint", "instinto", "insting", "in sync"): the user's OTHER AI assistant, which lives on WhatsApp and has the user's mailbox, groceries, to-dos and reminders. "Manda pro Instinct" / "pede pro Instinct" / "manda um Instinct" = call route_to_instinct with the request. Never message Instinct through wacli.
- herdr ("hoarder", "herd", "herder", "Herd"): the user's terminal workspace running many Claude Code agents, on THIS Mac. You control it directly with the `herdr` CLI (no Cloudflare tunnel needed; that is only how Instinct, which runs elsewhere, reaches it): list agents, read their screens, prompt them, answer their permission prompts, start agents. Load the `herdr` skill first for the commands and the user's approval rules (approve read-only lookups such as MCP searches and read-only SQL; never approve writes, deletes, deploys, pushes or sending messages: tell the user).
- wacli ("wacky"): the WhatsApp CLI. Blackbone / My Network: the user's company and its network product. Onurb: you.
{MAC_NOTE}

Documents: anything the user should read in full (lists, trackers, reports, research, drafts) goes to Notion, the default place for docs; not Claude Docs or local files. {NOTION_LINE} Give the page a clear title, use headings, tables or a database when the content is tabular, update an existing Onurb page instead of duplicating it, and put the page link in the result (never in SPOKEN). Notion is also how you hand longer material to the user's computer.

Shopping: shipping address {SHIP_TO_LINE}; per-purchase ceiling US$ {PURCHASE_MAX} (checkout total).

{MSG_LINE}

Finish with a line `SPOKEN: <one or two short sentences>` in the user's language summarizing the result for the watch speaker. Put the full result above it.
The task is a thread on the user's watch, and you decide whether it is finished: end the SPOKEN line with " [[DONE]]" when the task is fully handled and nothing more is needed from the user (the thread leaves the watch), or with " [[OPEN]]" when you need an answer or decision from them (the thread waits for them). Not [[DONE]] while a WhatsApp/purchase preview you proposed still awaits their confirmation."""
CODE_PROMPT = f"""You are Onurb's code worker. The user (Bruno) asked, by voice from their watch, for a change to
Onurb itself. You may edit files in the Onurb repo ({REPO}) and build the watch firmware, without asking
anyone: the user pre-approved this. Only this repo; nothing else on the machine.

Repo map: firmware/main/*.c (ESP32-S3 watch, ESP-IDF + LVGL; ui.c = screens, bringup_main.c = main loop,
screen.c = screen power, turn.c/api.c = gateway calls, wifi.c, ota.c), gateway/onurb_gateway.py (HTTP
gateway, STT, Instinct), gateway/onurb_agent.py (your own prompts and tools), gateway/onurb_mcp.py,
docs/SPEC.md, docs/WATCH_CONSTRAINTS.md.

Rules:
- Before any firmware change read docs/WATCH_CONSTRAINTS.md and follow it (internal RAM is scarce: big
  buffers in PSRAM, no bigger stacks, no new periodic radio/screen activity; HTTPS needs stack).
- Keep changes minimal and in the style of the surrounding code. Comment the why, briefly.
- Firmware: after editing, run `./tools/onurb_build.sh` (from the repo root) until it prints BUILD OK.
  A good build is offered to the watch automatically. Never leave a failing build: fix it or revert
  your edits.
- Gateway/agent Python: run `python3 -m py_compile <file>` on what you changed. The gateway restarts
  itself after this task when its code changed.
- Never read, print or copy secrets: firmware/sdkconfig (Wi-Fi, Instinct's chat, tokens), ~/.config,
  .env files. This is a public repo: no names of private people, hosts, IPs, phone numbers or keys in code.
- When done (and the build is OK), commit only the files you changed, by path:
  `git add <file> <file>` then `git commit -m "onurb: <what changed> (voice request)"`. Never `git add -A`/`.`,
  never amend, push, reset or delete files; leave other uncommitted changes alone (they are someone else's).
- If the request is unclear or risky (e.g. could brick the watch), do the safe part and say what is left.
- {MSG_LINE}

Finish with a line `SPOKEN: <one or two short sentences, in Portuguese>` saying what changed and whether it
was built (e.g. "Pronto: a tela das conversas agora apaga em 1 minuto; o relógio atualiza na próxima
checagem."). Put the details above it. End the SPOKEN line with " [[DONE]]" when the change is complete and
nothing more is needed from the user (the watch thread then goes away), or " [[OPEN]]" when you need their
answer or decision to go on."""

TURN_PROMPT = TURN_PROMPT.replace("{MAC_NOTE}", MAC_NOTE)
TASK_PROMPT = TASK_PROMPT.replace("{MAC_NOTE}", MAC_NOTE).replace("{MSG_LINE}", MSG_LINE)
NOTION_LINE = (f'Create new pages as children of the user\'s "ONURB" page (page id {NOTION_HUB}); '
               "never put them in the workspace root. Don't delete, move or publicly share pages the user made."
               if NOTION_HUB else
               'Create new pages under the user\'s "ONURB" page (search for it). Don\'t delete, move or '
               "publicly share pages the user made.")
TURN_PROMPT = TURN_PROMPT.replace("{NOTION_LINE}", NOTION_LINE)
TASK_PROMPT = TASK_PROMPT.replace("{NOTION_LINE}", NOTION_LINE)
TASK_PROMPT = TASK_PROMPT.replace("{SHIP_TO_LINE}", f"{SHIP_TO} (ZIP {SHIP_ZIP})" if SHIP_TO else "not configured")
TASK_PROMPT = TASK_PROMPT.replace("{PURCHASE_MAX}", f"{PURCHASE_MAX_USD:.0f}")

CHECKOUT_PROMPT = """You are Onurb's checkout worker. The user confirmed THIS purchase on their watch a moment ago:
{order}
Place exactly this order now, in Claude in Chrome's "Linux" browser (the Onurb server's Chrome; never the
"macOS" one):
1. Open the product URL (or the site's cart if the item is already there). The cart must contain only this
   item and quantity; remove anything else.
2. Go to checkout. Check: same product, same seller, quantity; shipping address = {ship_to}; payment = the
   card saved in the account. The total (item + shipping + tax) must not exceed US$ {limit:.2f}.
3. If everything matches, place the order ("Place your order" / "Buy now" / "Pay"). This click is
   authorized by the user's Confirm, for this order only.
4. Stop without buying if anything differs (price above the limit, other seller, out of stock, different
   address), or if the site asks for a card number, CVC, password, one-time code or captcha: never type
   any of those. Report what is needed instead.
Never buy anything else, never change account settings, never save new payment methods.

Finish with the order number, the final total and the delivery date, then
`SPOKEN: <one short sentence in Portuguese: bought or not, total, arrival>`."""


# ------------------------------------------------------------------ state
LOCK = threading.Lock()
CODE_TURN = threading.Condition(LOCK)   # signalled when the code task at CODE_QUEUE[0] is done
ACTIONS = {}                       # id -> dict (kind, jid, name, messages, avatar, state, turn)
PROPOSED_BY_TURN = {}              # turn id -> latest action id
INBOX = collections.deque()        # (text, lang, task_id) waiting for the watch
SESSION = {"id": None, "t": 0.0}   # spoken conversation continuity
TASKS = {}                         # task id -> {task, started, state, proc, session, turn}
# Threads (watch: one side stick per request). Keyed by the turn id of the request
# that opened it; a spoken reply to it resumes that thread's agent session.
THREADS = collections.OrderedDict()  # turn id -> {sid, heard, reply, state}
THREADS_MAX = 40
OPEN_TAG = "[[OPEN]]"
DONE_TAG = "[[DONE]]"               # the agent says the thread is finished (see thread_state)
INSTINCT_WAIT = set()                # turns waiting for Instinct's real answer
TURN_PROCS = {}                      # turn id -> running claude -p (killed when the watch cancels)
CANCELLED_TURNS = set()

# Project skills for the agent (cwd = received/, which is gitignored): link them in.
_SKILLS = RECEIVED / ".claude" / "skills"
for _src in (HERE / "skills").glob("*/SKILL.md"):
    _dst = _SKILLS / _src.parent.name
    if not _dst.exists():
        _SKILLS.mkdir(parents=True, exist_ok=True)
        _dst.symlink_to(_src.parent)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _mcp_config(turn, gateway):
    cfg = {"mcpServers": {"onurb": {"command": "python3", "args": [str(MCP_SERVER)],
                                    "env": {"ONURB_GATEWAY": gateway, "ONURB_TURN_ID": turn}}}}
    if MYNETWORK_SERVER.is_file():     # its config/secrets live in ~/.config/onurb/mynetwork.env
        cfg["mcpServers"]["mynetwork"] = {"command": "python3", "args": [str(MYNETWORK_SERVER)]}
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, prefix="onurb_mcp_")
    json.dump(cfg, f)
    f.close()
    return f.name


def _claude(prompt, system, model, tools, turn, gateway, resume=None, timeout=TURN_TIMEOUT_S, chrome=False,
            on_start=None, never=None, cwd=None):
    """Run claude -p; returns (result text, session id). on_start(proc) lets a caller cancel it."""
    mcp = _mcp_config(turn, gateway)
    args = ["claude", "-p", prompt, "--output-format", "json", "--model", model,
            "--append-system-prompt", system, "--mcp-config", mcp,
            "--allowedTools", *tools, "--disallowedTools", *(never or NEVER), *SANDBOX]
    if resume:
        args += ["--resume", resume]
    if chrome:
        args += ["--chrome"]
    try:
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                cwd=str(cwd or RECEIVED), stdin=subprocess.DEVNULL, start_new_session=True)
        with LOCK:
            TURN_PROCS[turn] = proc
        if on_start:
            on_start(proc)
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            _kill(proc)
            proc.communicate()
            raise
        r = subprocess.CompletedProcess(args, proc.returncode, stdout, stderr)
    finally:
        os.unlink(mcp)
        with LOCK:
            TURN_PROCS.pop(turn, None)
    try:
        out = json.loads(r.stdout)
    except ValueError:
        raise RuntimeError(f"claude -p failed ({r.returncode}): {(r.stderr or r.stdout)[-300:]}")
    if out.get("is_error"):
        raise RuntimeError(f"agent error: {str(out.get('result'))[-300:]}")
    return (out.get("result") or "").strip(), out.get("session_id")


def cancel_turn(turn):
    """The watch cancelled this turn: stop its agent run now instead of letting it finish."""
    with LOCK:
        proc = TURN_PROCS.get(turn)
        CANCELLED_TURNS.add(turn)
        for t in TASKS.values():         # a cancelled addition must not reach its task either
            t["notes"] = [n for n in t.get("notes", []) if n[0] != turn]
    if proc and proc.poll() is None:
        _kill(proc)
        log(f"  turn {turn[:8]}: agent run stopped (cancelled on the watch)")


def thread_state(reply, turn, proposed=None):
    """(reply without the tags, state): open = needs the user, working = a background
    task it started is still running, done otherwise. The agent decides with [[OPEN]] /
    [[DONE]] (done threads leave the watch a minute later); untagged, a trailing "?" means
    open. [[DONE]] doesn't hold while a task / Instinct is still on it or a preview it
    proposed (proposed = action id) awaits the user's confirmation."""
    state = "done"
    done = DONE_TAG in reply
    reply = reply.replace(DONE_TAG, "").strip()
    if OPEN_TAG in reply:
        reply, state = reply.replace(OPEN_TAG, "").strip(), "open"
    elif reply.rstrip().endswith("?") and not (done and not proposed):
        state = "open"
    with LOCK:
        if (any(t.get("turn") == turn and t["state"] == "running" for t in TASKS.values())
                or turn in INSTINCT_WAIT):
            state = "working" if state == "done" else state
    return reply, state


THREAD_OF = {}                       # turn id of a thread answer -> thread id


def instinct_pending(turn, on):
    with LOCK:
        (INSTINCT_WAIT.add if on else INSTINCT_WAIT.discard)(turn)


def continue_after_instinct(turn, text, gateway):
    """Instinct's later answer: resume the thread's session so the agent finishes
    the request (e.g. propose_whatsapp). Returns True when the request is done."""
    with LOCK:
        thread = THREAD_OF.get(turn, turn)
        th = dict(THREADS.get(thread) or {})
    prompt = (f"Instinct's later answer to what you asked it for the user's request "
              f"\"{th.get('heard', '')}\": {text}\n\nFinish the user's request now with it (for "
              f"example propose_whatsapp if they asked to get it on WhatsApp). If this message "
              f"is not about that request, or Instinct is still only acknowledging, reply exactly NOOP.")
    try:
        reply, sid = _claude(prompt, TURN_PROMPT, TURN_MODEL, READ_TOOLS + ONURB_TOOLS, f"follow-{turn[:8]}",
                             gateway, th.get("sid"))
    except Exception as e:  # noqa: BLE001
        log(f"  Instinct follow-up failed: {e}")
        return False
    if reply.strip().upper().startswith("NOOP"):
        log("  (agent: not the answer yet)")
        return False
    with LOCK:
        action = PROPOSED_BY_TURN.pop(f"follow-{turn[:8]}", None)
    reply, state = thread_state(reply, "-", action)
    with LOCK:
        INBOX.append((reply, "pt", "instinct", thread, action))
    _remember_thread(thread, sid=sid, reply=reply, state=state)
    log(f"  thread {thread[:8]}: {state} after Instinct's answer: {reply!r}" + (f" + WhatsApp {action}" if action else ""))
    return True


def instinct_gave_up(turn, superseded=False):
    """No real answer from Instinct: close the thread (it was showing 'working')."""
    with LOCK:
        thread = THREAD_OF.get(turn, turn)
        if thread not in THREADS or THREADS[thread].get("state") != "working":
            return
        msg = ("O Instinct recebeu outro pedido antes de responder este; pergunte de novo se ainda precisar."
               if superseded else "O Instinct não mandou o resultado em 15 minutos.")
        INBOX.append((msg, "pt", "instinct", thread, None))
    _remember_thread(thread, reply=msg, state="done")
    log(f"  thread {thread[:8]}: closed ({'superseded' if superseded else 'Instinct timeout'})")


def _remember_thread(tid, **kw):
    with LOCK:
        THREADS.setdefault(tid, {}).update(kw)
        THREADS.move_to_end(tid)
        while len(THREADS) > THREADS_MAX:
            THREADS.popitem(last=False)


DISMISS_TAG = "[[DISMISS]]"
GESTURE_NOTE = ("(This recording was started by the raise-to-mouth wrist gesture, which often fires by accident. "
                f"If what you hear is not a request or question addressed to you (background conversation, the user "
                f"talking to someone else, TV/music, a fragment that makes no sense on its own), reply exactly "
                f"{DISMISS_TAG} and nothing else: the watch then drops it silently and learns from the false trigger. "
                "When in doubt between a real short request and noise, answer it.) ")


def run_turn(text, lang, turn, gateway, note="", thread=None, gesture=False):
    """Spoken turn. Returns (reply, action_id or None, thread state); reply == DISMISS_TAG when a
    gesture-started recording wasn't meant for Onurb (the gateway drops it as a false trigger).
    thread: the turn id of an earlier request the user is answering on the watch."""
    if gesture and not thread:
        note = GESTURE_NOTE + note
    with LOCK:
        th = dict(THREADS[thread]) if thread in THREADS else None
        # The watch's mic works on any thread (2026-11-12): a running task of this thread gets the
        # addition directly (check_user_messages / resumed at its end); a finished one is context.
        running, last = [], None
        for tid, t in TASKS.items() if th else ():
            if t.get("turn") != thread:
                continue
            if t["state"] == "running":
                t.setdefault("notes", []).append((turn, text))
                running.append(tid)
            else:
                last = (tid, t)
    if th:
        resume = th.get("sid")
        ctx = ""
        if running:
            ctx = (f" Their message was passed to this thread's background task {', '.join(running)}, still "
                   "running, which will take it into account: just acknowledge briefly unless they ask something "
                   "else (e.g. to cancel it); don't start a duplicate task.")
        elif last:
            tid, t = last
            ctx = (f" This thread's background task {tid} ({t['task'][:200]!r}) "
                   + ("was cancelled." if t["state"] == "cancelled" else
                      f"finished; its full result is in {t['out']}." if t.get("out") else "finished.")
                   + " If they add to it or want more, continue from that context (a new background or code task"
                   " with the full context if it needs long work).")
        prompt = (f"(The user is answering inside the thread of their earlier request "
                  f"\"{th.get('heard', '')}\", where you replied \"{th.get('reply', '')}\".{ctx}) {note}{text}")
    else:
        resume = SESSION["id"] if time.time() - SESSION["t"] < SESSION_GAP_S else None
        prompt = f"{note}{text}" if note else text
    t0 = time.monotonic()
    try:
        reply, sid = _claude(prompt, TURN_PROMPT, TURN_MODEL, READ_TOOLS + ONURB_TOOLS, turn, gateway, resume)
    except RuntimeError as e:
        if not resume or turn in CANCELLED_TURNS:   # killed by a cancel: don't run it again
            raise
        log(f"  resume failed ({e}); fresh session")
        reply, sid = _claude(prompt, TURN_PROMPT, TURN_MODEL, READ_TOOLS + ONURB_TOOLS, turn, gateway)
    if gesture and DISMISS_TAG in reply:
        log(f"  agent ({time.monotonic() - t0:.1f} s): not addressed to Onurb - false gesture trigger")
        return DISMISS_TAG, None, "done"
    SESSION.update(id=sid, t=time.time())
    log(f"  agent ({time.monotonic() - t0:.1f} s, {'resumed' if resume else 'new'} session"
        + (f", thread {thread[:8]}" if th else "") + ")")
    with LOCK:
        action = PROPOSED_BY_TURN.pop(turn, None)
    reply, state = thread_state(reply, turn, action)
    if th:      # background tasks / Instinct answers from a thread reply report to that thread
        with LOCK:
            if state == "done" and any(TASKS[tid]["state"] == "running" for tid in running):
                state = "working"           # the task it was added to is still on it
            if turn in INSTINCT_WAIT:
                THREAD_OF[turn] = thread
            for t in TASKS.values():
                if t.get("turn") == turn:
                    t["turn"] = thread
    _remember_thread(thread if th else turn, sid=sid, reply=reply, state=state,
                     **({} if th else {"heard": text}))
    return reply, action, state


def run_edit(action_id, instruction, turn, gateway):
    """User asked (by voice) to change a proposed message; the agent proposes again."""
    with LOCK:
        a = ACTIONS.get(action_id)
    if a is None:
        return "Não achei essa mensagem.", None
    a["state"] = "edited"
    prompt = (f"The user wants a change to the WhatsApp message you proposed to {a['name']} "
              f"({json.dumps(a['messages'], ensure_ascii=False)}): \"{instruction}\". "
              "Call propose_whatsapp again with the revised messages, then reply briefly.")
    resume = SESSION["id"]
    reply, sid = _claude(prompt, TURN_PROMPT, TURN_MODEL, READ_TOOLS + ONURB_TOOLS, turn, gateway, resume)
    SESSION.update(id=sid, t=time.time())
    with LOCK:
        new = PROPOSED_BY_TURN.pop(turn, None)
    return reply.replace(DONE_TAG, "").replace(OPEN_TAG, "").strip(), new   # no thread here: never speak a tag


# ------------------------------------------------------------------ WhatsApp actions
def _wacli_json(args, timeout=30):
    r = subprocess.run(["wacli", *args, "--json"], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"wacli {' '.join(args[:3])}: {r.stderr.strip()[-200:]}")
    d = json.loads(r.stdout or "{}")
    return d.get("data", d)


_CHATS = {"t": 0.0, "list": []}


def _chats():
    """wacli chat list (people and groups, with names), cached for 5 min."""
    if time.time() - _CHATS["t"] > 300:
        try:
            _CHATS["list"] = _wacli_json(["--read-only", "chats", "list", "--limit", "5000"]) or []
            _CHATS["t"] = time.time()
        except (RuntimeError, ValueError) as e:
            log(f"  chats list: {e}")
    return _CHATS["list"]


def _name_for_jid(jid):
    for c in _chats():
        if c.get("jid") == jid and c.get("name"):
            return c["name"]
    return jid.split("@")[0]


def resolve_contact(to):
    """-> (jid, name) or raises LookupError(candidates list). People and groups."""
    to = to.strip()
    digits = re.sub(r"\D", "", to)
    if "@" in to:
        return to, _name_for_jid(to)
    chats = [c for c in _chats() if str(c.get("name", "")).strip().lower() == to.lower()]
    if len(chats) == 1:                          # exact chat name (incl. groups, e.g. "Eu")
        return chats[0]["jid"], chats[0]["name"]
    if len(digits) >= 10 and len(digits) >= len(to.replace(" ", "")) - 3:
        return f"{digits}@s.whatsapp.net", to
    found = _wacli_json(["--read-only", "contacts", "search", to, "--limit", "10"]) or []
    def label(c):
        return c.get("alias") or c.get("system_name") or c.get("name") or c.get("phone") or c.get("jid")
    exact = [c for c in found if to.lower() in [str(label(c)).lower(), str(c.get("name", "")).lower()]]
    pool = exact or found
    pool = [c for c in pool if str(c.get("jid", "")).endswith("@s.whatsapp.net")] or pool
    if len(pool) == 1:
        return pool[0]["jid"], label(pool[0])
    if not pool:
        raise LookupError([])
    raise LookupError([f"{label(c)} ({c.get('phone') or c.get('jid')})" for c in pool[:6]])


def _bmp_to_rgb565(path, px):
    data = Path(path).read_bytes()
    off, = struct.unpack_from("<I", data, 10)
    w, h = struct.unpack_from("<ii", data, 18)
    bpp, = struct.unpack_from("<H", data, 28)
    if bpp not in (24, 32):
        raise ValueError(f"bmp {bpp} bpp")
    step = bpp // 8
    row = (w * step + 3) & ~3
    out = bytearray()
    for y in range(px):
        sy = (h - 1 - y) if h > 0 else y          # BMP rows are bottom-up when h > 0
        for x in range(px):
            i = off + sy * row + x * step
            b, g, r = data[i], data[i + 1], data[i + 2]
            out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return bytes(out)


# WhatsApp Desktop (Mac) keeps 168 px JPEG thumbnails of profile photos, named
# "<phone>-<picture id>.thumb". wacli can't fetch photos while the user's
# `wacli sync --follow` holds its store, so that cache comes first.
DESKTOP_PHOTOS = Path.home() / "Library/Group Containers/group.net.whatsapp.WhatsApp.shared/Media/Profile"


def _jpeg_to_rgb565(jpeg_path):
    with tempfile.TemporaryDirectory() as tmp:
        bmp = Path(tmp) / "out.bmp"
        subprocess.run(["sips", "-z", str(AVATAR_PX), str(AVATAR_PX), "-s", "format", "bmp",
                        str(jpeg_path), "--out", str(bmp)], check=True, capture_output=True)
        return _bmp_to_rgb565(bmp, AVATAR_PX)


def _desktop_photo(jid):
    user = jid.split("@")[0]                     # phone digits, or "<creator>-<ts>" for groups
    if not re.fullmatch(r"[0-9]+(-[0-9]+)?", user):
        return None
    pat = re.compile(re.escape(user) + r"-[0-9]+\.thumb$")
    thumbs = sorted((p for p in DESKTOP_PHOTOS.glob(f"{user}-*.thumb") if pat.match(p.name)),
                    key=lambda p: p.stat().st_mtime)
    return thumbs[-1] if thumbs else None


def _wacli_photo(jid, tmp):
    """Live fetch through wacli; only works while no other wacli holds the store."""
    info = _wacli_json(["profile", "picture-info", "--jid", jid, "--preview"], timeout=20) or {}
    url = info.get("url") or info.get("URL") or (info.get("picture") or {}).get("url")
    if not url:
        return None
    dst = Path(tmp) / "in.jpg"
    with urllib.request.urlopen(url, timeout=15) as r:
        dst.write_bytes(r.read())
    return dst


def fetch_avatar(jid):
    """WhatsApp profile picture as raw RGB565 (AVATAR_PX square), or None."""
    try:
        thumb = _desktop_photo(jid)
        if thumb:
            return _jpeg_to_rgb565(thumb)
        with tempfile.TemporaryDirectory() as tmp:
            src = _wacli_photo(jid, tmp)
            return _jpeg_to_rgb565(src) if src else None
    except Exception as e:  # noqa: BLE001 - a missing photo must not block the preview
        log(f"  avatar: none ({str(e)[:80]})")
        return None


# The watch fonts have no emoji; the preview must show exactly what gets sent,
# so proposed messages carry no emoji at all.
EMOJI_RE = re.compile("[\U0001F000-\U0001FAFF\u2600-\u27BF\uFE0F\u200D\U0001F1E6-\U0001F1FF]")


def propose_whatsapp(to, messages, turn):
    messages = [re.sub(r"\s{2,}", " ", EMOJI_RE.sub("", m)).strip() for m in messages if m]
    messages = [m for m in messages if m][:8]
    if not messages:
        return {"error": "no message text"}
    try:
        jid, name = resolve_contact(to)
    except LookupError as e:
        cands = e.args[0]
        return {"candidates": cands} if cands else {"error": f"no WhatsApp contact matches '{to}'"}
    aid = uuid.uuid4().hex[:12]
    action = {"id": aid, "kind": "whatsapp", "jid": jid, "name": name, "messages": messages,
              "avatar": fetch_avatar(jid), "state": "pending", "turn": turn, "t": time.time()}
    with LOCK:
        ACTIONS[aid] = action
        PROPOSED_BY_TURN[turn] = aid
    log(f"  WhatsApp proposal {aid} -> {name}: {len(messages)} message(s), avatar {'yes' if action['avatar'] else 'no'}")
    return {"action_id": aid, "name": name}


def _image_to_rgb565(url, px):
    """Product photo URL -> px*px RGB565 (letterboxed on white), or None."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (Onurb watch preview)"})
        with tempfile.TemporaryDirectory() as tmp:
            src, out = Path(tmp) / "in.img", Path(tmp) / "out"
            with urllib.request.urlopen(req, timeout=20) as r:
                src.write_bytes(r.read(4 * 1024 * 1024))
            if shutil.which("ffmpeg"):
                raw = out.with_suffix(".raw")
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(src), "-vf",
                                f"scale={px}:{px}:force_original_aspect_ratio=decrease,"
                                f"pad={px}:{px}:(ow-iw)/2:(oh-ih)/2:white", "-frames:v", "1",
                                "-f", "rawvideo", "-pix_fmt", "rgb565le", str(raw)], check=True, capture_output=True)
                data = raw.read_bytes()
                return data if len(data) == px * px * 2 else None
            bmp = out.with_suffix(".bmp")                # macOS: sips
            subprocess.run(["sips", "-Z", str(px), "-s", "format", "bmp", str(src), "--out", str(bmp)],
                           check=True, capture_output=True)
            subprocess.run(["sips", "--padToHeightWidth", str(px), str(px), "--padColor", "FFFFFF", str(bmp)],
                           check=True, capture_output=True)
            return _bmp_to_rgb565(bmp, px)
    except Exception as e:  # noqa: BLE001 - the preview works without a photo
        log(f"  product photo: none ({str(e)[:80]})")
        return None


def propose_purchase(p, turn):
    """Purchase preview for the watch (gateway/skills/shopping). The order is placed only on Confirm."""
    try:
        total = float(str(p.get("total_usd", "")).replace("$", "").replace(",", ""))
    except ValueError:
        return {"error": "total_usd must be a number (checkout total in USD)"}
    if total <= 0:
        return {"error": "total_usd must be the checkout total"}
    if total > PURCHASE_MAX_USD:
        return {"error": f"total US$ {total:.2f} is above the per-purchase ceiling of US$ {PURCHASE_MAX_USD:.0f}; "
                         "pick an option under it or tell the user"}
    title = re.sub(r"\s{2,}", " ", EMOJI_RE.sub("", str(p.get("title", "")))).strip()[:90]
    details = [re.sub(r"\s{2,}", " ", EMOJI_RE.sub("", str(d))).strip() for d in (p.get("details") or [])]
    details = [d[:200] for d in details if d][:6]
    if not title or not p.get("url"):
        return {"error": "title and url are required"}
    aid = uuid.uuid4().hex[:12]
    action = {"id": aid, "kind": "purchase", "name": title, "site": str(p.get("site", ""))[:40],
              "url": str(p["url"])[:500], "total": total, "details": details,
              "messages": [f"US$ {total:.2f}" + (f" · {p.get('site')}" if p.get("site") else "")] + details,
              "avatar": _image_to_rgb565(str(p.get("image_url", "")), PRODUCT_PX) if p.get("image_url") else None,
              "avatar_px": PRODUCT_PX, "state": "pending", "turn": turn, "t": time.time()}
    with LOCK:
        ACTIONS[aid] = action
        PROPOSED_BY_TURN[turn] = aid
    log(f"  purchase proposal {aid}: {title!r} US$ {total:.2f} at {action['site']}, photo {'yes' if action['avatar'] else 'no'}")
    return {"action_id": aid}


def _purchase_log(a, result):
    f = RECEIVED / "purchases.csv"
    new = not f.exists()
    with open(f, "a") as fh:
        if new:
            fh.write("time,action,site,total_usd,title,url,result\n")
        clean = lambda x: str(x).replace(",", " ").replace("\n", " ")[:200]
        fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')},{a['id']},{clean(a['site'])},{a['total']:.2f},"
                 f"{clean(a['name'])},{clean(a['url'])},{clean(result)}\n")


def _checkout(aid):
    """After the watch Confirm: place exactly the confirmed order (background)."""
    with LOCK:
        a = ACTIONS[aid]
    order = (f"- {a['name']}\n- site: {a['site']}\n- product page: {a['url']}\n"
             f"- confirmed total: US$ {a['total']:.2f}\n- notes shown to the user: {'; '.join(a['details'])}")
    prompt = CHECKOUT_PROMPT.format(order=order, ship_to=SHIP_TO or "the account's default address",
                                    limit=a["total"] + PURCHASE_TOLERANCE_USD)
    tid = uuid.uuid4().hex[:8]
    with LOCK:
        TASKS[tid] = {"task": f"checkout {aid}: {a['name']}", "started": time.monotonic(), "state": "running",
                      "proc": None, "turn": a.get("turn") or "", "kind": "checkout"}
    def work():
        log(f"  purchase {aid}: checkout task {tid} started (confirmed on the watch)")
        try:
            text, _ = _claude(f"Place the confirmed order {aid}.", prompt, TASK_MODEL, ["mcp__claude-in-chrome"],
                              f"checkout-{tid}", "http://127.0.0.1:8080", timeout=20 * 60, chrome=True,
                              on_start=lambda pr: TASKS[tid].__setitem__("proc", pr))
        except Exception as e:  # noqa: BLE001
            text = f"SPOKEN: A compra não foi concluída: {str(e)[:80]}"
        m = re.search(r"SPOKEN:\s*(.+)", text, re.S)
        spoken = (m.group(1) if m else text).strip().splitlines()[0][:300]
        out = RECEIVED / "tasks"
        out.mkdir(exist_ok=True)
        (out / f"{time.strftime('%Y%m%d_%H%M%S')}_checkout_{aid}.md").write_text(f"# Checkout {aid}\n\n{order}\n\n---\n\n{text}\n")
        with LOCK:
            TASKS[tid]["state"] = "done"
            TASKS[tid]["proc"] = None
            a["state"] = "ordered" if re.search(r"order|pedido", text, re.I) else "checkout-done"
            INBOX.append((spoken, "pt", tid, a.get("turn") or "", None))
        _purchase_log(a, spoken)
        log(f"  purchase {aid}: {spoken!r}")
    threading.Thread(target=work, daemon=True, name=f"checkout-{tid}").start()


def confirm(aid):
    """WhatsApp: send exactly the previewed messages. Purchase: start the checkout. Returns (ok, detail)."""
    with LOCK:
        a = ACTIONS.get(aid)
        if a is None or a["state"] != "pending":
            return False, "not pending"
        a["state"] = "ordering" if a["kind"] == "purchase" else "sending"
    if a["kind"] == "purchase":
        log(f"  action {aid}: purchase CONFIRMED on the watch ({a['name']!r}, US$ {a['total']:.2f})")
        _checkout(aid)
        return True, "ordering"
    for m in a["messages"]:
        r = subprocess.run(["wacli", "send", "text", "--to", a["jid"], "--message", m],
                           capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            a["state"] = "failed"
            return False, r.stderr.strip()[-200:]
    a["state"] = "sent"
    log(f"  action {aid}: sent {len(a['messages'])} message(s) to {a['name']} (confirmed on the watch)")
    return True, "sent"


def cancel(aid):
    with LOCK:
        a = ACTIONS.get(aid)
        if a and a["state"] == "pending":
            a["state"] = "cancelled"
            log(f"  action {aid}: cancelled on the watch")
            return True
    return False


def action_public(aid):
    with LOCK:
        a = ACTIONS.get(aid)
    if a is None:
        return None
    return {"id": a["id"], "kind": a["kind"], "name": a["name"], "messages": a["messages"],
            "state": a["state"], "avatar": bool(a["avatar"]), "avatar_px": a.get("avatar_px", AVATAR_PX)}


# ------------------------------------------------------------------ background tasks
def _kill(proc):
    """Stop a claude -p run and its children (MCP servers, Chrome bridge)."""
    try:
        os.killpg(proc.pid, 15)
    except (ProcessLookupError, PermissionError):
        pass


def list_tasks():
    with LOCK:
        return [{"id": tid, "kind": t.get("kind", "background"),
                 "state": "queued" if t.get("queued") and t["state"] == "running" else t["state"],
                 "task": t["task"][:200], "minutes": round((time.monotonic() - t["started"]) / 60, 1)}
                for tid, t in TASKS.items()]


def add_to_task(tid, text, turn):
    """add_to_task: a new request that belongs to a running task goes to that task's worker (it reads it
    with check_user_messages, or is resumed with it at its end) instead of starting another one."""
    with LOCK:
        t = TASKS.get(tid)
        if t is None:
            return "unknown task"
        if t["state"] != "running":
            return f"not running ({t['state']}); start a new task instead"
        t.setdefault("notes", []).append((turn, text))
    log(f"task {tid}: request added by the agent from turn {turn[:8]}")
    return "added: the task's worker will take it into account; its result arrives on that task's watch thread"


def cancel_task(tid):
    """Cancel a running background task; its result is dropped (nothing reaches the watch)."""
    with LOCK:
        t = TASKS.get(tid)
        if t is None:
            return "unknown task"
        if t["state"] != "running":
            return f"not running ({t['state']})"
        t["state"] = "cancelled"
        proc = t.get("proc")
    if proc:
        _kill(proc)
    log(f"task {tid}: cancelled by the agent")
    return "cancelled"


def take_task_notes(turn):
    """check_user_messages: what the user added on the task's watch thread since the last call.
    turn is the task's MCP turn id (task-<id> / code-<id>)."""
    tid = turn.split("-", 1)[-1]
    with LOCK:
        t = TASKS.get(tid)
        if t is None or t["state"] != "running":
            return []
        notes, t["notes"] = [n[1] for n in t.get("notes", [])], []
    if notes:
        log(f"task {tid}: picked up {len(notes)} addition(s) from the watch")
    return notes


def _finish_with_notes(tid, text, sid, run):
    """Additions that arrived after the task's last check_user_messages: resume its session with
    them (a few rounds) so nothing the user added is lost. run(prompt, sid) -> (text, sid)."""
    for _ in range(3):
        notes = take_task_notes(f"x-{tid}")
        if not notes:
            break
        if not sid:
            log(f"task {tid}: {len(notes)} addition(s) dropped (no session to resume)")
            break
        prompt = ("The user added this from the watch while you were working; take it into account, finish "
                  "the task and give the final result again (with the SPOKEN line):\n"
                  + "\n".join(f"- {n}" for n in notes))
        try:
            text, sid = run(prompt, sid)
        except Exception as e:  # noqa: BLE001 - keep the result we already have
            log(f"task {tid}: resuming with the additions failed: {e}")
            break
    return text


def start_task(task, turn, gateway, lang_hint="pt"):
    tid = uuid.uuid4().hex[:8]
    with LOCK:
        TASKS[tid] = {"task": task, "started": time.monotonic(), "state": "running", "proc": None,
                      "turn": turn}
    def started(proc):
        with LOCK:
            TASKS[tid]["proc"] = proc
            cancelled = TASKS[tid]["state"] == "cancelled"
        if cancelled:
            _kill(proc)
    def work():
        t0 = time.monotonic()
        log(f"task {tid}: started: {task[:120]!r}")
        tools = (READ_TOOLS + [t for t in ONURB_TOOLS if t != "mcp__onurb__start_code_task"]
                 + ["mcp__claude-in-chrome", MSG_TOOL])
        run = lambda p, s=None: _claude(p, TASK_PROMPT, TASK_MODEL, tools, f"task-{tid}", gateway, resume=s,
                                        timeout=TASK_TIMEOUT_S, chrome=True, on_start=started)
        try:
            text, sid = run(task)
        except Exception as e:  # noqa: BLE001
            text, sid = f"SPOKEN: A tarefa em segundo plano falhou: {str(e)[:80]}", None
        text = _finish_with_notes(tid, text, sid, run)
        with LOCK:
            cancelled = TASKS[tid]["state"] == "cancelled"
            TASKS[tid]["proc"] = None
            if not cancelled:
                TASKS[tid]["state"] = "done"
        if cancelled:
            log(f"task {tid}: stopped after {(time.monotonic() - t0) / 60:.1f} min (cancelled; result dropped)")
            return
        out = RECEIVED / "tasks"
        out.mkdir(exist_ok=True)
        f = out / f"{time.strftime('%Y%m%d_%H%M%S')}_{tid}.md"
        f.write_text(f"# Task {tid}\n\n{task}\n\n---\n\n{text}\n")
        TASKS[tid]["out"] = str(f.relative_to(RECEIVED))   # a later follow-up on its thread reads it
        m = re.search(r"SPOKEN:\s*(.+)", text, re.S)
        spoken = (m.group(1) if m else text).strip().splitlines()[0][:300]
        with LOCK:
            thread = TASKS[tid].get("turn") or ""
            action = PROPOSED_BY_TURN.pop(f"task-{tid}", None)
        spoken, state = thread_state(spoken, thread, action)   # its [[OPEN]] / [[DONE]] decides
        if thread in THREADS:   # before the inbox item: the watch gets the state with it
            _remember_thread(thread, reply=spoken, state=state)
        with LOCK:
            INBOX.append((spoken, lang_hint, tid, thread, action))
        log(f"task {tid}: done in {(time.monotonic() - t0) / 60:.1f} min -> watch inbox: {spoken!r}")
    threading.Thread(target=work, daemon=True, name=f"task-{tid}").start()
    return tid


def _code_mtimes():
    return {str(f): f.stat().st_mtime for f in (REPO / "gateway").glob("*.py")}


def _git(*args, cwd=None, timeout=120):
    return subprocess.run(["git", *args], cwd=str(cwd or REPO), capture_output=True, text=True,
                          timeout=timeout, stdin=subprocess.DEVNULL)


_OLD_WORKTREES_CLEANED = False


def _clean_old_worktrees():
    """Once per gateway run: remove what the old parallel code tasks left (worktrees under .worktrees/
    on code/<id> branches, and those branches). Only clean ones whose commits are all in main; any
    with uncommitted edits or unmerged commits is kept and logged, never deleted."""
    global _OLD_WORKTREES_CLEANED
    if _OLD_WORKTREES_CLEANED:
        return
    _OLD_WORKTREES_CLEANED = True
    try:
        _git("worktree", "prune")
        wts, path = {}, None
        for line in _git("worktree", "list", "--porcelain").stdout.splitlines():
            if line.startswith("worktree "):
                path = Path(line[9:])
            elif line.startswith("branch refs/heads/code/") and path and path.parent == WORKTREES:
                wts[line[len("branch refs/heads/"):]] = path
        for ref in _git("for-each-ref", "--format=%(refname:short)", "refs/heads/code/").stdout.split():
            wt = wts.get(ref)
            if wt and _git("status", "--porcelain", cwd=wt).stdout.strip():
                log(f"old code worktree {wt.name}: uncommitted edits, kept")
                continue
            ahead = _git("rev-list", "--count", f"main..{ref}").stdout.strip()
            if ahead != "0":
                log(f"old code branch {ref}: {ahead or '?'} commit(s) not in main, kept")
                continue
            if wt:
                _git("worktree", "remove", "--force", str(wt))
            _git("branch", "-D", ref)
            log(f"old code branch {ref} removed (already in main)")
    except Exception as e:  # noqa: BLE001 - housekeeping only
        log(f"old code worktrees cleanup failed: {e}")


def code_ahead(tid):
    """How many code tasks are before this one in line (0: it's the one running)."""
    with LOCK:
        return CODE_QUEUE.index(tid) if tid in CODE_QUEUE else 0


def start_code_task(task, turn, gateway, lang_hint="pt"):
    """Edit/build Onurb itself (see CODE_TOOLS), directly in the repo. One code agent at a time: the
    others wait in CODE_QUEUE and start in order. Mac only (the repo lives here)."""
    if HOST != "mac":
        return None, "code tasks run on the Mac (the repo and the firmware toolchain are there)"
    with LOCK:
        tid = uuid.uuid4().hex[:8]
        TASKS[tid] = {"task": task, "started": time.monotonic(), "state": "running", "proc": None,
                      "turn": turn, "kind": "code", "queued": bool(CODE_QUEUE)}
        CODE_QUEUE.append(tid)
    def started(proc):
        with LOCK:
            TASKS[tid]["proc"] = proc
            cancelled = TASKS[tid]["state"] == "cancelled"
        if cancelled:
            _kill(proc)
    def work():
        with CODE_TURN:
            CODE_TURN.wait_for(lambda: CODE_QUEUE[0] == tid)
            TASKS[tid]["queued"] = False
            go = TASKS[tid]["state"] == "running"   # cancelled while queued: just pass the turn on
        restart = False
        try:
            if go:
                _clean_old_worktrees()
                restart = _code_work(tid, task, gateway, lang_hint, started)
        finally:
            with CODE_TURN:   # the next one in line starts (also after a cancel or a crash)
                CODE_QUEUE.remove(tid)
                CODE_TURN.notify_all()
        if restart:
            _restart_after_code(tid)
    threading.Thread(target=work, daemon=True, name=f"code-{tid}").start()
    return tid, None


def _code_work(tid, task, gateway, lang_hint, started):
    """Run the code agent in the repo; its result goes to the task's watch thread. Returns True when
    the gateway's own code changed (it must restart)."""
    t0 = time.monotonic()
    before = _code_mtimes()
    log(f"code task {tid}: started: {task[:120]!r}")
    run = lambda p, s=None: _claude(p, CODE_PROMPT, TASK_MODEL, CODE_TOOLS, f"code-{tid}", gateway, resume=s,
                                    timeout=CODE_TIMEOUT_S, on_start=started, never=CODE_NEVER, cwd=REPO)
    try:
        text, sid = run(task)
    except Exception as e:  # noqa: BLE001
        text, sid = f"SPOKEN: A mudança no Onurb falhou: {str(e)[:80]}", None
    text = _finish_with_notes(tid, text, sid, run)
    with LOCK:
        cancelled = TASKS[tid]["state"] == "cancelled"
        TASKS[tid]["proc"] = None
        if not cancelled:
            TASKS[tid]["state"] = "done"
    if cancelled:
        log(f"code task {tid}: cancelled after {(time.monotonic() - t0) / 60:.1f} min")
        return False
    out = RECEIVED / "tasks"
    out.mkdir(exist_ok=True)
    f = out / f"{time.strftime('%Y%m%d_%H%M%S')}_code_{tid}.md"
    f.write_text(f"# Code task {tid}\n\n{task}\n\n---\n\n{text}\n")
    TASKS[tid]["out"] = str(f.relative_to(RECEIVED))   # a later follow-up on its thread reads it
    m = re.search(r"SPOKEN:\s*(.+)", text, re.S)
    spoken = (m.group(1) if m else text).strip().splitlines()[0][:300]
    with LOCK:
        thread = TASKS[tid].get("turn") or ""
    spoken, state = thread_state(spoken, thread)   # its [[OPEN]] / [[DONE]] decides
    if thread in THREADS:   # before the inbox item: the watch gets the state with it
        _remember_thread(thread, reply=spoken, state=state)
    with LOCK:
        INBOX.append((spoken, lang_hint, tid, thread, None))
    log(f"code task {tid}: done in {(time.monotonic() - t0) / 60:.1f} min -> watch inbox: {spoken!r}")
    return _code_mtimes() != before and RESTART_HOOK is not None


def _restart_after_code(tid):
    """The gateway's own code changed: restart once the watch has the result (the inbox is in memory)."""
    for _ in range(180):
        with LOCK:
            if not any(item[2] == tid for item in INBOX):
                break
        time.sleep(1)
    # A restart kills running tasks (they are the gateway's children) and forgets queued ones: wait
    # for them (lost a background task this way on 2026-11-11). This task has already passed its
    # turn on, so queued code tasks run meanwhile instead of waiting on it.
    for _ in range(60 * 60):
        with LOCK:
            if not any(t["state"] == "running" for t in TASKS.values()):
                break
        time.sleep(1)
    log(f"code task {tid}: gateway code changed - restarting the gateway")
    RESTART_HOOK()


def inbox_pop():
    with LOCK:
        return INBOX.popleft() if INBOX else None
