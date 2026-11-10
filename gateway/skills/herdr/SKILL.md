---
name: herdr
description: "Control the user's herdr (their terminal workspace running many Claude Code agents, on this Mac): see what each agent is doing, read its screen, prompt it, answer its permission prompts, start agents. Use whenever the user mentions herdr (speech-to-text: 'hoarder', 'herd', 'herder'), their agents/sessions/abas, or asks to watch, approve, prompt or check on them."
---

# herdr for Onurb

herdr runs on this same Mac, so the `herdr` CLI talks to it directly. No Cloudflare
tunnel needed (that tunnel only exists so Instinct, which runs elsewhere, can reach
its local web UI). Every command prints JSON; read ids from it,
never guess them. Never run bare `herdr` (it opens the TUI).

## Look

```bash
herdr agent list                                   # every agent: pane_id, agent_status, terminal_title_stripped, cwd
herdr agent get <pane_id|name>
herdr agent read <pane_id> --source visible --lines 40           # current screen (works while blocked)
herdr agent read <pane_id> --source recent-unwrapped --lines 150 # history (only while idle)
herdr workspace list ; herdr tab list --workspace <w> ; herdr pane list --workspace <w>
```

Status: `idle`/`done` = ready for input, `working` = busy, `blocked` = waiting at a
permission prompt or question, `unknown` = can't tell. Refer to agents by their title
(terminal_title_stripped), e.g. "o agente do Top candidatos por scout".

Run each `herdr` command on its own: no pipes, `python3`, `jq` or `&&` (only `herdr ...`
commands are allowed, anything else is denied). Read the JSON yourself.

## Answer a permission prompt (blocked agent)

1. `herdr agent read <pane_id> --source visible --lines 40` and read the whole prompt:
   which tool, and the full command/query.
2. Before every key: `herdr agent get <pane_id>` must say `agent_status: blocked` and the
   screen must show the prompt ("Do you want to proceed?" with numbered options). If not,
   send nothing: a stray `1` would be typed into the agent's input box. One key per
   prompt you have actually read; parallel tool calls show their prompts one after another.
3. Decide with the rules below, then answer:
   - Yes: `herdr agent send-keys <pane_id> 1`
   - Yes, don't ask again (only if offered and the user asked for it): `herdr agent send-keys <pane_id> 2`
   - No / cancel: `herdr agent send-keys <pane_id> esc`
4. Read the screen again to confirm the prompt is gone.

Approval rules (the user's):
- **Approve** read-only lookups: MCP searches/gets/lists, and `supabase-prod`
  `execute_sql` only when it is a single `select`/`with` query with no
  insert/update/delete/merge/drop/alter/truncate/create/grant/revoke/copy/call and no
  function that writes. If any part of the query is cut off or unclear, don't approve.
- **Never approve, tell the user instead**: anything that writes to production data,
  migrations, deletes (files, rows, branches), git push/merge, deploys, sending messages
  or emails, spending money, accepting terms. Say which agent wants what.

## Prompt an agent

```bash
herdr agent prompt <pane_id|name> "texto" --wait --timeout 20000      # in a spoken turn: short wait
herdr agent prompt <pane_id|name> "texto" --wait --timeout 1800000    # in a background task
```

It refuses a blocked agent (`agent_blocked`): answer the prompt first. A timeout does
not mean it wasn't delivered: read the screen before re-sending. Pass the user's request
in their words; don't add instructions they didn't give.

## Start a new agent

```bash
herdr pane split <pane_id> --direction right --cwd <dir> --no-focus   # returns .result.pane.pane_id
herdr agent start <name> --kind claude --pane <new_pane_id>
herdr agent prompt <name> "..." --wait --timeout 20000
```

Put it next to an agent working in the same repo, with that repo as cwd.

## Watching ("fica olhando o herdr")

This only runs while you are running, so do it as a background task
(start_background_task): loop for up to ~40 min: list agents, handle each `blocked` one
with the rules above, then `herdr agent wait <pane_id> --until blocked --timeout 60000`
on a working agent or sleep via `herdr agent list` polling about every 20 s. Report what
you approved and what is waiting for the user.

## Don't

- Don't type into plain shell panes (`pane send-text`, `pane send-keys`, `pane run`),
  close panes/tabs/workspaces, or touch `server`, `session`, `machine`, `config`,
  `update`: not allowed.
- Don't focus panes (it moves the user's view) unless asked.
