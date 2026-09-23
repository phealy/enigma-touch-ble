#!/usr/bin/env bash
set -euo pipefail

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    bison \
    ccache \
    cmake \
    dfu-util \
    flex \
    git \
    gperf \
    libffi-dev \
    libssl-dev \
    libusb-1.0-0 \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    wget

deps="$HOME/enigma-ble-deps"
esp_idf="$deps/esp-idf"
mkdir -p "$deps"

if [[ ! -d "$esp_idf/.git" ]]; then
    git clone --branch v5.5.5 --recursive \
        https://github.com/espressif/esp-idf.git "$esp_idf"
fi
git -C "$esp_idf" checkout b774170ff46c393eeb5e495ea37936038d3f4f4f
git -C "$esp_idf" submodule update --init --recursive

"$esp_idf/install.sh" esp32s3
