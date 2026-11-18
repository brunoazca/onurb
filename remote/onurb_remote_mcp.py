#!/usr/bin/env python3
"""Mac side of the Mac -> server bridge (remote/README.md): a stdio MCP server for the user's
Claude Code sessions on the Mac. Each tool is one SSH call to remote/onurb_remote.py on the server,
with a key the server pins to that script.

Config (private, never in the repo): ~/.config/onurb/remote.env (ONURB_REMOTE_SSH=onurb@<host>)
and the key ~/.config/onurb/remote_key. Register once:
    claude mcp add --scope user onurb-server -- python3 <repo>/remote/onurb_remote_mcp.py
Stdlib only.
"""
import base64
import json
import os
import subprocess
import sys
from pathlib import Path

CFG = Path.home() / ".config" / "onurb"
KEY = CFG / "remote_key"
MAX_FILE = 25 * 1024 * 1024


def _host():
    env = CFG / "remote.env"
    if env.exists():
        for line in env.read_text().splitlines():
            k, _, v = line.partition("=")
            if k.strip() == "ONURB_REMOTE_SSH":
                return v.strip().strip('"')
    return os.environ.get("ONURB_REMOTE_SSH", "")


def remote(cmd, payload):
    host = _host()
    if not host or not KEY.exists():
        return {"error": "bridge not configured (~/.config/onurb/remote.env and remote_key)"}
    r = subprocess.run(["ssh", "-i", str(KEY), "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
                        "-o", "ConnectTimeout=15", host, cmd],
                       input=json.dumps(payload), capture_output=True, text=True, timeout=120)
    lines = r.stdout.strip().splitlines()
    try:
        return json.loads(lines[-1])
    except (IndexError, ValueError):
        return {"error": (r.stderr or r.stdout).strip()[-300:] or f"ssh exit {r.returncode}"}


TOOLS = [
    {"name": "server_send",
     "description": ("Copy a local file (path) or a text (name + text) to the Onurb server's ~/inbox/, "
                     "where server sessions can read it. Max 25 MB."),
     "inputSchema": {"type": "object", "properties": {
         "path": {"type": "string", "description": "local file to copy"},
         "name": {"type": "string", "description": "file name on the server (for text, or to rename)"},
         "text": {"type": "string", "description": "content to send instead of a file"}}}},
    {"name": "server_start_session",
     "description": ("Start a Claude Code session on the Onurb server (always on; keeps running if the Mac "
                     "sleeps). It has the watch agent's tools: WhatsApp (read; sending and purchases only as "
                     "previews the user confirms on the watch), the server's Chrome, herdr via the Mac, Notion, "
                     "calendar, web, and read/write in ~/work. Returns a session id. Give a complete prompt: it "
                     "starts with no context."),
     "inputSchema": {"type": "object", "properties": {
         "prompt": {"type": "string"}, "title": {"type": "string", "description": "short label"}},
         "required": ["prompt"]}},
    {"name": "server_message",
     "description": ("Send a follow-up to a server session (resumes it with full context; queued if it is "
                     "still running). Ids starting with onurb- are Onurb's own background tasks."),
     "inputSchema": {"type": "object", "properties": {"id": {"type": "string"}, "text": {"type": "string"}},
                     "required": ["id", "text"]}},
    {"name": "server_sessions",
     "description": ("List sessions on the server: ones started through this bridge, Onurb's background tasks "
                     "(onurb-<id>), and other recent Claude Code transcripts there."),
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "server_read",
     "description": ("Read a server session: mode 'result' (default: its latest result/answer) or 'full' "
                     "(transcript with tool calls), last `tail` blocks (default 40). Works for bridge sessions, "
                     "onurb-<id> tasks and any transcript id from server_sessions."),
     "inputSchema": {"type": "object", "properties": {
         "id": {"type": "string"}, "mode": {"type": "string", "enum": ["result", "full"]},
         "tail": {"type": "integer"}}, "required": ["id"]}},
    {"name": "server_cancel",
     "description": "Stop a server session (or an onurb-<id> task).",
     "inputSchema": {"type": "object", "properties": {"id": {"type": "string"}}, "required": ["id"]}},
]


def run_tool(name, a):
    if name == "server_send":
        if a.get("path"):
            p = Path(os.path.expanduser(a["path"]))
            if not p.is_file():
                return {"error": f"no such file: {p}"}
            if p.stat().st_size > MAX_FILE:
                return {"error": "file over 25 MB"}
            data, fname = p.read_bytes(), a.get("name") or p.name
        elif a.get("text") is not None:
            data, fname = a["text"].encode(), a.get("name") or "note.txt"
        else:
            return {"error": "give path or text"}
        return remote("send", {"name": fname, "content_b64": base64.b64encode(data).decode()})
    if name == "server_start_session":
        return remote("start", {"prompt": a.get("prompt", ""), "title": a.get("title", "")})
    if name == "server_message":
        return remote("message", {"id": a.get("id", ""), "text": a.get("text", "")})
    if name == "server_sessions":
        return remote("sessions", {})
    if name == "server_read":
        return remote("read", {"id": a.get("id", ""), "mode": a.get("mode") or "result", "tail": a.get("tail") or 40})
    if name == "server_cancel":
        return remote("cancel", {"id": a.get("id", "")})
    raise ValueError(f"unknown tool {name}")


def reply(msg_id, result=None, error=None):
    out = {"jsonrpc": "2.0", "id": msg_id}
    if error:
        out["error"] = {"code": -32603, "message": error}
    else:
        out["result"] = result
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        mid, method = msg.get("id"), msg.get("method")
        if method == "initialize":
            reply(mid, {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2024-11-05"),
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": "onurb-server", "version": "1.0"}})
        elif method == "tools/list":
            reply(mid, {"tools": TOOLS})
        elif method == "tools/call":
            p = msg.get("params", {})
            try:
                r = run_tool(p.get("name"), p.get("arguments") or {})
                text = r if isinstance(r, str) else json.dumps(r, ensure_ascii=False, indent=1)
                reply(mid, {"content": [{"type": "text", "text": text}], "isError": isinstance(r, dict) and "error" in r})
            except Exception as e:  # noqa: BLE001
                reply(mid, {"content": [{"type": "text", "text": f"error: {e}"}], "isError": True})
        elif mid is not None:
            reply(mid, {} if method == "ping" else None, None if method == "ping" else f"unknown method {method}")


if __name__ == "__main__":
    main()
