#!/bin/bash
# Build the watch firmware (ESP-IDF). The gateway serves firmware/build/onurb.bin to the
# watch, so a successful build is offered to it on its next update check.
# Used by Onurb's code tasks (gateway/onurb_agent.py) and by hand.
set -o pipefail
cd "$(dirname "$0")/../firmware" || exit 1
# shellcheck disable=SC1090
source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1 || { echo "ESP-IDF not found"; exit 1; }
if ! out=$(idf.py build 2>&1); then
    echo "$out" | grep -E "error|Error" | grep -v "^--" | head -40
    echo "BUILD FAILED"
    exit 1
fi
echo "$out" | grep -E "warning:" | head -20
echo "$out" | grep -E "onurb.bin binary size" | tail -1
# Away from home the watch checks the server gateway, which serves its own copy of the image.
# ONURB_SERVER_SSH names the ssh host ("" skips). Copy to a temp name, then rename, so the
# server never serves a half-written image.
srv=${ONURB_SERVER_SSH-onurb-1}
if [ -n "$srv" ] && ssh -o BatchMode=yes -o ConnectTimeout=5 "$srv" true 2>/dev/null; then
    dst=/home/onurb/onurb/firmware/build
    if ssh "$srv" "mkdir -p $dst" &&
       rsync -az build/onurb.bin "$srv:$dst/.onurb.bin.tmp" &&
       ssh "$srv" "mv $dst/.onurb.bin.tmp $dst/onurb.bin && chown -R onurb:onurb $dst"; then
        echo "image copied to the server"
    else
        echo "WARNING: could not copy the image to the server (the watch away from home won't see it)"
    fi
fi
echo "BUILD OK - offered to the watch on its next update check (hourly, or when the screen is opened)"
