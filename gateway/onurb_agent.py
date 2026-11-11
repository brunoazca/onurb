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
]
ONURB_TOOLS = ["mcp__onurb__route_to_instinct", "mcp__onurb__propose_whatsapp",
               "mcp__onurb__start_background_task", "mcp__onurb__list_background_tasks",
               "mcp__onurb__cancel_background_task", "mcp__onurb__start_code_task"]

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
]
CODE_NEVER = [
    f"Read(/{REPO}/firmware/sdkconfig)", f"Edit(/{REPO}/firmware/sdkconfig)", f"Write(/{REPO}/firmware/sdkconfig)",
    f"Read(/{REPO}/firmware/sdkconfig.old)", f"Read(/{Path.home()}/.config/**)",
    f"Read(/{REPO}/gateway/received/**)", f"Edit(/{REPO}/gateway/received/**)",
    "Bash(git push:*)", "Bash(git reset:*)", "Bash(git checkout:*)", "Bash(git rebase:*)",
    "Bash(git add -A:*)", "Bash(git add .:*)", "Bash(git commit -a:*)", "Bash(git commit --amend:*)",
    "Bash(rm:*)", "WebSearch", "WebFetch", "Task", "NotebookEdit",
]
CODE_TIMEOUT_S = 40 * 60
RESTART_HOOK = None                # set by the gateway: restarts it after a code task changed its code
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
- Each request is its own thread on the watch. If your reply asks the user something, or you need a decision or information from them to continue, end it with " [[OPEN]]" (the watch then shows the thread as waiting and lets the user answer it by voice). Otherwise don't add it.

