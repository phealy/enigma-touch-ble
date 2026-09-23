import argparse
import asyncio
from pathlib import Path, PureWindowsPath
import platform
from queue import Queue
import subprocess
import sys
from typing import NamedTuple

from prompt_toolkit.application import Application, run_in_terminal
from prompt_toolkit.data_structures import Point
from prompt_toolkit.input import DummyInput
from prompt_toolkit.key_binding import KeyBindings
from prompt_toolkit.layout import Layout
from prompt_toolkit.layout.containers import Window
from prompt_toolkit.layout.controls import FormattedTextControl

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
NUS_STATUS = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
USB_HID = 1
USB_SERIAL = 2
BACKSPACE = "\x08"
ERASE_PREVIOUS = b"\x08 \x08"
SESSION_INTERRUPT = "\x03"
SESSION_EOF = "\x04"
SESSION_BREAK = "\x1d"
SESSION_ESCAPE = "\x1b"
KEY_UP = "\x00UP"
KEY_DOWN = "\x00DOWN"
KEY_LEFT = "\x00LEFT"
KEY_RIGHT = "\x00RIGHT"
KEY_TAB = "\t"
WRITE_CHUNK_SIZE = 20
POPUP_MAX_CHOICES = 8


class CommandHelp(NamedTuple):
    code: str
    name: str
    summary: str
    set_arguments: str | None
    query_supported: bool = True
    set_supported: bool = True
    values_text: str | None = None

    def action(self, prefix):
        subject = self.name
        if not subject.startswith(("Enigma", "Morse")):
            subject = subject[:1].lower() + subject[1:]
        if self.code in ("LM", "LW", "LR"):
            return (
                f"Query {subject}."
                if prefix == "?"
                else f"Lock/unlock {subject.removeprefix('lock ')}."
            )
        if self.code == "LP":
            return (
                "Query power button lock."
                if prefix == "?"
                else "Lock/unlock the power button."
            )
        if prefix == "?":
            verb = "Query"
        elif self.code == "PO":
            return "Turn off the Enigma Touch."
        elif self.code == "RS":
            return "Restore default settings."
        else:
            verb = "Set"
        article = "" if self.code in ("RI", "RP", "XM", "MV") else "the "
        return f"{verb} {article}{subject}."


LOCK_VALUES = "0 = unlocked; 1 = locked; omitting the value locks the setup mode."
TIMEOUT_VALUES = "0 = disabled; 1..99 = minutes of inactivity."
CUSTOM_ROTOR_VALUES = (
    "26 distinct letters A..Z; punctuation from ! through / marks turnovers. "
    "An empty set command disables the rotor."
)

