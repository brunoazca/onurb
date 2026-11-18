# Mac → server bridge

Lets the user's Claude Code sessions on the Mac hand work to the Onurb server, which is always
on: send files there, start Claude Code sessions there, follow up on them, read their transcripts.

```
Claude Code (Mac) ──MCP stdio──▶ onurb_remote_mcp.py ──ssh (pinned key)──▶ onurb_remote.py (server, user onurb)
                                                                             ├─ ~/inbox/   files from the Mac
                                                                             ├─ ~/work/    sessions' workspace
                                                                             └─ ~/remote/<id>/  meta, queue, out.jsonl
```

## Tools (MCP server `onurb-server`)

| Tool | What it does |
|---|---|
| `server_send` | Copy a local file, or a text, to `~/inbox/` on the server (max 25 MB) |
| `server_start_session` | Start a Claude Code session there; returns an id. It runs detached, so it survives the Mac sleeping |
| `server_message` | Follow up on a session: resumes it with its full context (queued if it is still running) |
| `server_sessions` | This bridge's sessions, Onurb's background tasks (`onurb-<id>`), other recent transcripts |
| `server_read` | A session's latest result, or the full transcript with tool calls (`mode: full`) |
| `server_cancel` | Stop a session or an Onurb task |

## What a session can do

The watch agent's toolset and rules (`gateway/onurb_agent.py`: WhatsApp read, the server's Chrome,
Notion, calendar, Granola, web; herdr is Mac-only, so not here) plus read/write in `~/work`. WhatsApp
messages and purchases stay **previews that the user confirms on the watch**: when a run ends
with one, the gateway puts it on the watch (`/internal/remote_done`).
No general shell (only `wacli --read-only`, as for the watch agent); `~/.config` and `~/.ssh` are unreadable and the Onurb repo is not writable.

## Setup (done once)

- Mac: `~/.config/onurb/remote_key` (ed25519) and `~/.config/onurb/remote.env`
  (`ONURB_REMOTE_SSH=onurb@<server>`), both private, never in the repo.
- Server: `~onurb/.ssh/authorized_keys` pins that key to this script:
  `command="/usr/bin/python3 /home/onurb/onurb/remote/onurb_remote.py",no-port-forwarding,no-X11-forwarding,no-agent-forwarding,no-pty,no-user-rc <key>`
  so the key can only run the commands above (no shell).
- Register on the Mac (all projects):
  `claude mcp add --scope user onurb-server -- python3 <repo>/remote/onurb_remote_mcp.py`
- Deploy: `rsync -az --exclude __pycache__ remote/ <server>:/home/onurb/onurb/remote/`, then
  `chown -R onurb:onurb`.