What you can do:
- Read WhatsApp with `wacli --read-only ...` (chats list, messages list --chat <jid>, messages search, contacts search). You can never send with wacli.
- To message someone on WhatsApp, call propose_whatsapp: the watch shows a preview and the user confirms there. Keep the user's voice; split into natural separate bubbles. No emoji (the watch can't show them).
- Email of any kind (reading, searching, replying, sending) goes to Instinct via route_to_instinct: Instinct owns the user's mailbox.
- People in the user's network (Blackbone My Network, their LinkedIn connections): mynetwork search_people / list_by_company / list_by_tag (investor, founder, stealth) / get_person / network_overview. Say names and roles, never ids.
- Calendar (read), Granola meeting notes, the user's Claude Code session transcripts under ~/.claude/projects, web search.
- Groceries, to-dos, reminders, or anything Instinct already handles: route_to_instinct, then relay its answer briefly.
- herdr: quick things (what's blocked, answer a prompt, prompt an agent) directly with the herdr skill; watching it over time ("fica olhando") as a background task.
- Anything long (deep research, multi-step browsing, computer use): start_background_task with a complete description, then reply that you're on it.
- Changes to Onurb itself (the watch's screens/behavior/firmware, the gateway, your own prompts or tools): start_code_task with a complete spec of what the user wants (their words, the screen or behavior involved, edge cases). Don't ask for confirmation: the user pre-approved code changes to Onurb. Ask only if the request itself is ambiguous. Then say you're on it.

Names (speech-to-text often mangles them; map them back):
- Instinct ("instint", "instinto", "insting", "in sync"): the user's OTHER AI assistant, which lives on WhatsApp and has the user's mailbox, groceries, to-dos and reminders. "Manda pro Instinct" / "pede pro Instinct" / "manda um Instinct" = call route_to_instinct with the request. Never message Instinct through wacli.
- herdr ("hoarder", "herd", "herder", "Herd"): the user's terminal workspace running many Claude Code agents, on THIS Mac. You control it directly with the `herdr` CLI (no Cloudflare tunnel needed; that is only how Instinct, which runs elsewhere, reaches it): list agents, read their screens, prompt them, answer their permission prompts, start agents. Load the `herdr` skill first for the commands and the user's approval rules (approve read-only lookups such as MCP searches and read-only SQL; never approve writes, deletes, deploys, pushes or sending messages: tell the user).
- wacli ("wacky"): the WhatsApp CLI. Blackbone / My Network: the user's company and its network product. Onurb: you.
{MAC_NOTE}

Never spend money, accept terms, delete things, or send messages/emails yourself. Those need the user's explicit confirmation: use propose_whatsapp, or Instinct, whose own confirmation rule applies."""

TASK_PROMPT = """You cannot write or edit files, run shell commands other than `wacli --read-only` and `herdr` (see the herdr skill), or build/deploy anything; if a task needs that, say so in the result and stop.
You are Onurb's background worker for the user (Bruno). Do the task thoroughly with the tools you have (web research, reading WhatsApp with `wacli --read-only`, calendar, the browser via Claude in Chrome for computer use such as herdr). Read and navigate freely, but never submit forms, send messages or emails, purchase, accept terms, merge/deploy or delete anything; if the task needs one of those, stop and say what is needed (for WhatsApp, call propose_whatsapp: the user confirms on the watch). Email goes through route_to_instinct.

Names (speech-to-text often mangles them; map them back):
- Instinct ("instint", "instinto", "insting", "in sync"): the user's OTHER AI assistant, which lives on WhatsApp and has the user's mailbox, groceries, to-dos and reminders. "Manda pro Instinct" / "pede pro Instinct" / "manda um Instinct" = call route_to_instinct with the request. Never message Instinct through wacli.
- herdr ("hoarder", "herd", "herder", "Herd"): the user's terminal workspace running many Claude Code agents, on THIS Mac. You control it directly with the `herdr` CLI (no Cloudflare tunnel needed; that is only how Instinct, which runs elsewhere, reaches it): list agents, read their screens, prompt them, answer their permission prompts, start agents. Load the `herdr` skill first for the commands and the user's approval rules (approve read-only lookups such as MCP searches and read-only SQL; never approve writes, deletes, deploys, pushes or sending messages: tell the user).
- wacli ("wacky"): the WhatsApp CLI. Blackbone / My Network: the user's company and its network product. Onurb: you.
{MAC_NOTE}

Finish with a line `SPOKEN: <one or two short sentences>` in the user's language summarizing the result for the watch speaker. Put the full result above it."""
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

Finish with a line `SPOKEN: <one or two short sentences, in Portuguese>` saying what changed and whether it
was built (e.g. "Pronto: a tela das conversas agora apaga em 1 minuto; o relógio atualiza na próxima
checagem."). Put the details above it."""

TURN_PROMPT = TURN_PROMPT.replace("{MAC_NOTE}", MAC_NOTE)
TASK_PROMPT = TASK_PROMPT.replace("{MAC_NOTE}", MAC_NOTE)


# ------------------------------------------------------------------ state
LOCK = threading.Lock()
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
INSTINCT_WAIT = set()                # turns waiting for Instinct's real answer

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
    try:
        out = json.loads(r.stdout)
    except ValueError:
        raise RuntimeError(f"claude -p failed ({r.returncode}): {(r.stderr or r.stdout)[-300:]}")
    if out.get("is_error"):
        raise RuntimeError(f"agent error: {str(out.get('result'))[-300:]}")
    return (out.get("result") or "").strip(), out.get("session_id")


def thread_state(reply, turn):
    """(reply without the tag, state): open = needs the user, working = a background
    task it started is still running, done otherwise."""
    state = "done"
    if OPEN_TAG in reply:
        reply, state = reply.replace(OPEN_TAG, "").strip(), "open"
    elif reply.rstrip().endswith("?"):
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
    reply, state = thread_state(reply, "-")
    with LOCK:
        action = PROPOSED_BY_TURN.pop(f"follow-{turn[:8]}", None)
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


def run_turn(text, lang, turn, gateway, note="", thread=None):
    """Spoken turn. Returns (reply, action_id or None, thread state).
    thread: the turn id of an earlier request the user is answering on the watch."""
    with LOCK:
        th = dict(THREADS[thread]) if thread in THREADS else None
    if th:
        resume = th.get("sid")
        prompt = (f"(The user is answering inside the thread of their earlier request "
                  f"\"{th.get('heard', '')}\", where you replied \"{th.get('reply', '')}\".) {note}{text}")
    else:
        resume = SESSION["id"] if time.time() - SESSION["t"] < SESSION_GAP_S else None
        prompt = f"{note}{text}" if note else text
    t0 = time.monotonic()
    try:
        reply, sid = _claude(prompt, TURN_PROMPT, TURN_MODEL, READ_TOOLS + ONURB_TOOLS, turn, gateway, resume)
    except RuntimeError as e:
        if not resume:
            raise
        log(f"  resume failed ({e}); fresh session")
        reply, sid = _claude(prompt, TURN_PROMPT, TURN_MODEL, READ_TOOLS + ONURB_TOOLS, turn, gateway)
    SESSION.update(id=sid, t=time.time())
    log(f"  agent ({time.monotonic() - t0:.1f} s, {'resumed' if resume else 'new'} session"
        + (f", thread {thread[:8]}" if th else "") + ")")
    with LOCK:
        action = PROPOSED_BY_TURN.pop(turn, None)
    reply, state = thread_state(reply, turn)
    if th:      # background tasks / Instinct answers from a thread reply report to that thread
        with LOCK:
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
    return reply, new


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


def confirm(aid):
    """Send exactly the previewed messages. Returns (ok, detail)."""
    with LOCK:
        a = ACTIONS.get(aid)
        if a is None or a["state"] != "pending":
            return False, "not pending"
        a["state"] = "sending"
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
            "state": a["state"], "avatar": bool(a["avatar"]), "avatar_px": AVATAR_PX}


# ------------------------------------------------------------------ background tasks
def _kill(proc):
    """Stop a claude -p run and its children (MCP servers, Chrome bridge)."""
    try:
        os.killpg(proc.pid, 15)
    except (ProcessLookupError, PermissionError):
        pass


def list_tasks():
    with LOCK:
        return [{"id": tid, "state": t["state"], "task": t["task"][:200],
                 "minutes": round((time.monotonic() - t["started"]) / 60, 1)}
                for tid, t in TASKS.items()]


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
        try:
            text, _ = _claude(task, TASK_PROMPT, TASK_MODEL, READ_TOOLS + ONURB_TOOLS + ["mcp__claude-in-chrome"],
                              f"task-{tid}", gateway, timeout=TASK_TIMEOUT_S, chrome=True, on_start=started)
        except Exception as e:  # noqa: BLE001
            text = f"SPOKEN: A tarefa em segundo plano falhou: {str(e)[:80]}"
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
        (out / f"{time.strftime('%Y%m%d_%H%M%S')}_{tid}.md").write_text(f"# Task {tid}\n\n{task}\n\n---\n\n{text}\n")
        m = re.search(r"SPOKEN:\s*(.+)", text, re.S)
        spoken = (m.group(1) if m else text).strip().splitlines()[0][:300]
        with LOCK:
            thread = TASKS[tid].get("turn") or ""
            INBOX.append((spoken, lang_hint, tid, thread, PROPOSED_BY_TURN.pop(f"task-{tid}", None)))
        if thread in THREADS:
            _remember_thread(thread, reply=spoken, state="open" if spoken.rstrip().endswith("?") else "done")
        log(f"task {tid}: done in {(time.monotonic() - t0) / 60:.1f} min -> watch inbox: {spoken!r}")
    threading.Thread(target=work, daemon=True, name=f"task-{tid}").start()
    return tid


def _code_mtimes():
    return {str(f): f.stat().st_mtime for f in (REPO / "gateway").glob("*.py")}


def start_code_task(task, turn, gateway, lang_hint="pt"):
    """Edit/build Onurb itself (see CODE_TOOLS). One at a time; Mac only (the repo lives here)."""
    if HOST != "mac":
        return None, "code tasks run on the Mac (the repo and the firmware toolchain are there)"
    with LOCK:
        if any(t["state"] == "running" and t.get("kind") == "code" for t in TASKS.values()):
            return None, "another code task is still running; try again when it finishes"
        tid = uuid.uuid4().hex[:8]
        TASKS[tid] = {"task": task, "started": time.monotonic(), "state": "running", "proc": None,
                      "turn": turn, "kind": "code"}
    def started(proc):
        with LOCK:
            TASKS[tid]["proc"] = proc
            cancelled = TASKS[tid]["state"] == "cancelled"
        if cancelled:
            _kill(proc)
    def work():
        t0 = time.monotonic()
        before = _code_mtimes()
        log(f"code task {tid}: started: {task[:120]!r}")
        try:
            text, _ = _claude(task, CODE_PROMPT, TASK_MODEL, CODE_TOOLS, f"code-{tid}", gateway,
                              timeout=CODE_TIMEOUT_S, on_start=started, never=CODE_NEVER, cwd=REPO)
        except Exception as e:  # noqa: BLE001
            text = f"SPOKEN: A mudança no Onurb falhou: {str(e)[:80]}"
        with LOCK:
            cancelled = TASKS[tid]["state"] == "cancelled"
            TASKS[tid]["proc"] = None
            if not cancelled:
                TASKS[tid]["state"] = "done"
        if cancelled:
            log(f"code task {tid}: cancelled after {(time.monotonic() - t0) / 60:.1f} min")
            return
        out = RECEIVED / "tasks"
        out.mkdir(exist_ok=True)
        (out / f"{time.strftime('%Y%m%d_%H%M%S')}_code_{tid}.md").write_text(f"# Code task {tid}\n\n{task}\n\n---\n\n{text}\n")
        m = re.search(r"SPOKEN:\s*(.+)", text, re.S)
        spoken = (m.group(1) if m else text).strip().splitlines()[0][:300]
        with LOCK:
            thread = TASKS[tid].get("turn") or ""
            INBOX.append((spoken, lang_hint, tid, thread, None))
        if thread in THREADS:
            _remember_thread(thread, reply=spoken, state="done")
        log(f"code task {tid}: done in {(time.monotonic() - t0) / 60:.1f} min -> watch inbox: {spoken!r}")
        if _code_mtimes() != before and RESTART_HOOK:
            # The gateway's own code changed: restart once the watch has the result (the inbox is in memory).
            for _ in range(180):
                with LOCK:
                    if not any(item[2] == tid for item in INBOX):
                        break
                time.sleep(1)
            # A restart kills running tasks (they are the gateway's children): wait for them
            # (lost a background task this way on 2026-11-11).
            for _ in range(60 * 60):
                with LOCK:
                    if not any(t["state"] == "running" for t in TASKS.values()):
                        break
                time.sleep(1)
            log(f"code task {tid}: gateway code changed - restarting the gateway")
            RESTART_HOOK()
    threading.Thread(target=work, daemon=True, name=f"code-{tid}").start()
    return tid, None


def inbox_pop():
    with LOCK:
        return INBOX.popleft() if INBOX else None
