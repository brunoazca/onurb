#!/usr/bin/env python3
"""Onurb's tools for the agent (docs/AGENT_B.md), as a minimal stdio MCP server.

The agent (claude -p, started by the gateway) can NOT send WhatsApp messages or
email itself: `wacli send` is disallowed. It can only *propose* a WhatsApp
message; the gateway shows the preview on the watch and sends exactly that text
only after the user taps Confirm there.

Tools (each one calls the gateway's localhost-only /internal/* endpoints):
  route_to_instinct(text)              hand the request to Instinct, get its reply
  propose_whatsapp(to, messages[])     preview on the watch; sent only on Confirm
  start_background_task(task)          long work (deep research, computer use);
                                       the result reaches the watch inbox later

Stdlib only. The gateway passes ONURB_GATEWAY and ONURB_TURN_ID in the env.
"""
import json
import os
import sys
import urllib.request

GATEWAY = os.environ.get("ONURB_GATEWAY", "http://127.0.0.1:8080")
TURN = os.environ.get("ONURB_TURN_ID", "")

TOOLS = [
    {
        "name": "route_to_instinct",
        "description": (
            "Send the request to Instinct (the user's other assistant) and return its reply. "
            "Use for anything about the user's email (Instinct has the personal mailbox that "
            "receives all the other accounts), groceries/shopping list, to-dos, reminders, or "
            "anything Instinct already handles better. Pass the request in the user's words."),
        "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}},
                        "required": ["text"]},
    },
    {
        "name": "propose_whatsapp",
        "description": (
            "Propose WhatsApp message(s) to a contact. This does NOT send: the watch shows a "
            "preview (bubbles, the contact's photo and name) with Edit / Confirm / Cancel, and "
            "only the user's Confirm sends exactly these messages. `to` is a contact name, "
            "phone number or JID; if several contacts match you get the candidates back and "
            "must ask the user which one. `messages` is the list of separate bubbles, in "
            "order, written as the user would write them."),
        "inputSchema": {"type": "object",
                        "properties": {"to": {"type": "string"},
                                       "messages": {"type": "array", "items": {"type": "string"}}},
                        "required": ["to", "messages"]},
    },
    {
        "name": "start_background_task",
        "description": (
            "Start long work that would not fit in a spoken reply: deep research, multi-step "
            "web work, computer use (e.g. herdr in the browser). Returns immediately; the result "
            "is delivered to the user's watch later. Describe the task completely (it runs in a "
            "fresh session). Then tell the user briefly that you're on it."),
        "inputSchema": {"type": "object", "properties": {"task": {"type": "string"}},
                        "required": ["task"]},
    },
    {
        "name": "start_code_task",
        "description": (
            "Change Onurb itself: the watch firmware (screens, buttons, timeouts, behavior), the "
            "gateway, or your own prompts/tools. Pre-approved by the user: no confirmation needed. "
            "A code worker edits the Onurb repo, builds the firmware (the watch updates itself) and "
            "the result reaches the watch when done. Describe the change completely: what the user "
            "wants, in their words, which screen/behavior, and edge cases."),
        "inputSchema": {"type": "object", "properties": {"task": {"type": "string"}},
                        "required": ["task"]},
    },
    {
        "name": "list_background_tasks",
        "description": ("List the background tasks started since the gateway last restarted: id, "
                        "state (running / done / cancelled), minutes since start and the task text."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "cancel_background_task",
        "description": ("Stop a running background task (when the user says cancel/stop it). Its "
                        "result is dropped and nothing reaches the watch. Use the id from "
                        "start_background_task or list_background_tasks."),
        "inputSchema": {"type": "object", "properties": {"task_id": {"type": "string"}},
                        "required": ["task_id"]},
    },
]


def call_gateway(path, payload):
    req = urllib.request.Request(GATEWAY + path, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=180) as r:
        return json.loads(r.read())


def run_tool(name, args):
    if name == "route_to_instinct":
        r = call_gateway("/internal/instinct", {"text": args.get("text", ""), "turn": TURN})
        return r.get("reply", "")
    if name == "propose_whatsapp":
        r = call_gateway("/internal/propose_whatsapp",
                         {"to": args.get("to", ""), "messages": args.get("messages", []), "turn": TURN})
        if r.get("candidates"):
            return "Several contacts match; ask the user which one: " + "; ".join(r["candidates"])
        if r.get("error"):
            return "Not proposed: " + r["error"]
        return (f"Preview sent to the watch for {r['name']} ({len(args.get('messages', []))} "
                "message(s)). Nothing is sent until the user taps Confirm. Tell the user to check "
                "the watch; do not try to send it any other way.")
    if name == "start_background_task":
        r = call_gateway("/internal/background", {"task": args.get("task", ""), "turn": TURN})
        return f"Started (task {r.get('task_id')}). The result will reach the user's watch when done."
    if name == "start_code_task":
        r = call_gateway("/internal/code", {"task": args.get("task", ""), "turn": TURN})
        if r.get("error"):
            return "Not started: " + r["error"]
        return f"Started (code task {r.get('task_id')}). The result will reach the user's watch when done."
    if name == "list_background_tasks":
        tasks = call_gateway("/internal/tasks", {}).get("tasks", [])
        return json.dumps(tasks, ensure_ascii=False) if tasks else "No background tasks since the gateway started."
    if name == "cancel_background_task":
        r = call_gateway("/internal/tasks/cancel", {"task_id": args.get("task_id", "")})
        return f"Task {args.get('task_id')}: {r.get('result')}"
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
        method, msg_id = msg.get("method"), msg.get("id")
        if msg_id is None:                      # notifications (initialized, cancelled, ...)
            continue
        if method == "initialize":
            reply(msg_id, {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2025-06-18"),
                           "capabilities": {"tools": {}},
                           "serverInfo": {"name": "onurb", "version": "0.1"}})
        elif method == "tools/list":
            reply(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            p = msg.get("params", {})
            try:
                text = run_tool(p.get("name"), p.get("arguments") or {})
                reply(msg_id, {"content": [{"type": "text", "text": text}]})
            except Exception as e:  # noqa: BLE001 - report to the agent, keep serving
                reply(msg_id, {"content": [{"type": "text", "text": f"Tool error: {e}"}], "isError": True})
        elif method == "ping":
            reply(msg_id, {})
        else:
            reply(msg_id, error=f"method not found: {method}")


if __name__ == "__main__":
    main()
