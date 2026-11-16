#!/usr/bin/env python3
"""Scheduled WhatsApp messages (the user: "build it", 2026-11-16).

The user confirms a message *and its send time* on the watch; the server then
sends exactly that text at that time. The queue lives only on the server (always
on), so a message goes out even with the Mac asleep, and never twice.

- The server's gateway runs `run_scheduler()` (a thread): every 20 s it sends
  what is due with `wacli send text`, retries for 10 min, then gives up and
  tells the watch. Each send/failure is reported through `notify`.
- The Mac's gateway hands a confirmed message over with SSH, using a key that
  the server restricts to this file (authorized_keys `command=`):
      ssh <server> add      < {"jid", "name", "messages", "send_at", "turn"}
      ssh <server> list
      ssh <server> cancel <id>
  See docs/SERVER.md ("Scheduled WhatsApp").

Queue file: <gateway>/received/scheduled.json (gitignored), under a file lock.
"""
import fcntl
import json
import os
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
QUEUE = HERE / "received" / "scheduled.json"
RETRY_FOR_S = 10 * 60
MAX_AHEAD_S = 60 * 24 * 3600
TICK_S = 20


class _Locked:
    def __enter__(self):
        QUEUE.parent.mkdir(exist_ok=True)
        self.fh = open(QUEUE.with_suffix(".lock"), "w")
        fcntl.flock(self.fh, fcntl.LOCK_EX)
        try:
            self.items = json.loads(QUEUE.read_text()) if QUEUE.exists() else []
        except ValueError:
            self.items = []
        return self

    def save(self):
        tmp = QUEUE.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.items, ensure_ascii=False, indent=1))
        os.replace(tmp, QUEUE)

    def __exit__(self, *exc):
        self.fh.close()


def validate(item):
    """-> error string, or None."""
    if not isinstance(item, dict):
        return "bad item"
    if not item.get("jid") or "@" not in str(item["jid"]):
        return "jid missing"
    msgs = item.get("messages")
    if not isinstance(msgs, list) or not msgs or not all(isinstance(m, str) and m.strip() for m in msgs):
        return "messages missing"
    if len(msgs) > 8 or sum(len(m) for m in msgs) > 6000:
        return "too long"
    try:
        at = float(item.get("send_at"))
    except (TypeError, ValueError):
        return "send_at missing"
    now = time.time()
    if at < now - 60 or at > now + MAX_AHEAD_S:
        return "send_at out of range"
    return None


def add(item):
    err = validate(item)
    if err:
        return {"error": err}
    rec = {"id": uuid.uuid4().hex[:8], "jid": str(item["jid"]), "name": str(item.get("name", ""))[:80],
           "messages": [m.strip() for m in item["messages"]], "send_at": float(item["send_at"]),
           "when": str(item.get("when", ""))[:60], "turn": str(item.get("turn", ""))[:32],
           "state": "scheduled", "created": time.time()}
    with _Locked() as q:
        q.items.append(rec)
        q.save()
    tz = str(item.get("tz", ""))
    if tz and len(tz) < 64 and all(c.isalnum() or c in "/_-+" for c in tz):   # the Mac's zone, for the server agent
        (QUEUE.parent / "user_tz").write_text(tz)
    return {"id": rec["id"]}


def list_pending():
    with _Locked() as q:
        return [{k: i[k] for k in ("id", "name", "messages", "send_at", "when")}
                for i in q.items if i["state"] == "scheduled"]


def cancel(sid):
    with _Locked() as q:
        for i in q.items:
            if i["id"] == sid and i["state"] == "scheduled":
                i["state"] = "cancelled"
                q.save()
                return True
    return False


def _send(rec):
    """Send the messages not sent yet (a retry never repeats a bubble that went out)."""
    done = rec.setdefault("sent_n", 0)
    for m in rec["messages"][done:]:
        r = subprocess.run(["wacli", "send", "text", "--to", rec["jid"], "--message", m],
                           capture_output=True, text=True, timeout=90)
        if r.returncode != 0:
            return (r.stderr or r.stdout).strip()[-200:] or "wacli failed"
        rec["sent_n"] += 1
    return None


def tick(notify, log=print):
    now = time.time()
    with _Locked() as q:
        due = [i for i in q.items if i["state"] == "scheduled" and i["send_at"] <= now]
        for rec in due:
            if now - rec["send_at"] > RETRY_FOR_S and not rec.get("sent_n") and not rec.get("tried"):
                # the server was down at the time: don't send hours late
                rec["state"], rec["error"] = "missed", "server was down at the send time"
                log(f"  scheduled {rec['id']}: MISSED (server down at the time) for {rec['name']}")
                notify(f"A mensagem agendada pro {rec['name']} não saiu na hora (servidor fora do ar); "
                       "não enviei atrasada. Quer mandar agora?", rec)
                continue
            rec["tried"] = True
            err = _send(rec)
            if err is None:
                rec["state"], rec["sent"] = "sent", time.time()
                log(f"  scheduled {rec['id']}: sent {len(rec['messages'])} message(s) to {rec['name']}")
                notify(f"Enviei a mensagem agendada pro {rec['name']}.", rec)
            elif now - rec["send_at"] > RETRY_FOR_S:
                rec["state"], rec["error"] = "failed", err
                log(f"  scheduled {rec['id']}: FAILED for {rec['name']}: {err}")
                notify(f"Não consegui enviar a mensagem agendada pro {rec['name']}; nada foi enviado "
                       f"depois de 10 minutos tentando." if not rec.get("sent_n") else
                       f"A mensagem agendada pro {rec['name']} saiu só em parte; o resto falhou.", rec)
            else:
                log(f"  scheduled {rec['id']}: send failed, retrying ({err})")
            q.save()
        # keep finished items a week for "what was sent", then drop them
        q.items = [i for i in q.items if i["state"] == "scheduled" or now - i.get("send_at", now) < 7 * 86400]
        q.save()


def run_scheduler(notify, log=print):
    def loop():
        while True:
            try:
                tick(notify, log)
            except Exception as e:  # noqa: BLE001 - the scheduler must keep running
                log(f"  scheduler error: {e}")
            time.sleep(TICK_S)
    threading.Thread(target=loop, daemon=True, name="wa-scheduler").start()


def main():
    """SSH forced command: the command comes from SSH_ORIGINAL_COMMAND (or argv)."""
    cmd = (os.environ.get("SSH_ORIGINAL_COMMAND") or " ".join(sys.argv[1:])).split()
    if not cmd:
        print(json.dumps({"error": "usage: add | list | cancel <id>"}))
        return 2
    if cmd[0] == "add":
        try:
            item = json.loads(sys.stdin.read(100_000))
        except ValueError:
            print(json.dumps({"error": "bad json"}))
            return 2
        print(json.dumps(add(item)))
    elif cmd[0] == "list":
        print(json.dumps({"items": list_pending()}, ensure_ascii=False))
    elif cmd[0] == "cancel" and len(cmd) == 2:
        print(json.dumps({"cancelled": cancel(cmd[1])}))
    else:
        print(json.dumps({"error": "unknown command"}))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