COMMANDS = (
    CommandHelp(
        "MO", "Enigma model", "Select the simulated Enigma model.", "<model>",
        values_text="One- or two-letter model code, e.g. I, M3, M4, or X.",
    ),
    CommandHelp(
        "RO",
        "Rotor set",
        "Set reflector and three or four active rotors.",
        "<reflector> <rotor...>",
        values_text=(
            "Model-specific reflector code; three or four distinct rotors "
            "(I..VIII, plus beta/gamma for M4)."
        ),
    ),
    CommandHelp(
        "RI",
        "Ring settings",
        "Set three or four ring positions as letters or numbers.",
        "<letters | numbers 1..26>",
        values_text="A..Z or 1..26; three rings, or four for M4.",
    ),
    CommandHelp(
        "RP",
        "Rotor positions",
        "Set rotor positions, including a settable reflector when present.",
        "<letters | numbers 1..26>",
        values_text="A..Z or 1..26; three positions, or four with M4 or a movable reflector.",
    ),
    CommandHelp(
        "RD",
        "Reflector D wiring",
        "Set UKW-D wiring in German or Bletchley Park notation.",
        "<13 letter pairs>",
        values_text=(
            "13 pairs using every letter once; JY paired = German notation, "
            "BO paired = Bletchley Park notation."
        ),
    ),
    CommandHelp(
        "UD",
        "Reflector D wiring",
        "Alias for the RD reflector wiring command.",
        "<13 letter pairs>",
        values_text=(
            "13 pairs using every letter once; JY paired = German notation, "
            "BO paired = Bletchley Park notation."
        ),
    ),
    CommandHelp(
        "PB",
        "Plugboard wiring",
        "Set zero to thirteen virtual plugboard connections.",
        "[letter pairs]",
        values_text=(
            "0..13 letter pairs, with no repeated letter; setting requires "
            "no physical plugboard connections."
        ),
    ),
    CommandHelp(
        "XM",
        "Custom model features",
        "Combine counter, fixed-notch, numeric, plugboard, and reflector features.",
        "<feature letters C F N P R>",
        values_text=(
            "C = counter; F = fixed notches; N = numeric rotors; "
            "P = plugboard; R = movable reflector."
        ),
    ),
    *(
        CommandHelp(
            f"X{index}",
            f"Custom rotor {index}",
            "Define a 26-letter rotor wiring; punctuation marks turnover positions.",
            "[26-letter wiring]",
            values_text=CUSTOM_ROTOR_VALUES,
        )
        for index in range(1, 9)
    ),
    CommandHelp(
        "XE",
        "Custom entry wheel",
        "Define the custom model's 26-letter entry-wheel wiring.",
        "<26-letter wiring>",
        values_text="26 distinct letters A..Z; turnover markers are not allowed.",
    ),
    CommandHelp(
        "XR",
        "Custom reflector",
        "Define the custom reflector as thirteen letter pairs.",
        "<13 letter pairs>",
        values_text="13 pairs using every letter A..Z once; no fixed pair is required.",
    ),
    CommandHelp(
        "XU",
        "Custom reflector",
        "Alias for the XR custom reflector command.",
        "<13 letter pairs>",
        values_text="13 pairs using every letter A..Z once; no fixed pair is required.",
    ),
    CommandHelp("LM", "Lock model setup", "Lock or unlock model setup.", "[0 | 1]", values_text=LOCK_VALUES),
    CommandHelp("LW", "Lock rotor setup", "Lock or unlock rotor setup.", "[0 | 1]", values_text=LOCK_VALUES),
    CommandHelp("LR", "Lock ring setup", "Lock or unlock ring setup.", "[0 | 1]", values_text=LOCK_VALUES),
    CommandHelp(
        "LP", "Lock power button", "Enable or disable power-off by button.",
        "<0 | 1>", values_text="0 = allow power-off by button; 1 = disable it.",
    ),
    CommandHelp(
        "PO",
        "Power off",
        "Turn off the Enigma Touch.",
        None,
        query_supported=False,
    ),
    CommandHelp(
        "MB", "Brightness", "Set lamp brightness.", "<1..5>",
        values_text="1..5 = lamp brightness levels.",
    ),
    CommandHelp(
        "MV",
        "Volume and sound",
        "Select silence, mechanical sound, or Morse output volume.",
        "<0..6>",
        values_text=(
            "0 = silence; 1..3 = mechanical noise; 4..6 = Morse code "
            "(each at three volume levels)."
        ),
    ),
    CommandHelp(
        "ML", "Logging format", "Select serial log format and group size.",
        "<1..4>",
        values_text="1 = short/5; 2 = short/4; 3 = extended/5; 4 = extended/4.",
    ),
    CommandHelp(
        "TB", "Battery timeout", "Set battery power-off timeout in minutes.",
        "<0..99>", values_text=TIMEOUT_VALUES,
    ),
    CommandHelp(
        "TP", "External-power timeout", "Set plugged-in power-off timeout in minutes.",
        "<0..99>", values_text=TIMEOUT_VALUES,
    ),
    CommandHelp(
        "TS", "Screen saver timeout", "Set screen saver timeout in minutes.",
        "<0..99>", values_text=TIMEOUT_VALUES,
    ),
    CommandHelp(
        "TM", "Setup timeout", "Set setup-mode timeout in seconds.",
        "<seconds; 0 disables>",
        values_text="0 = disabled; positive values = seconds of inactivity.",
    ),
    CommandHelp(
        "FW",
        "Firmware version",
        "Query the active Enigma Touch firmware version.",
        None,
        set_supported=False,
        values_text="Three-digit firmware version number.",
    ),
    CommandHelp(
        "RS",
        "Restore defaults",
        "Restore user-interface and Enigma settings to defaults.",
        None,
        query_supported=False,
    ),
    CommandHelp(
        "CS", "Morse speed", "Set Morse character speed in words per minute.",
        "<5..30>", values_text="5..30 = Morse words per minute.",
    ),
    CommandHelp(
        "CF", "Morse frequency", "Set Morse pitch in units of 100 Hz.",
        "<2..20>", values_text="2..20 = 200..2000 Hz in 100 Hz steps.",
    ),
)
COMMANDS = tuple(sorted(COMMANDS, key=lambda command: command.code))


