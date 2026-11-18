#!/usr/bin/env python3
"""Server side of the Mac -> server bridge (remote/README.md). Runs as `onurb` on the server.

Called over SSH with a key that authorized_keys pins to this file (`command=`), so the Mac can
only run these commands (SSH_ORIGINAL_COMMAND), with a JSON payload on stdin:

  send     {name, content_b64}            write a file into ~/inbox/
  start    {prompt, title}                start a Claude Code session; returns its id
  message  {id, text}                     follow-up (queued while the session is running)
  sessions {}                             this bridge's sessions + Onurb tasks + other recent ones
  read     {id, mode: result|full, tail}  a session's result or transcript
  cancel   {id}                           stop it
  run <id>                                (internal) the detached runner

Sessions get the watch agent's tools and rules (gateway/onurb_agent.py: WhatsApp, browser, herdr,
Notion, calendar...) plus read/write in ~/work. WhatsApp messages and purchases stay previews that
the user confirms on the watch. Onurb's own background tasks show up as "onurb-<id>".
"""
import base64
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request
import uuid
from pathlib import Path

HOME = Path.home()
ROOT = HOME / "remote"                      # one folder per session: meta.json, out.jsonl, queue/
INBOX = HOME / "inbox"
WORK = HOME / "work"
TASKS_DIR = HOME / "onurb" / "gateway" / "received" / "tasks"
GATEWAY = "http://127.0.0.1:8080"
CLAUDE = str(HOME / ".npm-global" / "bin" / "claude")
MODEL = "opus"
MAX_FILE = 25 * 1024 * 1024
RUN_TIMEOUT_S = 60 * 60

os.environ["PATH"] = f"{HOME}/.npm-global/bin:/usr/local/bin:/usr/bin:/bin"
os.environ.setdefault("ONURB_HOST", "server")     # the agent prompt: what needs the Mac
os.environ.setdefault("DISPLAY", ":1")           # the server Chrome (Claude in Chrome)

sys.path.insert(0, str(HOME / "onurb" / "gateway"))


def agent_args():
    """The watch agent's toolset (gateway/onurb_agent.py), plus read/write in ~/work: WhatsApp and
    purchases still only as previews the user confirms on the watch."""
    import onurb_agent as A
    tools = (A.READ_TOOLS + [t for t in A.ONURB_TOOLS if t != "mcp__onurb__start_code_task"]
             + ["mcp__claude-in-chrome", f"Read(/{WORK}/**)", f"Read(/{INBOX}/**)",
                f"Edit(/{WORK}/**)", f"Write(/{WORK}/**)", "TodoWrite"])
    never = [n for n in A.NEVER if n not in ("Write", "Edit")] + [
        f"Read(/{HOME}/.config/**)", f"Edit(/{HOME}/onurb/**)", f"Write(/{HOME}/onurb/**)",
        f"Edit(/{HOME}/.claude/**)", f"Write(/{HOME}/.claude/**)", f"Read(/{HOME}/.ssh/**)"]
    system = (A.TASK_PROMPT + "\n\nThis session was started remotely by one of the user's Claude Code sessions on "
              f"their Mac (the Mac-to-server bridge), not by voice. Exception to the file rule above: you may "
              f"read and write files in {WORK}; files the Mac sent are in {INBOX}. WhatsApp messages and "
              "purchases you propose go to the user's watch for Confirm. Finish with a clear summary of what "
              "you did and where the output is (the SPOKEN line is optional here).")
    system = system.replace("{NOW_LOCAL}", time.strftime("%A %Y-%m-%d %H:%M"))
    return tools, never, system, A


def out(obj, code=0):
    print(json.dumps(obj, ensure_ascii=False))
    sys.exit(code)


def safe_id(s):
    s = re.sub(r"[^0-9a-zA-Z_-]", "", str(s))[:64]
    if not s:
        out({"error": "id required"}, 2)
    return s


