#!/usr/bin/env python3
"""Relabel gesture captures whose recording was dismissed as "wrong" (Bruno,
2026-11-10: a gesture that started a recording nobody spoke into was a false
trigger). Reads the gateway log in order:
  "mouth pose entry ...: GESTURE -> turn -> <file>_accepted.csv"  a gesture started a recording
  "turn <id>: ..."                                                 its audio reached the gateway
  "no speech" / "dismissed"                                        ...and was dropped there
An accepted capture with no upload before the next pose entry was dropped on the
watch (no voice in 4 s). Usage: tools/relabel_dismissed.py [--apply]"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FB = ROOT / "gateway" / "received" / "gesture_feedback"
LOG = ROOT / "gateway" / "received" / "gateway.log"

entry_re = re.compile(r"mouth pose entry .*GESTURE -> turn -> (\S+_accepted\.csv)")
dismissed = []
cur, uploaded = None, False
for line in LOG.read_text(errors="replace").splitlines():
    if "mouth pose entry" in line:
        if cur and not uploaded:
            dismissed.append((cur, "dropped on the watch"))
        m = entry_re.search(line)
        cur, uploaded = (m.group(1), False) if m else (None, False)
    elif cur and re.search(r"\] turn [0-9a-f]{8}: [0-9.]+ s @", line):
        uploaded = True
    elif cur and uploaded and ("no speech" in line or "dismissed" in line):
        dismissed.append((cur, "no speech at the gateway"))
        cur = None
done = 0
for name, why in dismissed:
    f = FB / name
    if not f.exists():
        continue
    print(f"{name}: {why}")
    if "--apply" in sys.argv:
        f.rename(f.with_name(name.replace("_accepted", "_wrong")))
        t = f.with_suffix(".txt")
        if t.exists():
            t.rename(t.with_name(t.name.replace("_accepted", "_wrong")))
        done += 1
print(f"{len([d for d in dismissed if (FB / d[0]).exists()])} dismissed captures" + (f", {done} relabelled" if done else " (dry run)"))