def _is_wsl():
    return sys.platform == "linux" and "microsoft" in platform.release().lower()


def _wslpath(option, path):
    return subprocess.check_output(
        ("wslpath", option, str(path)),
        text=True,
    ).strip()


def relay_wsl_to_windows(args):
    local_app_data = subprocess.check_output(
        ("cmd.exe", "/d", "/c", "echo %LOCALAPPDATA%"),
        text=True,
    ).strip()
    python_windows = PureWindowsPath(local_app_data) / "Programs/Python/Python313/python.exe"
    python_wsl = _wslpath("-u", python_windows)
    script_windows = _wslpath("-w", Path(__file__).resolve())

    command = [
        python_wsl,
        script_windows,
        "--name",
        args.name,
        "--line-delay",
        str(args.line_delay),
        "--final-wait",
        str(args.final_wait),
    ]
    if args.script:
        command.extend(("--script", _wslpath("-w", Path(args.script).resolve())))
    raise SystemExit(subprocess.call(command))


def matching_commands(prefix, typed_code=""):
    typed_code = typed_code.upper()
    return tuple(
        command
        for command in COMMANDS
        if command.code.startswith(typed_code)
        and (
            (prefix == "?" and command.query_supported)
            or (prefix == "!" and command.set_supported)
        )
    )


def find_command(prefix, code):
    code = code.upper()
    return next(
        (
            command
            for command in matching_commands(prefix, code)
            if command.code == code
        ),
        None,
    )


