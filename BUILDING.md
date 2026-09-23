# Building Enigma BLE

The firmware is a native ESP-IDF project for ESP32-S3. Builds are pinned to
ESP-IDF 5.5.5 and managed component versions are locked in
[`dependencies.lock`](dependencies.lock). Internet access is required on the
first build so ESP-IDF can download its toolchain and managed components.

## Common requirements

- ESP32-S3 N16R8 development board
- Data-capable USB cable for the programming/UART connector
- Git
- Python 3.9 or newer
- CMake 3.16 or newer and Ninja
- ESP-IDF 5.5.5 with the ESP32-S3 toolchain

The PC utilities use the packages pinned in [`requirements.txt`](requirements.txt):

```shell
python -m pip install -r requirements.txt
```

The default firmware drives a NeoPixel on GPIO 48. On a board without one,
run `idf.py menuconfig` from an ESP-IDF shell (after sourcing `export.sh` in
WSL/Linux), then disable **Enigma Bridge → Enable GPIO 48 NeoPixel status
LED** before building. This leaves GPIO 48 untouched. An unpopulated WS2812
cannot be detected automatically; LED initialization failures are logged but
do not stop the bridge.

## Windows

### Automated Windows and WSL setup

This is the tested setup used by the supplied scripts. It requires Windows 10
or 11, PowerShell, `winget`, WSL2, and the Ubuntu 24.04 WSL distribution.

Install WSL from an elevated PowerShell window if it is not already present:

```powershell
wsl --install -d Ubuntu-24.04
```

Restart Windows if requested, open Ubuntu once to create its user account,
then run from the project folder:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\tools\setup.ps1
.\tools\build.ps1
```

[`setup.ps1`](tools/setup.ps1) installs Python 3.13 and the pinned PC packages
on Windows. It then invokes [`setup_wsl.sh`](tools/setup_wsl.sh), which installs
the Ubuntu compiler prerequisites, clones ESP-IDF 5.5.5 into
`~/enigma-ble-deps/esp-idf`, and installs the ESP32-S3 toolchain.

### Native Windows ESP-IDF

As an alternative to WSL, install ESP-IDF 5.5.5 with Espressif's
[ESP-IDF Installation Manager](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/get-started/windows-setup.html).
It installs Python, Git, CMake, Ninja, and the Xtensa toolchain. Open an
**ESP-IDF PowerShell** so `idf.py` is on `PATH`, then run:

```powershell
python -m pip install -r requirements.txt
.\tools\build.ps1
```

[`build.ps1`](tools/build.ps1) uses native `idf.py` when available and
otherwise falls back to Ubuntu 24.04 under WSL.

Build output:

```text
build\enigma_ble_firmware.bin
build\esp-idf\enigma_ble.bin
build\esp-idf\bootloader\bootloader.bin
build\esp-idf\partition_table\partition-table.bin
```

Flash a clean installation:

```powershell
.\tools\flash.ps1 -Port COM8 -Erase
```

For later development updates, omit `-Erase`. The script then writes the
individual ESP-IDF images and preserves the NVS bond database:

```powershell
.\tools\flash.ps1 -Port COM8
```

## Ubuntu and Debian Linux

Install the ESP-IDF 5.5.5 prerequisites and BlueZ support needed by the NUS
terminal:

```shell
sudo apt-get update
sudo apt-get install -y \
  git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 \
  bluez
```

Install the pinned ESP-IDF release:

```shell
mkdir -p "$HOME/esp"
git clone --branch v5.5.5 --recursive \
  https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf"
