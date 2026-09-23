import asyncio
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from prompt_toolkit.data_structures import Size
from prompt_toolkit.input import DummyInput, create_pipe_input
from prompt_toolkit.output import DummyOutput
from prompt_toolkit.output.vt100 import Vt100_Output


MODULE_PATH = Path(__file__).parents[1] / "tools" / "nus_terminal.py"
SPEC = importlib.util.spec_from_file_location("nus_terminal", MODULE_PATH)
nus_terminal = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nus_terminal)


class FakeClient:
    is_connected = True

    def __init__(self):
        self.writes = []

    async def write_gatt_char(self, characteristic, data, response):
        self.writes.append(data)


class RecordingPopup:
    def __init__(self):
        self.events = []

    def show_choices(self, prefix, typed_code, commands, selected):
        self.events.append(
            ("choices", prefix, typed_code, tuple(c.code for c in commands), selected)
        )

    def show_details(self, prefix, command):
        self.events.append(("details", prefix, command.code, command.set_arguments))

    def hide(self):
        self.events.append(("hide",))

    def show_status(self, message):
        self.events.append(("status", message))


class SizedDummyOutput(DummyOutput):
    def get_size(self):
        return Size(rows=8, columns=48)


def make_terminal():
    return nus_terminal.PromptTerminal(input=DummyInput(), output=DummyOutput())


