# Enigma Touch USB-to-BLE bridge

Native ESP-IDF firmware for an ESP32-S3 N16R8 development board. The board
acts as a USB host for an Enigma Touch and exposes either operating mode over
Bluetooth Low Energy:

- USB HID keyboard input as a standard BLE HID keyboard.
- USB CDC serial data as Nordic UART Service (NUS).

The firmware automatically detects the active Enigma Touch mode, supports
hot-plugging, stores BLE bonds in NVS, and accepts two simultaneous BLE
connections.

For source builds and developer dependencies on Windows, Linux, and macOS,
see [`BUILDING.md`](BUILDING.md).

## Connect the hardware

Connect the Enigma Touch to the ESP32-S3 native USB OTG port:

| ESP32-S3 signal | Pin |
| --- | --- |
| USB D- | GPIO 19 |
| USB D+ | GPIO 20 |
| USB VBUS | 5 V host output |
| USB GND | GND |

Use the separate UART, COM, or USB-JTAG connector for flashing and diagnostic
output. The Enigma Touch requires stable 5 V VBUS and must not be connected to
the PC and ESP32 USB host simultaneously.

Serial mode is accepted only when the USB device identifies as VID `0483` and
PID `5740`.

## Install a published firmware image

Download `enigma_ble_firmware.bin` from a GitHub Release, or download and
extract the `enigma_ble_firmware` artifact from a GitHub Actions run. Install
Python 3 and esptool, then connect the ESP32-S3 programming port and flash the
merged image:

```shell
python -m pip install esptool==5.4.0
python -m esptool --chip esp32s3 --port COM8 erase-flash
python -m esptool --chip esp32s3 --port COM8 --baud 460800 write-flash 0 enigma_ble_firmware.bin
```

Replace `COM8` with the board's programming port. Linux ports normally look
like `/dev/ttyUSB0` or `/dev/ttyACM0`; macOS ports normally begin with
`/dev/cu.`. If automatic download mode fails, hold **BOOT**, tap **RESET**,
then release **BOOT** before retrying.

After a full erase or migration from older firmware, remove any existing
**Enigma Touch BLE** pairing from the PC or phone and pair again.

## Pair and use keyboard mode

1. Put the Enigma Touch into one of its USB keyboard modes.
2. Pair **Enigma Touch BLE** in the PC or phone Bluetooth settings.
3. Open the destination application and type on the Enigma Touch.

Windows claims the HID service for its keyboard driver, so generic BLE tools
may show NUS but omit HID after pairing. Device Manager should list a
Bluetooth HID keyboard.

Four-byte Enigma Touch reports are normalized to standard eight-byte BLE
boot-keyboard reports.

## Use serial mode

Put the Enigma Touch into one of its USB serial logging modes. The bridge uses
the standard Nordic UART Service UUIDs:

- Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- PC or phone to Enigma Touch: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- Enigma Touch to PC or phone: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- Bridge USB status (read/notify): `6E400004-B5A3-F393-E0A9-E50E24DCCA9E`

The status byte is `0` while USB is disconnected, `1` in keyboard mode, and
`2` in serial mode (bit flags may be combined). The included terminal reads
and subscribes to this status. If the Enigma Touch is unplugged or switched to
keyboard mode, the terminal displays that state, clears command help, and
shows the notice on the input line. Only exit keys work until serial mode
returns, at which point the notice is erased. Command scripts fail explicitly
when serial mode is unavailable. Other NUS clients can read this characteristic
to check availability before sending data. After installing firmware with this
new characteristic, remove and re-pair **Enigma Touch BLE** if the PC has
cached the previous GATT database.

BLE GATT does not create an operating-system COM port. Use the included
terminal or another NUS-compatible application.

Install the terminal dependency and connect:

```shell
python -m pip install bleak==3.0.2 prompt_toolkit==3.0.53
python tools/nus_terminal.py
```

On Windows, `.\tools\nus_terminal.ps1` is a convenience wrapper.

### Terminal controls

- Input is transmitted immediately with no local echo.
- Incoming data is displayed immediately. The `prompt_toolkit` UI keeps
  incomplete serial lines visible, including backspace/erase echoes, while
  handling help and terminal resizing.
- **Ctrl+C**, **Ctrl+D**, **Ctrl+]**, or **Escape** disconnects.
- Enter sends CRLF.
- The terminal sends an initial CRLF when USB serial mode becomes available.
- Backspace is ignored during normal text input.
- A semicolon begins an Enigma command comment for the rest of the line.

A line beginning with `?` or `!` enters Enigma command editing mode. Backspace
sends `BS SPACE BS`, matching the Enigma Touch line editor. If the initial
`?` or `!` is erased, press Enter before entering another command prefix.

Typing `?` or `!` also opens local command help based on the official Enigma
Touch [User Manual version 2.5](https://e-basteln.de/file/enigma/Enigma%20Touch%20Instructions.pdf),
section 4.3:

- Type letters to filter the documented commands.
- Commands appear alphabetically by their two-character codes.
- Use **Up** and **Down** to select.
- Use **Enter**, **Tab**, or **Right Arrow** to insert the selected command.
- Once a command is selected or its two-letter code is typed, the popup shows
  its argument syntax, a short explanation, and any documented value meanings
  for both query and set commands.
- Query entries explicitly say what `?` will query; set/action entries say
  what `!` will set or do.
- The help popup is local only; selected command text is still sent live to
  the Enigma Touch.

### Send a command script

Send a UTF-8 file and disconnect when it completes:

```shell
python tools/nus_terminal.py --script commands.txt
```

Each line is CRLF-terminated. Configure pacing when needed:

```shell
python tools/nus_terminal.py --script commands.txt --line-delay 0.25 --final-wait 2
```

## Connection status

On boards fitted with a GPIO 48 WS2812-compatible NeoPixel, the firmware
indicates connection status (see [BUILDING.md](BUILDING.md) to disable this
output on boards without the LED):

- Off: USB mode is unavailable, or no BLE connection has completed encryption
  and bonding. A connection retry using an obsolete pairing key does not light
  the LED.
- Dim green: keyboard USB mode and a secured BLE connection are active.
- Dim blue: serial USB mode and a secured BLE connection are active.
- Dim red: stored bonds are being erased.

If the PC or phone drops its pairing record while the ESP32 retains its old
bond, pairing automatically replaces the stale peer record. To erase all
stored bonds manually, first disconnect BLE (turn off the PC's Bluetooth if
it keeps reconnecting), then hold the board's **BOOT** button for three
seconds. A hold made while connected cannot clear bonds; release and press
again once disconnected. The NeoPixel turns dim red and the board restarts.
The BLE address stays the same. If Windows still has its old pairing after
this reset, it may repeatedly connect and disconnect using its obsolete key;
remove **Enigma Touch BLE** from Windows Bluetooth settings, then pair again.

## Diagnostic serial output

On Windows:

```powershell
.\tools\monitor.ps1 -Port COM8
```

The monitor runs at 115200 baud. Press **Ctrl+]** to exit.

## License

This project is available under the [`MIT License`](LICENSE).