class PromptTerminal:
    def __init__(self, input=None, output=None):
        self._keys = Queue()
        self._notifications = asyncio.Queue()
        self._status = ""
        self._help_lines = ()
        self._partial_data = b""
        self._in_flight = b""
        bindings = KeyBindings()
        for name, value in (
            ("c-c", SESSION_INTERRUPT),
            ("c-d", SESSION_EOF),
            ("c-]", SESSION_BREAK),
            ("escape", SESSION_ESCAPE),
            ("backspace", BACKSPACE),
            ("c-h", BACKSPACE),
            ("enter", "\r"),
            ("up", KEY_UP),
            ("down", KEY_DOWN),
            ("left", KEY_LEFT),
            ("right", KEY_RIGHT),
            ("tab", KEY_TAB),
        ):
            bindings.add(name, eager=True)(
                lambda _event, value=value: self._keys.put(value)
            )

        @bindings.add("<any>")
        def handle_character(event):
            if event.data:
                self._keys.put(event.data)

        content = FormattedTextControl(
            text=self.render_text,
            focusable=True,
            get_cursor_position=lambda: Point(x=self._line_state()[1], y=0),
        )
        self.application = Application(
            layout=Layout(Window(
                content=content, wrap_lines=True, dont_extend_height=True
            )),
            key_bindings=bindings,
            full_screen=False,
            erase_when_done=True,
            input=input,
            output=output,
        )
        self.application.ttimeoutlen = 0.05
        self.application.timeoutlen = 0.1

    def _line_state(self):
        try:
            text = self._partial_data.decode("utf-8")
        except UnicodeDecodeError:
            text = repr(self._partial_data)
            return text, len(text)
        line = []
        cursor = 0
        for char in text:
            if char == "\r":
                line.clear()
                cursor = 0
            elif char == BACKSPACE:
                cursor = max(0, cursor - 1)
            else:
                if cursor == len(line):
                    line.append(char)
                else:
                    line[cursor] = char
                cursor += 1
        return "".join(line), cursor

    def render_text(self):
        line, _ = self._line_state()
        # Keep a cell after the last character so prompt_toolkit can place the cursor there.
        lines = [line + " "] if self._partial_data else []
        if self._status:
            lines.append(f"[{self._status}]")
        else:
            lines.append("")
            lines.extend(self._help_lines)
        return "\n".join(lines)

    def read_key(self):
        return self._keys.get()

    def show_status(self, message):
        self._status = message
        self.application.invalidate()

    def hide(self):
        self._help_lines = ()
        self.application.invalidate()

    def show_choices(self, prefix, typed_code, commands, selected):
        if not commands:
            self._help_lines = (
                f"  No documented {prefix}{typed_code.upper()} command",
            )
        else:
            available = max(0, self.application.output.get_size().rows - 3)
            visible_count = min(POPUP_MAX_CHOICES, available)
            start = max(0, selected - visible_count + 1)
            visible = commands[start:start + visible_count]
            lines = [
                f"  Enigma commands ({prefix}{typed_code.upper()}): "
                "Up/Down select, Enter/Tab/Right insert"
            ]
            for offset, command in enumerate(visible, start):
                marker = ">" if offset == selected else " "
                lines.append(
                    f" {marker} {prefix}{command.code:<3} "
                    f"{command.name} - {command.action(prefix)}"
                )
            self._help_lines = tuple(lines)
        self.application.invalidate()

    def show_details(self, prefix, command):
        arguments = (
            command.set_arguments
            if prefix == "!" and command.set_arguments
            else "(none)"
        )
        lines = [
            f"  {prefix}{command.code} - {command.name}",
            f"  Arguments: {arguments}",
            f"  {command.action(prefix)}",
        ]
        if command.values_text is not None:
            lines.append(f"  Values: {command.values_text}")
        self._help_lines = tuple(lines)
        self.application.invalidate()

    def receive_data(self, data):
        self._notifications.put_nowait(bytes(data))

    async def _print_notifications(self):
        while True:
            data = await self._notifications.get()
            combined = self._partial_data + data
            complete, separator, partial = combined.rpartition(b"\n")
            if separator:
                self._partial_data = partial
                self._in_flight = complete + separator
                self.application.invalidate()

                def write_data():
                    sys.stdout.buffer.write(self._in_flight)
                    sys.stdout.buffer.flush()
                    self._in_flight = b""

                await run_in_terminal(write_data)
            else:
                self._partial_data = combined
                self.application.invalidate()

    async def run(self, client, status):
        if isinstance(self.application.input, DummyInput):
            raise RuntimeError("Interactive mode requires a terminal")
        task = asyncio.create_task(
            send_input(client, self.read_key, popup=self, status=status)
        )

        def on_input_done(_):
            if self.application.is_running and not self.application.is_done:
                self.application.exit()

        task.add_done_callback(on_input_done)
        try:
            def start_notifications():
                self.application.create_background_task(
                    self._print_notifications()
                )

            await self.application.run_async(
                pre_run=start_notifications
            )
            await task
        finally:
            if not task.done():
                self._keys.put(SESSION_INTERRUPT)
                await task
            pending = self._in_flight + self._partial_data
            while not self._notifications.empty():
                pending += self._notifications.get_nowait()
            if pending:
                sys.stdout.buffer.write(pending)
                sys.stdout.buffer.flush()


class _NullPopup:
    def show_choices(self, *_args):
        del _args
        pass

    def show_details(self, *_args):
        del _args
        pass

    def hide(self):
        pass


class UsbStatus:
    def __init__(self, output, popup=None):
        self._output = output
        self._popup = popup
        self.mode = None
        self.generation = 0

    @property
    def serial_connected(self):
        return self.mode is not None and bool(self.mode & USB_SERIAL)

    def _show_mode(self):
        if self.serial_connected:
            message = "Enigma Touch serial connected; input enabled."
        elif self.mode is not None and self.mode & USB_HID:
            message = "Enigma Touch in keyboard mode; serial input disabled."
        else:
            message = "Enigma Touch disconnected; serial input disabled."
        if self._popup is not None:
            self._popup.show_status("" if self.serial_connected else message)
        else:
            self._output.write(f"\r\n[{message}]\r\n")
            self._output.flush()

    def print_notification(self, _, data):
        if not data:
            return
        self._output.buffer.write(data)
        self._output.buffer.flush()
        if not self.serial_connected:
            if not data.endswith(b"\n"):
                self._output.write("\r\n")
            self._show_mode()

    def update(self, data):
        if len(data) != 1 or data[0] & ~(USB_HID | USB_SERIAL):
            raise ValueError(f"Invalid Enigma USB status: {bytes(data)!r}")
        mode = data[0]
        if mode == self.mode:
            return
        self.mode = mode
        self.generation += 1
        if self._popup is not None:
            self._popup.hide()
        self._show_mode()


