#!/usr/bin/env python3
"""Live view of Onurb's agent (docs/AGENT_B.md) for a terminal tab (e.g. herdr).

The gateway runs the agent as headless `claude -p` sessions with cwd
gateway/received; Claude Code records each session as JSONL under
~/.claude/projects/<that dir>/. This follows those files (spoken turns and
background tasks) and prints what the agent hears, the tools it calls, what
they return and what it answers. With --gateway-log it also interleaves the
gateway's own log (what the watch sent, transcripts, WhatsApp proposals).

Usage: tools/onurb_agent_watch.py [--gateway-log PATH] [--since-min 30]
"""
import argparse
import json
import os
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECT_DIR = Path.home() / ".claude" / "projects" / str(ROOT / "gateway" / "received").replace("/", "-")

DIM, BOLD, CYAN, GREEN, YELLOW, MAG, RESET = "\033[2m", "\033[1m", "\033[36m", "\033[32m", "\033[33m", "\033[35m", "\033[0m"


def short(x, n=220):
    s = x if isinstance(x, str) else json.dumps(x, ensure_ascii=False)
    s = " ".join(s.split())
    return s if len(s) <= n else s[:n] + "…"


def render(entry, sid):
    t, m = entry.get("type"), entry.get("message") or {}
    content = m.get("content")
    tag = f"{DIM}[{sid[:6]}]{RESET}"
    out = []
    if t == "user":
        if isinstance(content, str):
            out.append(f"{tag} {BOLD}{CYAN}▶ {short(content, 600)}{RESET}")
        for c in content if isinstance(content, list) else []:
            if c.get("type") == "text":
                out.append(f"{tag} {BOLD}{CYAN}▶ {short(c.get('text', ''), 600)}{RESET}")
            elif c.get("type") == "tool_result":
                res = c.get("content")
                if isinstance(res, list):
                    res = " ".join(x.get("text", "") for x in res if isinstance(x, dict))
                out.append(f"{tag} {DIM}  ↳ {short(res or '', 260)}{RESET}")
    elif t == "assistant":
        for c in content if isinstance(content, list) else []:
            if c.get("type") == "text" and c.get("text", "").strip():
                out.append(f"{tag} {GREEN}◀ {short(c['text'], 800)}{RESET}")
            elif c.get("type") == "tool_use":
                name = c.get("name", "").replace("mcp__claude_ai_", "").replace("mcp__", "")
                out.append(f"{tag} {YELLOW}⚙ {name}{RESET} {DIM}{short(c.get('input', {}), 200)}{RESET}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway-log", type=Path, help="also follow the gateway log")
    ap.add_argument("--since-min", type=float, default=30, help="replay sessions touched in the last N minutes")
    args = ap.parse_args()

    print(f"{BOLD}Onurb agent — live{RESET}  {DIM}{PROJECT_DIR}{RESET}\n")
    offsets = {}
    cutoff = time.time() - args.since_min * 60
    glog = args.gateway_log
    glog_off = glog.stat().st_size if glog and glog.exists() else 0
    while True:
        files = sorted(PROJECT_DIR.glob("*.jsonl"), key=lambda p: p.stat().st_mtime) if PROJECT_DIR.exists() else []
        for f in files:
            st = f.stat()
            if f not in offsets:
                offsets[f] = 0 if st.st_mtime >= cutoff else st.st_size
            if st.st_size <= offsets[f]:
                continue
            with open(f) as fh:
                fh.seek(offsets[f])
                chunk = fh.read()
            complete, _, rest = chunk.rpartition("\n")
            offsets[f] += len(complete.encode()) + (1 if complete else 0)
            for line in complete.splitlines():
                try:
                    for row in render(json.loads(line), f.stem):
                        print(row, flush=True)
                except ValueError:
                    pass
        if glog and glog.exists() and glog.stat().st_size > glog_off:
            with open(glog, errors="replace") as fh:
                fh.seek(glog_off)
                data = fh.read()
            glog_off += len(data.encode(errors="replace"))
            for line in data.splitlines():
                if any(k in line for k in ("turn ", "heard", "reply", "proposal", "action ", "task ", "inbox", "Instinct", "edit ")):
                    print(f"{MAG}{line}{RESET}", flush=True)
        time.sleep(0.5)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