def meta_path(sid):
    return ROOT / sid / "meta.json"


def load_meta(sid):
    try:
        return json.loads(meta_path(sid).read_text())
    except (OSError, ValueError):
        return None


def save_meta(m):
    p = meta_path(m["id"])
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(m, ensure_ascii=False, indent=1))
    os.replace(tmp, p)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError, TypeError):
        return False


def gateway(path, payload):
    req = urllib.request.Request(GATEWAY + path, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


# ------------------------------------------------------------------ commands
def _spawn_runner(sid):
    """Detached runner. Without SSH_ORIGINAL_COMMAND (else it would read itself as an SSH command)."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("SSH_")}
    return subprocess.Popen([sys.executable, __file__, "run", sid], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=open(ROOT / sid / "runner.log", "a"),
                            start_new_session=True, cwd=str(WORK), env=env)


def cmd_send(p):
    name = re.sub(r"[^0-9A-Za-z._ -]", "_", Path(str(p.get("name", ""))).name)[:120].strip(" .")
    if not name:
        out({"error": "name required"}, 2)
    data = base64.b64decode(p.get("content_b64", ""))
    if len(data) > MAX_FILE:
        out({"error": f"file over {MAX_FILE // 1024 // 1024} MB"}, 2)
    INBOX.mkdir(exist_ok=True)
    dst = INBOX / name
    if dst.exists():
        dst = INBOX / f"{dst.stem}-{time.strftime('%Y%m%d%H%M%S')}{dst.suffix}"
    dst.write_bytes(data)
    out({"path": str(dst), "bytes": len(data)})


def cmd_start(p):
    prompt = str(p.get("prompt", "")).strip()
    if not prompt:
        out({"error": "prompt required"}, 2)
    title = str(p.get("title") or prompt.splitlines()[0])[:80]
    sid = uuid.uuid4().hex[:10]
    (ROOT / sid / "queue").mkdir(parents=True)
    WORK.mkdir(exist_ok=True)
    save_meta({"id": sid, "title": title, "state": "running", "created": time.time(),
               "claude_session": None, "pid": None, "runs": 0})
    (ROOT / sid / "queue" / f"{time.time():.6f}.txt").write_text(prompt)
    proc = _spawn_runner(sid)
    m = load_meta(sid)
    m["pid"] = proc.pid
    save_meta(m)
    out({"id": sid})


def cmd_run(sid):
    """Detached runner: one `claude -p` per queued message, resuming the same Claude session."""
    d = ROOT / sid
    while True:
        q = sorted((d / "queue").glob("*.txt"))
        m = load_meta(sid)
        if m is None or m["state"] == "cancelled":
            return
        if not q:
            m["state"], m["pid"] = "done", None
            save_meta(m)
            return
        text = "\n\n".join(f.read_text() for f in q)
        for f in q:
            f.unlink()
        tools, never, system, A = agent_args()
        mcp = A._mcp_config(f"remote-{sid}", GATEWAY)
        args = [CLAUDE, "-p", text, "--output-format", "stream-json", "--verbose", "--model", MODEL,
                "--append-system-prompt", system, "--mcp-config", mcp, "--chrome",
                "--allowedTools", *tools, "--disallowedTools", *never, *A.SANDBOX]
        if m.get("claude_session"):
            args += ["--resume", m["claude_session"]]
        with open(d / "out.jsonl", "a") as fh:
            fh.write(json.dumps({"type": "user_prompt", "text": text, "t": time.time()}) + "\n")
            fh.flush()
            proc = subprocess.Popen(args, stdout=fh, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                    cwd=str(A.RECEIVED), start_new_session=True)   # cwd: the agent's skills
            m["state"], m["child"], m["runs"] = "running", proc.pid, m.get("runs", 0) + 1
            save_meta(m)
            try:
                proc.wait(timeout=RUN_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGTERM)
                proc.wait()
        m = load_meta(sid) or m
        for ev in _events(sid):
            if ev.get("session_id"):
                m["claude_session"] = ev["session_id"]
        m["child"] = None
        save_meta(m)
        try:                       # a WhatsApp/purchase preview it proposed -> the watch
            gateway("/internal/remote_done", {"turn": f"remote-{sid}", "title": m["title"]})
        except OSError:
            pass


def _events(sid):
    f = ROOT / sid / "out.jsonl"
    if not f.exists():
        return []
    evs = []
    for line in f.read_text(errors="replace").splitlines():
        try:
            evs.append(json.loads(line))
        except ValueError:
            pass
    return evs


def cmd_message(p):
    sid = safe_id(p.get("id"))
    text = str(p.get("text", "")).strip()
    if not text:
        out({"error": "text required"}, 2)
    if sid.startswith("onurb-"):
        r = gateway("/internal/tasks/add", {"task_id": sid[6:], "text": text, "turn": ""})
        out({"result": r.get("result")})
    m = load_meta(sid)
    if m is None:
        out({"error": "no such session"}, 2)
    if m["state"] == "cancelled":
        out({"error": "session was cancelled"}, 2)
    (ROOT / sid / "queue" / f"{time.time():.6f}.txt").write_text(text)
    if m["state"] == "running" and alive(m.get("pid")):
        out({"result": "queued: the session takes it when its current run ends"})
    proc = _spawn_runner(sid)
    m["state"], m["pid"] = "running", proc.pid
    save_meta(m)
    out({"result": "resumed with your message"})


def _render(evs, full):
    """stream-json events -> readable lines (user prompts, assistant text, tool calls, result)."""
    lines = []
    for ev in evs:
        t = ev.get("type")
        if t == "user_prompt":
            lines.append(f"## user\n{ev['text']}")
        elif t == "assistant" and full:
            for c in (ev.get("message") or {}).get("content") or []:
                if c.get("type") == "text" and c.get("text", "").strip():
                    lines.append(f"## assistant\n{c['text']}")
                elif c.get("type") == "tool_use":
                    lines.append(f"- tool {c.get('name')}: {json.dumps(c.get('input'), ensure_ascii=False)[:200]}")
        elif t == "result":
            lines.append(f"## result ({ev.get('subtype')}, {round((ev.get('duration_ms') or 0) / 1000)} s)\n"
                         f"{ev.get('result', '')}")
    return lines


def _transcript_file(uid):
    for f in (HOME / ".claude" / "projects").glob(f"*/{uid}.jsonl"):
        return f
    return None


def _render_transcript(f, full):
    lines = []
    for raw in f.read_text(errors="replace").splitlines():
        try:
            ev = json.loads(raw)
        except ValueError:
            continue
        msg = ev.get("message") or {}
        content = msg.get("content")
        if ev.get("type") == "user":
            if isinstance(content, str) and content.strip():
                lines.append(f"## user\n{content}")
        elif ev.get("type") == "assistant":
            for c in content or []:
                if c.get("type") == "text" and c.get("text", "").strip():
                    lines.append(f"## assistant\n{c['text']}")
                elif c.get("type") == "tool_use" and full:
                    lines.append(f"- tool {c.get('name')}: {json.dumps(c.get('input'), ensure_ascii=False)[:200]}")
    return lines


def cmd_read(p):
    sid = safe_id(p.get("id"))
    full = p.get("mode") == "full"
    tail = max(1, min(int(p.get("tail") or 40), 400))
    if sid.startswith("onurb-"):
        tid = sid[6:]
        files = sorted(TASKS_DIR.glob(f"*_{tid}.md"))
        if files:
            out({"state": "done", "text": files[-1].read_text()[-60000:]})
        tasks = gateway("/internal/tasks", {}).get("tasks", [])
        t = next((t for t in tasks if t["id"] == tid), None)
        out({"state": t["state"] if t else "unknown", "text": ""})
    m = load_meta(sid)
    if m is not None:
        if m["state"] == "running" and not alive(m.get("pid")):
            m["state"] = "done"
        lines = _render(_events(sid), full)
        if not full:
            res = [l for l in lines if l.startswith("## result")]
            lines = res[-1:] if res else ["(no result yet)"]
        out({"state": m["state"], "title": m["title"], "text": "\n\n".join(lines[-tail:])[-60000:]})
    f = _transcript_file(sid)                    # any other Claude Code session on the server
    if f is None:
        out({"error": "no such session"}, 2)
    lines = _render_transcript(f, full)
    if not full:
        lines = [l for l in lines if l.startswith("## assistant")][-1:]
    out({"state": "transcript", "text": "\n\n".join(lines[-tail:])[-60000:]})


def cmd_sessions(_p):
    mine = []
    if ROOT.exists():
        for d in sorted(ROOT.iterdir(), key=lambda d: d.stat().st_mtime, reverse=True)[:30]:
            m = load_meta(d.name)
            if m:
                state = m["state"] if (m["state"] != "running" or alive(m.get("pid"))) else "done"
                mine.append({"id": m["id"], "title": m["title"], "state": state,
                             "started": time.strftime("%Y-%m-%d %H:%M", time.localtime(m["created"]))})
    try:
        onurb = [{"id": f"onurb-{t['id']}", "title": t["task"][:80], "state": t["state"], "kind": t["kind"]}
                 for t in gateway("/internal/tasks", {}).get("tasks", [])]
    except OSError:
        onurb = []
    other = []
    proj = HOME / ".claude" / "projects"
    if proj.exists():
        for f in sorted(proj.glob("*/*.jsonl"), key=lambda f: f.stat().st_mtime, reverse=True)[:15]:
            first = ""
            with open(f, errors="replace") as fh:
                for raw in fh:
                    try:
                        ev = json.loads(raw)
                    except ValueError:
                        continue
                    c = (ev.get("message") or {}).get("content")
                    if ev.get("type") == "user" and isinstance(c, str):
                        first = c.strip().splitlines()[0][:80] if c.strip() else ""
                        break
            other.append({"id": f.stem, "title": first, "updated": time.strftime(
                "%Y-%m-%d %H:%M", time.localtime(f.stat().st_mtime))})
    out({"bridge": mine, "onurb_tasks": onurb, "recent_transcripts": other})


def cmd_cancel(p):
    sid = safe_id(p.get("id"))
    if sid.startswith("onurb-"):
        out({"result": gateway("/internal/tasks/cancel", {"task_id": sid[6:]}).get("result")})
    m = load_meta(sid)
    if m is None:
        out({"error": "no such session"}, 2)
    m["state"] = "cancelled"
    save_meta(m)
    for pid in (m.get("child"), m.get("pid")):
        if pid and alive(pid):
            try:
                os.killpg(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    out({"result": "cancelled"})


def main():
    cmd = (os.environ.get("SSH_ORIGINAL_COMMAND") or " ".join(sys.argv[1:])).split()
    if not cmd:
        out({"error": "no command"}, 2)
    ROOT.mkdir(exist_ok=True)
    if cmd[0] == "run" and len(cmd) == 2 and not os.environ.get("SSH_ORIGINAL_COMMAND"):
        return cmd_run(safe_id(cmd[1]))
    handlers = {"send": cmd_send, "start": cmd_start, "message": cmd_message, "sessions": cmd_sessions,
                "read": cmd_read, "cancel": cmd_cancel}
    if cmd[0] not in handlers or len(cmd) != 1:
        out({"error": "unknown command"}, 2)
    try:
        payload = json.loads(sys.stdin.read(40 * 1024 * 1024) or "{}")
    except ValueError:
        out({"error": "bad json"}, 2)
    handlers[cmd[0]](payload if isinstance(payload, dict) else {})


if __name__ == "__main__":
    main()