async def write_data(client, data):
    for offset in range(0, len(data), WRITE_CHUNK_SIZE):
        await client.write_gatt_char(
            NUS_RX,
            data[offset:offset + WRITE_CHUNK_SIZE],
            response=False,
        )


async def send_input(client, read_key, popup=None, status=None):
    popup = popup or _NullPopup()
    if status is None:
        status = UsbStatus(sys.stdout)
        status.mode = USB_SERIAL
    generation = status.generation
    if status.serial_connected:
        await write_data(client, b"\r\n")
    at_line_start = True
    line_editing = False
    in_comment = False
    command_line = ""
    choices = ()
    selected = 0

    try:
        while client.is_connected:
            key = await asyncio.to_thread(read_key)
            if key in (SESSION_INTERRUPT, SESSION_EOF, SESSION_BREAK, SESSION_ESCAPE):
                return
            if status.generation != generation:
                generation = status.generation
                at_line_start = True
                line_editing = False
                in_comment = False
                command_line = ""
                choices = ()
                selected = 0
                popup.hide()
                if status.serial_connected:
                    await write_data(client, b"\r\n")
            if not status.serial_connected:
                continue
            if key is None:
                continue

            if line_editing and not in_comment and key in (KEY_UP, KEY_DOWN):
                if choices:
                    direction = -1 if key == KEY_UP else 1
                    selected = (selected + direction) % len(choices)
                    popup.show_choices(
                        command_line[0],
                        command_line[1:],
                        choices,
                        selected,
                    )
                continue

            if (
                line_editing
                and not in_comment
                and key in (KEY_TAB, KEY_RIGHT, "\r", "\n")
            ):
                command = find_command(command_line[0], command_line[1:3])
                if command is None and choices:
                    command = choices[selected]
                    missing = command.code[len(command_line) - 1:]
                    addition = missing
                    if (
                        command_line[0] == "!"
                        and command.set_arguments is not None
                    ):
                        addition += " "
                    if addition:
                        await write_data(client, addition.encode())
                        command_line += addition
                    popup.show_details(command_line[0], command)
                    continue
                if key in (KEY_TAB, KEY_RIGHT):
                    continue

            if key == BACKSPACE:
                if line_editing and command_line:
                    await write_data(client, ERASE_PREVIOUS)
                    command_line = command_line[:-1]
                    in_comment = ";" in command_line
                    if not command_line:
                        line_editing = False
                        in_comment = False
                        choices = ()
                        selected = 0
                        popup.hide()
                    elif in_comment:
                        popup.hide()
                    else:
                        choices = matching_commands(
                            command_line[0], command_line[1:]
                        )
                        selected = 0
                        command = find_command(
                            command_line[0], command_line[1:3]
                        )
                        if command is not None and len(command_line) >= 3:
                            popup.show_details(command_line[0], command)
                        else:
                            popup.show_choices(
                                command_line[0],
                                command_line[1:],
                                choices,
                                selected,
                            )
                continue
            if key in ("\r", "\n"):
                await write_data(client, b"\r\n")
                at_line_start = True
                line_editing = False
                in_comment = False
                command_line = ""
                choices = ()
                selected = 0
                popup.hide()
                continue
            if key in ("?", "!") and not at_line_start and not in_comment:
                continue
            if key in (KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_TAB):
                continue

            await write_data(client, key.encode())
            if at_line_start:
                line_editing = key in ("?", "!")
                if line_editing:
                    command_line = key
                    choices = matching_commands(key)
                    selected = 0
                    popup.show_choices(key, "", choices, selected)
            elif line_editing:
                command_line += key
                if key == ";" or in_comment:
                    popup.hide()
                else:
                    choices = matching_commands(
                        command_line[0], command_line[1:]
                    )
                    selected = 0
                    command = find_command(
                        command_line[0], command_line[1:3]
                    )
                    if command is not None and len(command_line) >= 3:
                        popup.show_details(command_line[0], command)
                    else:
                        popup.show_choices(
                            command_line[0],
                            command_line[1:],
                            choices,
                            selected,
                        )
            if key == ";":
                in_comment = True
            at_line_start = False
    finally:
        popup.hide()


