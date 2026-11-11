#!/usr/bin/env python3
"""Put the remote gateway URL and the Cloudflare Access service token into
firmware/sdkconfig (gitignored), from ~/.config/onurb/cf-access.env
(CF_ACCESS_CLIENT_ID / CF_ACCESS_CLIENT_SECRET). Never prints the secret.
Usage: tools/set_remote_gateway.py https://onurb-api.<domain>"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / "firmware" / "sdkconfig"
env = dict(l.split("=", 1) for l in (Path.home() / ".config/onurb/cf-access.env").read_text().split() if "=" in l)
vals = {"CONFIG_ONURB_API_REMOTE_URL": sys.argv[1].rstrip("/"),
        "CONFIG_ONURB_CF_ACCESS_ID": env["CF_ACCESS_CLIENT_ID"],
        "CONFIG_ONURB_CF_ACCESS_SECRET": env["CF_ACCESS_CLIENT_SECRET"]}
s = SDK.read_text()
for k, v in vals.items():
    line = f'{k}="{v}"'
    s, n = re.subn(rf"^{k}=.*$", line, s, flags=re.M)
    if not n:
        s = s.replace("CONFIG_ONURB_API_TOKEN=", f"{line}\nCONFIG_ONURB_API_TOKEN=", 1)
SDK.write_text(s)
print("set:", ", ".join(vals))