git -C "$HOME/esp/esp-idf" checkout b774170ff46c393eeb5e495ea37936038d3f4f4f
git -C "$HOME/esp/esp-idf" submodule update --init --recursive
"$HOME/esp/esp-idf/install.sh" esp32s3
```

Build:

```shell
. "$HOME/esp/esp-idf/export.sh"
idf.py build
idf.py merge-bin -o build/enigma_ble_firmware.bin
```

Flash while preserving NVS:

```shell
idf.py -p /dev/ttyUSB0 flash
```

Serial devices may instead appear as `/dev/ttyACM0`. If access is denied, add
your user to the port's group, commonly `dialout`, then sign out and back in:

```shell
sudo usermod -aG dialout "$USER"
```

For the Python terminal, a virtual environment is recommended:

```shell
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
python tools/nus_terminal.py
```

The terminal needs a running BlueZ service and permission to use the local
Bluetooth adapter.

## macOS

Install Apple's command-line tools and the prerequisites documented by
ESP-IDF:

```shell
xcode-select --install
brew install cmake ninja dfu-util ccache python libusb
```

Install ESP-IDF 5.5.5:

```shell
mkdir -p "$HOME/esp"
git clone --branch v5.5.5 --recursive \
  https://github.com/espressif/esp-idf.git "$HOME/esp/esp-idf"
git -C "$HOME/esp/esp-idf" checkout b774170ff46c393eeb5e495ea37936038d3f4f4f
git -C "$HOME/esp/esp-idf" submodule update --init --recursive
"$HOME/esp/esp-idf/install.sh" esp32s3
```

Build and flash:

```shell
. "$HOME/esp/esp-idf/export.sh"
idf.py build
idf.py merge-bin -o build/enigma_ble_firmware.bin
idf.py -p /dev/cu.usbserial-0001 flash
```

Use the actual `/dev/cu.*` device for the programming port. Install the Python
utilities in a virtual environment:

```shell
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
```

The NUS terminal uses macOS CoreBluetooth through Bleak and does not require
BlueZ.

## WSL Bluetooth behavior

Normal WSL2 installations do not expose the Windows Bluetooth adapter to
BlueZ. Running [`nus_terminal.py`](tools/nus_terminal.py) under WSL therefore
relays automatically to the installed Windows Python. Script paths are
translated automatically. Use `--native-wsl` only when a USB Bluetooth
adapter has been passed through to WSL and configured with BlueZ.

## GitHub Actions

[`build.yml`](.github/workflows/build.yml) runs for pushes, pull requests, and
manual dispatches (and is reused by the release workflow) in a digest-pinned
ESP-IDF 5.5.5 container. It uploads the
`enigma_ble_firmware` artifact containing:

- merged `enigma_ble_firmware.bin`
- application, bootloader, and partition-table images
- flash arguments and project metadata

The merged image is ready for `write-flash 0`. The individual images are
included for development flashing that preserves NVS.

To publish a release, open **Actions → Release ESP-IDF firmware → Run
workflow** for [`release.yml`](.github/workflows/release.yml), select the
commit/branch to release, and supply a version number such as `1.0.0` (or
`1.0.0-rc.1`). The workflow builds that commit, creates the `v1.0.0` tag and
release (or a prerelease for `v1.0.0-rc.1`), and attaches both the merged
`enigma_ble_firmware.bin` and an `enigma_ble_firmware.zip` containing all
build artifacts.
An existing version tag is rejected rather than publishing binaries from a
different commit under that tag. Releases remain available independently of
the 30-day Actions artifact retention.

## Build configuration

[`sdkconfig.defaults`](sdkconfig.defaults) selects ESP32-S3, 16 MB flash,
NimBLE peripheral mode, two BLE connections, NVS bond persistence, and the
USB host control-transfer size. The component manager resolves:

- `espressif/led_strip` 3.0.3
- `espressif/usb_host_cdc_acm` 2.4.1
- `espressif/usb_host_hid` 1.2.1

The generated `sdkconfig`, `build/`, and `managed_components/` paths are
ignored and should not be committed.

## Architecture

[`main/main.c`](main/main.c) owns the bridge loop, BOOT-button bond reset, and
NeoPixel status. [`main/enigma_ble.c`](main/enigma_ble.c) implements NimBLE
HID, NUS, security, bonding, notifications, and the BLE-to-USB queue.
[`components/enigma_usb_host`](components/enigma_usb_host) contains the HID
and CDC host drivers.
