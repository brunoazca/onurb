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
echo "BUILD OK - offered to the watch on its next update check (hourly, or when the screen is opened)"
