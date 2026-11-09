#!/usr/bin/env bash
# Read the watch's entire 16 MB flash into backups/ before flashing Onurb.
# Restore later with:
#   python -m esptool --chip esp32s3 -p <port> write_flash 0x0 backups/<file>.bin
# backups/ is gitignored: the image is Waveshare's firmware and its NVS may hold Wi-Fi settings.
set -euo pipefail

PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
if [[ -z "${PORT}" ]]; then
    echo "No /dev/cu.usbmodem* port found - is the watch connected?" >&2
    exit 1
fi

: "${IDF_PATH:=$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
. "${IDF_PATH}/export.sh" >/dev/null

cd "$(dirname "$0")/.."
mkdir -p backups
OUT="backups/factory_$(date +%Y%m%d_%H%M%S).bin"

python -m esptool --chip esp32s3 -p "${PORT}" -b 921600 read_flash 0x0 0x1000000 "${OUT}"
shasum -a 256 "${OUT}" | tee "${OUT}.sha256"
echo "Saved ${OUT}"