async def send_script(client, filename, line_delay, final_wait, status=None):
    source = Path(filename).read_text(encoding="utf-8")
    if status is not None and not status.serial_connected:
        raise RuntimeError("Enigma Touch is not connected in USB serial mode.")
    await write_data(client, b"\r\n")
    for line in source.splitlines():
        if status is not None and not status.serial_connected:
            raise RuntimeError("Enigma Touch USB serial disconnected during script.")
        await write_data(client, line.encode() + b"\r\n")
        if line_delay:
            await asyncio.sleep(line_delay)
    if final_wait:
        await asyncio.sleep(final_wait)


async def run(args):
    from bleak import BleakClient, BleakScanner

    print(f"Looking for {args.name}...")
    device = await BleakScanner.find_device_by_name(args.name, timeout=15)
    if device is None:
        raise RuntimeError(
            f"{args.name} was not found. Confirm it is powered on and not already "
            "using both BLE connections."
        )

    print(f"Connecting to {device.name} ({device.address})...")
    async with BleakClient(device) as client:
        if (client.services.get_characteristic(NUS_TX) is None or
                client.services.get_characteristic(NUS_STATUS) is None):
            raise RuntimeError(
                "Nordic UART or USB status characteristic is missing from the "
                "operating-system GATT cache. Install the latest bridge firmware, "
                "remove Enigma Touch BLE from Bluetooth settings, pair again, "
                "and retry."
            )
        terminal = PromptTerminal() if not args.script else None
        status = UsbStatus(sys.stdout, terminal)
        if not args.script:
            print(
                "Connected. Input is live with no local echo; "
                "press Ctrl+C, Ctrl+D, Ctrl+], or Esc to exit."
            )

        def on_status(_, data):
            try:
                status.update(data)
            except ValueError as exc:
                if terminal is not None:
                    terminal.receive_data(f"\r\nUSB status error: {exc}\r\n".encode())
                else:
                    print(f"\r\nUSB status error: {exc}", file=sys.stderr)
                status.update(b"\x00")

        await client.start_notify(NUS_STATUS, on_status)
        status.update(await client.read_gatt_char(NUS_STATUS))
        if terminal is None:
            await client.start_notify(NUS_TX, status.print_notification)
        else:
            await client.start_notify(
                NUS_TX, lambda _, data: terminal.receive_data(data)
            )
        try:
            if terminal is not None:
                await terminal.run(client, status)
            else:
                print(f"Sending command script: {args.script}")
                await send_script(
                    client,
                    args.script,
                    args.line_delay,
                    args.final_wait,
                    status=status,
                )
                print("Command script complete.")
        finally:
            sys.stdout.write("\r\n")
            sys.stdout.flush()


def parse_args():
    parser = argparse.ArgumentParser(description="Enigma Touch BLE NUS terminal")
    parser.add_argument("--name", default="Enigma Touch BLE")
    parser.add_argument(
        "--script",
        type=Path,
        help="send a UTF-8 text file as CRLF-terminated command lines, then exit",
    )
    parser.add_argument(
        "--line-delay",
        type=float,
        default=0.1,
        help="seconds to wait between script lines (default: 0.1)",
    )
    parser.add_argument(
        "--final-wait",
        type=float,
        default=1.0,
        help="seconds to wait after the final script line (default: 1.0)",
    )
    parser.add_argument(
        "--native-wsl",
        action="store_true",
        help="use WSL's own BlueZ adapter instead of relaying to Windows Python",
    )
    args = parser.parse_args()
    if args.line_delay < 0 or args.final_wait < 0:
        parser.error("script delays cannot be negative")
    if args.script and not args.script.is_file():
        parser.error(f"script file does not exist: {args.script}")
    return args


def main():
    args = parse_args()
    if _is_wsl() and not args.native_wsl:
        relay_wsl_to_windows(args)
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except Exception as exc:
        print(f"Terminal error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


if __name__ == "__main__":
    main()
