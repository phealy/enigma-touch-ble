#!/usr/bin/env bash
set -euo pipefail

root="${1:?workspace path is required}"
esp_idf="$HOME/enigma-ble-deps/esp-idf"
build="$root/build/esp-idf"
firmware="$root/build/enigma_ble_firmware.bin"

if [[ ! -f "$esp_idf/export.sh" ]]; then
    echo "WSL ESP-IDF is not installed. Run tools/setup.ps1 first." >&2
    exit 1
fi

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
source "$esp_idf/export.sh" >/dev/null
mkdir -p "$build"

cd "$root"
idf.py -B "$build" build
idf.py -B "$build" merge-bin -o "$firmware"