class TerminalTests(unittest.TestCase):
    def test_manual_command_catalog(self):
        self.assertEqual(
            {command.code for command in nus_terminal.COMMANDS},
            {
                "MO", "RO", "RI", "RP", "RD", "UD", "PB", "XM",
                "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8",
                "XE", "XR", "XU", "LM", "LW", "LR", "LP", "PO",
                "MB", "MV", "ML", "TB", "TP", "TS", "TM", "FW",
                "RS", "CS", "CF",
            },
        )

    def test_help_choices_are_sorted_by_command_code(self):
        for prefix in ("?", "!"):
            with self.subTest(prefix=prefix):
                codes = [
                    command.code for command in nus_terminal.matching_commands(prefix)
                ]
                self.assertEqual(codes, sorted(codes))
                filtered = [
                    command.code
                    for command in nus_terminal.matching_commands(prefix, "x")
                ]
                self.assertEqual(filtered, sorted(filtered))

    def test_popup_uses_query_and_set_actions(self):
        command = nus_terminal.find_command("?", "MO")

        query = make_terminal()
        query.show_details("?", command)
        self.assertIn("Query the Enigma model.", query.render_text())
        self.assertNotIn("Set the Enigma model.", query.render_text())

        setter = make_terminal()
        setter.show_details("!", command)
        self.assertIn("Set the Enigma model.", setter.render_text())
        self.assertNotIn("Query the Enigma model.", setter.render_text())

        power = nus_terminal.find_command("!", "PO")
        setter.show_details("!", power)
        self.assertIn("Turn off the Enigma Touch.", setter.render_text())

    def test_popup_omits_extraneous_articles(self):
        examples = {
            ("?", "LM"): "Query lock model setup.",
            ("!", "LM"): "Lock/unlock model setup.",
            ("?", "LP"): "Query power button lock.",
            ("!", "LP"): "Lock/unlock the power button.",
            ("?", "RI"): "Query ring settings.",
            ("!", "XM"): "Set custom model features.",
            ("?", "MV"): "Query volume and sound.",
        }
        for (prefix, code), wording in examples.items():
            with self.subTest(command=f"{prefix}{code}"):
                popup = make_terminal()
                command = nus_terminal.find_command(prefix, code)
                popup.show_choices(prefix, "", (command,), 0)
                self.assertIn(wording, popup.render_text())
                popup.show_details(prefix, command)
                self.assertIn(wording, popup.render_text())

    def test_documented_command_values_are_available_in_query_and_set_help(self):
        for command in nus_terminal.COMMANDS:
            if command.set_arguments is not None or command.code == "FW":
                with self.subTest(command=command.code):
                    self.assertTrue(command.values_text)
        for prefix, code, details in (
            ("?", "MV", ("0 = silence", "1..3 = mechanical noise", "4..6 = Morse code")),
            ("!", "MV", ("0 = silence", "1..3 = mechanical noise", "4..6 = Morse code")),
            ("?", "ML", ("1 = short/5", "4 = extended/4")),
            ("!", "ML", ("1 = short/5", "4 = extended/4")),
            ("?", "XM", ("C = counter", "R = movable reflector")),
            ("!", "LM", ("0 = unlocked", "1 = locked")),
            ("?", "LP", ("0 = allow power-off", "1 = disable it")),
            ("!", "TM", ("0 = disabled", "seconds of inactivity")),
            ("?", "FW", ("Three-digit firmware version number.",)),
        ):
            with self.subTest(command=f"{prefix}{code}"):
                popup = make_terminal()
                popup.show_details(prefix, nus_terminal.find_command(prefix, code))
                rendered = popup.render_text()
                self.assertIn("Values:", rendered)
                for detail in details:
                    self.assertIn(detail, rendered)

        popup = make_terminal()
        popup.show_details("!", nus_terminal.find_command("!", "PO"))
        self.assertNotIn("Values:", popup.render_text())

    def test_help_selection_fits_small_terminal_without_manual_cursor_codes(self):
        popup = nus_terminal.PromptTerminal(
            input=DummyInput(), output=SizedDummyOutput()
        )
        popup.show_choices("!", "", nus_terminal.matching_commands("!"), 0)
        self.assertLessEqual(len(popup.render_text().splitlines()), 7)
        popup.show_details("!", nus_terminal.find_command("!", "MV"))
        self.assertIn("Values:", popup.render_text())
        popup.show_choices("!", "", nus_terminal.matching_commands("!"), 15)
        self.assertIn("> !", popup.render_text())
        popup.hide()
        self.assertEqual(popup.render_text(), "")

    def test_renderer_keeps_input_and_values_visible_in_small_viewport(self):
        async def exercise():
            with create_pipe_input() as pipe:
                output = Vt100_Output(
                    io.StringIO(),
                    get_size=lambda: Size(rows=8, columns=48),
                    term="xterm",
                    enable_cpr=False,
                )
                terminal = nus_terminal.PromptTerminal(input=pipe, output=output)
                terminal.show_choices(
                    "!", "", nus_terminal.matching_commands("!"), 0
                )
                task = asyncio.create_task(terminal.application.run_async())
                await asyncio.sleep(0.05)
                terminal.show_details("!", nus_terminal.find_command("!", "MV"))
                await asyncio.sleep(0.05)
                screen = terminal.application.renderer.last_rendered_screen
                lines = [
                    "".join(
                        screen.data_buffer[row][column].char
                        for column in range(48)
                    )
                    for row in range(8)
                ]
                self.assertIn("!MV - Volume and sound", "\n".join(lines))
                self.assertIn("4..6 = Morse code", "\n".join(lines))
                self.assertEqual(
                    screen.cursor_positions[terminal.application.layout.current_window],
                    nus_terminal.Point(x=0, y=0),
                )
                terminal.application.exit()
                await task

        asyncio.run(exercise())

    def test_renderer_caret_follows_incomplete_serial_line_with_help(self):
        async def exercise():
            with create_pipe_input() as pipe:
                output = Vt100_Output(
                    io.StringIO(),
                    get_size=lambda: Size(rows=8, columns=48),
                    term="xterm",
                    enable_cpr=False,
                )
                terminal = nus_terminal.PromptTerminal(input=pipe, output=output)
                terminal.show_choices(
                    "!", "", nus_terminal.matching_commands("!"), 0
                )

                def start_notifications():
                    terminal.application.create_background_task(
                        terminal._print_notifications()
                    )

                task = asyncio.create_task(
                    terminal.application.run_async(pre_run=start_notifications)
                )
                try:
                    await asyncio.sleep(0.05)
                    terminal.receive_data(b"rem")
                    for _ in range(100):
                        if terminal.render_text().startswith("rem \n"):
                            await asyncio.sleep(0.05)
                            break
                        await asyncio.sleep(0.01)
                    else:
                        self.fail("Incomplete serial line was not rendered")
                    screen = terminal.application.renderer.last_rendered_screen
                    self.assertEqual(
                        screen.cursor_positions[terminal.application.layout.current_window],
                        nus_terminal.Point(x=3, y=0),
                    )
                    terminal.receive_data(b"ote")
                    await asyncio.sleep(0.05)
                    screen = terminal.application.renderer.last_rendered_screen
                    self.assertEqual(
                        screen.cursor_positions[terminal.application.layout.current_window],
                        nus_terminal.Point(x=6, y=0),
                    )
                    terminal.receive_data(b"\rY")
                    await asyncio.sleep(0.05)
                    screen = terminal.application.renderer.last_rendered_screen
                    self.assertEqual(
                        screen.cursor_positions[terminal.application.layout.current_window],
                        nus_terminal.Point(x=1, y=0),
                    )
                    terminal.receive_data(b"ab\x08 \x08")
                    await asyncio.sleep(0.05)
                    screen = terminal.application.renderer.last_rendered_screen
                    self.assertEqual(
                        screen.cursor_positions[terminal.application.layout.current_window],
                        nus_terminal.Point(x=2, y=0),
                    )
                    self.assertEqual(
                        "".join(screen.data_buffer[0][col].char for col in range(4)),
                        "Ya  ",
                    )
                    self.assertNotIn("^H", terminal.render_text())
                    terminal.receive_data(b"Z\x08")
                    await asyncio.sleep(0.05)
                    screen = terminal.application.renderer.last_rendered_screen
                    self.assertEqual(
                        screen.cursor_positions[terminal.application.layout.current_window],
                        nus_terminal.Point(x=2, y=0),
                    )
                    terminal.receive_data(b" \x08")
                    await asyncio.sleep(0.05)
                    screen = terminal.application.renderer.last_rendered_screen
                    self.assertEqual(
                        "".join(screen.data_buffer[0][col].char for col in range(4)),
                        "Ya  ",
                    )
                finally:
                    terminal.application.exit()
                    await task

        asyncio.run(exercise())

    def test_prompt_toolkit_streams_input_and_preserves_help_during_notifications(self):
        async def exercise():
            with create_pipe_input() as pipe:
                client = FakeClient()
                terminal = nus_terminal.PromptTerminal(
                    input=pipe, output=SizedDummyOutput()
                )
                status = nus_terminal.UsbStatus(io.StringIO(), terminal)
                status.update(bytes((nus_terminal.USB_SERIAL,)))
                buffer = io.BytesIO()
                output = io.TextIOWrapper(buffer, encoding="utf-8", newline="")
                with patch.object(nus_terminal.sys, "stdout", output):
                    task = asyncio.create_task(terminal.run(client, status))
                    await asyncio.sleep(0.05)
                    pipe.send_text("!MV")
                    for _ in range(100):
                        if "Values: 0 = silence" in terminal.render_text():
                            break
                        await asyncio.sleep(0.01)
                    else:
                        self.fail("Command value help never appeared")
                    terminal.receive_data(b"rem")
                    for _ in range(100):
                        if terminal.render_text().startswith("rem \n"):
                            break
                        await asyncio.sleep(0.01)
                    else:
                        self.fail("Partial serial line was not displayed live")
                    self.assertEqual(buffer.getvalue(), b"")
                    terminal.receive_data(b"ote\r\n")
                    for _ in range(100):
                        if buffer.getvalue() == b"remote\r\n":
                            break
                        await asyncio.sleep(0.01)
                    else:
                        self.fail("Remote serial output was not displayed")
                    self.assertFalse(terminal.render_text().startswith("remote"))
                    self.assertIn("Values: 0 = silence", terminal.render_text())
                    pipe.send_text("\x04")
                    await asyncio.wait_for(task, 2)
                self.assertEqual(client.writes, [b"\r\n", b"!", b"M", b"V"])

        asyncio.run(exercise())

    def test_prompt_toolkit_ignores_disconnected_keys_except_exit(self):
        async def exercise():
            with create_pipe_input() as pipe:
                client = FakeClient()
                terminal = nus_terminal.PromptTerminal(
                    input=pipe, output=DummyOutput()
                )
                status = nus_terminal.UsbStatus(io.StringIO(), terminal)
                status.update(b"\x00")
                task = asyncio.create_task(terminal.run(client, status))
                await asyncio.sleep(0.05)
                pipe.send_text("?a\r\x08!")
                await asyncio.sleep(0.1)
                self.assertIn("disconnected", terminal.render_text())
                self.assertEqual(client.writes, [])
                pipe.send_text("\x1d")
                await asyncio.wait_for(task, 2)

        asyncio.run(exercise())

    def test_prompt_toolkit_exit_keys(self):
        async def exercise(key):
            with create_pipe_input() as pipe:
                client = FakeClient()
                terminal = nus_terminal.PromptTerminal(
                    input=pipe, output=DummyOutput()
                )
                status = nus_terminal.UsbStatus(io.StringIO(), terminal)
                status.update(bytes((nus_terminal.USB_SERIAL,)))
                task = asyncio.create_task(terminal.run(client, status))
                await asyncio.sleep(0.05)
                pipe.send_text(key)
                await asyncio.wait_for(task, 2)
                self.assertEqual(client.writes, [b"\r\n"])

        for key in ("\x03", "\x04", "\x1d", "\x1b"):
            with self.subTest(key=repr(key)):
                asyncio.run(exercise(key))

    def test_prompt_toolkit_flushes_incomplete_serial_line_on_exit(self):
        async def exercise():
            with create_pipe_input() as pipe:
                terminal = nus_terminal.PromptTerminal(
                    input=pipe, output=DummyOutput()
                )
                status = nus_terminal.UsbStatus(io.StringIO(), terminal)
                status.update(bytes((nus_terminal.USB_SERIAL,)))
                buffer = io.BytesIO()
                output = io.TextIOWrapper(buffer, encoding="utf-8", newline="")
                with patch.object(nus_terminal.sys, "stdout", output):
                    task = asyncio.create_task(terminal.run(FakeClient(), status))
                    await asyncio.sleep(0.05)
                    terminal.receive_data(b"unfinished")
                    for _ in range(100):
                        if terminal.render_text().startswith("unfinished \n"):
                            break
                        await asyncio.sleep(0.01)
                    else:
                        self.fail("Partial output was not displayed")
                    pipe.send_text("\x04")
                    await asyncio.wait_for(task, 2)
                self.assertEqual(buffer.getvalue(), b"unfinished")

        asyncio.run(exercise())

    def test_interactive_mode_requires_a_terminal(self):
        terminal = make_terminal()
        client = FakeClient()
        status = nus_terminal.UsbStatus(io.StringIO(), terminal)
        status.update(bytes((nus_terminal.USB_SERIAL,)))
        with self.assertRaisesRegex(RuntimeError, "requires a terminal"):
            asyncio.run(terminal.run(client, status))
        self.assertEqual(client.writes, [])

    def test_interactive_editing_and_comments(self):
        keys = iter((
            "a",
            "\x08",
            "?",
            ";",
            "?",
            "!",
            "\r",
            "?",
            "a",
            "b",
            "\x08",
            ";",
            "!",
            "\n",
            "\x03",
        ))
        client = FakeClient()

        asyncio.run(nus_terminal.send_input(client, lambda: next(keys)))

        self.assertEqual(
            client.writes,
            [
                b"\r\n",
                b"a",
                b";",
                b"?",
                b"!",
                b"\r\n",
                b"?",
                b"a",
                b"b",
                b"\x08 \x08",
                b";",
                b"!",
                b"\r\n",
            ],
        )

    def test_erasing_command_prefix_requires_newline_for_another_prefix(self):
        keys = iter(("?", "\x08", "!", "?", "a", "\r", "!", "M", "B", "\x03"))
        client = FakeClient()
        popup = RecordingPopup()

        asyncio.run(
            nus_terminal.send_input(client, lambda: next(keys), popup=popup)
        )

        self.assertEqual(
            client.writes,
            [
                b"\r\n",
                b"?",
                b"\x08 \x08",
                b"a",
                b"\r\n",
                b"!",
                b"M",
                b"B",
            ],
        )
        self.assertEqual(
            [event[1] for event in popup.events if event[0] == "choices" and not event[2]],
            ["?", "!"],
        )
        self.assertIn(("details", "!", "MB", "<1..5>"), popup.events)

    def test_ctrl_d_exits_without_transmitting_even_when_usb_disconnected(self):
        for mode in (nus_terminal.USB_SERIAL, 0):
            with self.subTest(mode=mode):
                client = FakeClient()
                popup = RecordingPopup()
                status = nus_terminal.UsbStatus(io.StringIO(), popup)
                status.update(bytes((mode,)))
                keys = iter(("?", nus_terminal.SESSION_EOF))

                asyncio.run(
                    nus_terminal.send_input(
                        client, lambda: next(keys), popup=popup, status=status
                    )
                )

                self.assertEqual(
                    client.writes,
                    [b"\r\n", b"?"] if mode else [],
                )

    def test_arrow_selection_inserts_command_and_shows_arguments(self):
        keys = iter(
            (
                "!",
                nus_terminal.KEY_DOWN,
                "\r",
                "1",
                "2",
                "\r",
                "\x03",
            )
        )
        client = FakeClient()
        popup = RecordingPopup()

        asyncio.run(
            nus_terminal.send_input(client, lambda: next(keys), popup=popup)
        )

        self.assertEqual(
            client.writes[:4],
            [b"\r\n", b"!", b"CS ", b"1"],
        )
        self.assertEqual(client.writes[-1], b"\r\n")
        self.assertIn(
            ("details", "!", "CS", "<5..30>"),
            popup.events,
        )

    def test_typed_command_replaces_choices_with_argument_help(self):
        keys = iter(("!", "M", "B", "\x03"))
        client = FakeClient()
        popup = RecordingPopup()

        asyncio.run(
            nus_terminal.send_input(client, lambda: next(keys), popup=popup)
        )

        self.assertEqual(client.writes, [b"\r\n", b"!", b"M", b"B"])
        self.assertIn(("details", "!", "MB", "<1..5>"), popup.events)

    def test_disconnected_input_is_ignored_until_serial_hotplug(self):
        client = FakeClient()
        popup = RecordingPopup()
        output = io.StringIO()
        status = nus_terminal.UsbStatus(output, popup)
        status.update(b"\x00")
        self.assertIn(
            ("status", "Enigma Touch disconnected; serial input disabled."),
            popup.events,
        )
        keys = iter((
            "?", "!", "a", "\x08", ";", nus_terminal.KEY_UP,
            "\r", "?", "\x03",
        ))
        def read_key():
            key = next(keys)
            if key == "\r":
                status.update(bytes((nus_terminal.USB_SERIAL,)))
            return key

        asyncio.run(
            nus_terminal.send_input(
                client, read_key, popup=popup, status=status
            )
        )

        self.assertEqual(client.writes, [b"\r\n", b"\r\n", b"?"])
        self.assertIn(("status", ""), popup.events)
        self.assertEqual(
            [event for event in popup.events if event[0] == "choices"],
            [("choices", "?", "", tuple(
                command.code for command in nus_terminal.matching_commands("?")
            ), 0)],
        )

    def test_keyboard_mode_notice_replaces_disconnected_line_and_clears_on_serial(self):
        output = io.StringIO()
        status = nus_terminal.UsbStatus(output)
        status.update(b"\x00")
        status.update(bytes((nus_terminal.USB_HID,)))
        self.assertTrue(
            output.getvalue().endswith(
                "\r\n[Enigma Touch in keyboard mode; serial input disabled.]\r\n"
            )
        )
        status.update(bytes((nus_terminal.USB_SERIAL,)))
        self.assertTrue(
            output.getvalue().endswith(
                "\r\n[Enigma Touch serial connected; input enabled.]\r\n"
            )
        )

    def test_late_serial_output_preserves_disconnected_notice(self):
        buffer = io.BytesIO()
        output = io.TextIOWrapper(buffer, encoding="utf-8", newline="")
        status = nus_terminal.UsbStatus(output)
        status.update(b"\x00")
        status.print_notification(None, b"Last output\r\n")
        self.assertEqual(
            buffer.getvalue().decode(),
            "\r\n[Enigma Touch disconnected; serial input disabled.]\r\n"
            "Last output\r\n"
            "\r\n[Enigma Touch disconnected; serial input disabled.]\r\n",
        )

    def test_unplug_clears_command_help_and_blocks_more_input(self):
        client = FakeClient()
        popup = RecordingPopup()
        status = nus_terminal.UsbStatus(io.StringIO(), popup)
        status.update(bytes((nus_terminal.USB_SERIAL,)))
        keys = iter(("?", "M", "B", "\x03"))
        def read_key():
            key = next(keys)
            if key == "M":
                status.update(b"\x00")
            return key

        asyncio.run(
            nus_terminal.send_input(client, read_key, popup=popup, status=status)
        )

        self.assertEqual(client.writes, [b"\r\n", b"?"])
        self.assertNotIn(("details", "?", "MB", "<1..5>"), popup.events)
        self.assertGreaterEqual(popup.events.count(("hide",)), 1)

    def test_status_rejects_malformed_values(self):
        status = nus_terminal.UsbStatus(io.StringIO())
        for value in (b"", b"\x04", b"\x00\x01"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                status.update(value)

    def test_script_fails_without_serial_usb(self):
        client = FakeClient()
        status = nus_terminal.UsbStatus(io.StringIO())
        status.update(bytes((nus_terminal.USB_HID,)))
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "commands.txt"
            script.write_text("?FW\n", encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "USB serial mode"):
                asyncio.run(nus_terminal.send_script(client, script, 0, 0, status))
        self.assertEqual(client.writes, [])

    def test_script_preserves_commands_and_comments(self):
        client = FakeClient()
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "commands.txt"
            script.write_text("?one\n; comment !?\n!two\n", encoding="utf-8")

            asyncio.run(nus_terminal.send_script(client, script, 0, 0))

        self.assertEqual(
            client.writes,
            [
                b"\r\n",
                b"?one\r\n",
                b"; comment !?\r\n",
                b"!two\r\n",
            ],
        )


if __name__ == "__main__":
    unittest.main()
