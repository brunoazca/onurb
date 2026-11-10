#!/usr/bin/env python3
"""Open one of Onurb's agent sessions (docs/AGENT_B.md) interactively, with the
same tools and limits the gateway gives it: Onurb's MCP (route_to_instinct,
propose_whatsapp, background tasks incl. list/cancel), My Network, read-only
wacli, Chrome; wacli send / Write / Edit / Task denied, dontAsk sandbox.

Usage: tools/onurb_chat.py [SESSION_ID_PREFIX]   (default: the newest session)
The id prefix is the [xxxxxx] tag shown by tools/onurb_agent_watch.py.
"""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "gateway"))
import onurb_agent as agent  # noqa: E402

PROJECT_DIR = Path.home() / ".claude" / "projects" / str(agent.RECEIVED).replace("/", "-")


def main():
    prefix = sys.argv[1] if len(sys.argv) > 1 else ""
    files = sorted(PROJECT_DIR.glob(f"{prefix}*.jsonl"), key=lambda p: p.stat().st_mtime)
    if not files:
        sys.exit(f"no session matching {prefix!r} in {PROJECT_DIR}")
    sid = files[-1].stem
    mcp = agent._mcp_config("chat", "http://127.0.0.1:8080")   # small temp file, left for the session
    tools = agent.READ_TOOLS + agent.ONURB_TOOLS + ["mcp__claude-in-chrome"]
    print(f"resuming {sid} (model {agent.TASK_MODEL})", file=sys.stderr)
    os.chdir(agent.RECEIVED)
    os.execvp("claude", ["claude", "--resume", sid, "--model", agent.TASK_MODEL, "--chrome",
                         "--mcp-config", mcp, "--allowedTools", *tools,
                         "--disallowedTools", *agent.NEVER, *agent.SANDBOX])


if __name__ == "__main__":
    main()
