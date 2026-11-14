"""The plugin notices, from Steam's log, when Steam lost the race for the
SInput hidraw node, and rebinds the HID interface until Steam has it."""
import asyncio
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]


def load_backend():
    decky = types.ModuleType("decky")
    decky.logger = Mock()
    spec = importlib.util.spec_from_file_location("test_decky_sinput_backend", ROOT / "decky-plugin" / "main.py")
    backend = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {"decky": decky}):
        spec.loader.exec_module(backend)
    return backend


# As Steam logs them (console_log.txt).
def denied(node):
    return (f"[2026-11-14 14:54:56] HIDAPI_SetupDeviceDriver() couldn't open {node}: "
            f"Failed to open a device with path '{node}': Permission denied")


def added(node, ok):
    driver = "SDL_JOYSTICK_HIDAPI_SINPUT (ENABLED)" if ok else "NONE (DISABLED)"
    return (f"[2026-11-14 14:54:56] Added HIDAPI device 'Switch2-Pico SInput' VID 0x2e8a, PID 0x10c6, "
            f"bluetooth 0, version 273, serial 000000000001, interface 0, interface_class 0, "
            f"interface_subclass 0, interface_protocol 0, usage page 0x0001, usage 0x0005, "
            f"path = {node}, driver = {driver}")


def removed(node):
    return added(node, False).replace("Added", "Removing")


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.b = load_backend()

    def test_denied_then_disabled_is_lost(self):
        s = self.b.SteamSinput()
        self.assertIsNone(s.feed(denied("/dev/hidraw2")))
        self.assertEqual(s.feed(added("/dev/hidraw2", False)), ("added", "/dev/hidraw2", "lost"))
        self.assertEqual(s.lost(), ["/dev/hidraw2"])

    def test_enabled_is_ok(self):
        s = self.b.SteamSinput()
        self.assertEqual(s.feed(added("/dev/hidraw10", True)), ("added", "/dev/hidraw10", "ok"))
        self.assertEqual(s.lost(), [])

    def test_disabled_without_denied_is_left_alone(self):
        # SInput support turned off in Steam, for example: not ours to undo.
        s = self.b.SteamSinput()
        self.assertEqual(s.feed(added("/dev/hidraw2", False)), ("added", "/dev/hidraw2", "off"))
        self.assertEqual(s.lost(), [])

    def test_removed_forgets(self):
        s = self.b.SteamSinput()
        s.feed(denied("/dev/hidraw2"))
        s.feed(added("/dev/hidraw2", False))
        self.assertEqual(s.feed(removed("/dev/hidraw2")), ("removed", "/dev/hidraw2", None))
        self.assertEqual(s.lost(), [])
        # A later open of the same node that fails for another reason isn't "lost".
        self.assertEqual(s.feed(added("/dev/hidraw2", False))[2], "off")

    def test_other_devices_ignored(self):
        s = self.b.SteamSinput()
        line = added("/dev/hidraw1", False).replace("VID 0x2e8a, PID 0x10c6", "VID 0x2dc8, PID 0x3109")
        s.feed(denied("/dev/hidraw1"))
        self.assertIsNone(s.feed(line))
        self.assertEqual(s.lost(), [])


class LogTailTests(unittest.TestCase):
    def setUp(self):
        self.b = load_backend()
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "console_log.txt")

    def tearDown(self):
        self.dir.cleanup()

    def test_partial_lines_and_new_file(self):
        with open(self.path, "w") as f:
            f.write("one\ntw")
        t = self.b.LogTail(self.path)
        self.assertEqual(t.read(), ["one"])
        gen = t.gen
        with open(self.path, "a") as f:
            f.write("o\n")
        self.assertEqual(t.read(), ["two"])
        self.assertEqual(t.read(), [])
        # Steam restarted: the old log is renamed away and a new one started.
        os.rename(self.path, self.path + ".previous")
        with open(self.path, "w") as f:
            f.write("three\n")
        self.assertEqual(t.read(), ["three"])
        self.assertEqual(t.gen, gen + 1)

    def test_missing_file(self):
        self.assertEqual(self.b.LogTail(self.path).read(), [])


class FakeSteam:
    """Steam's reactions to the HID interface being unbound and bound,
    written to its log; opens fail while fails > 0."""

    def __init__(self, path, fails):
        self.path, self.fails, self.node, self.ops = path, fails, "/dev/hidraw2", []

    def log(self, *lines):
        with open(self.path, "a") as f:
            f.write("".join(l + "\n" for l in lines))

    def sysfs_write(self, path, value):
        op = os.path.basename(path)
        self.ops.append((op, value))
        if op == "unbind":
            self.log(removed(self.node))
        elif self.fails > 0:
            self.fails -= 1
            self.log(denied(self.node), added(self.node, False))
        else:
            self.log(added(self.node, True))


class WatchTests(unittest.TestCase):
    def setUp(self):
        self.b = load_backend()
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "console_log.txt")

    def tearDown(self):
        self.dir.cleanup()

    def run_watch(self, steam, seconds, dongle_nodes=("/dev/hidraw2",)):
        plugin = self.b.Plugin()
        plugin.updating = False

        async def go():
            task = asyncio.ensure_future(plugin._sinput_watch())
            await asyncio.sleep(seconds)
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

        with patch.multiple(self.b, steam_log_path=lambda: self.path, SINPUT_LOG_S=0.01,
                            SINPUT_REMOVE_WAIT_S=0.5, SINPUT_ADD_WAIT_S=0.5,
                            sinput_hid_device=lambda node: ("0003:2E8A:10C6.0012", "/sys/bus/hid/drivers/hid-generic")
                            if node in dongle_nodes else None,
                            udev_settle=lambda: None, sysfs_write=steam.sysfs_write):
            asyncio.run(go())

    def test_rebinds_until_steam_has_it(self):
        steam = FakeSteam(self.path, fails=2)
        # The failure happened before the plugin started.
        steam.log("unrelated", denied("/dev/hidraw2"), added("/dev/hidraw2", False))
        self.run_watch(steam, 1.0)
        dev = "0003:2E8A:10C6.0012"
        self.assertEqual(steam.ops, [("unbind", dev), ("bind", dev)] * 3)

    def test_gives_up_after_the_tries(self):
        steam = FakeSteam(self.path, fails=100)
        steam.log(denied("/dev/hidraw2"), added("/dev/hidraw2", False))
        self.run_watch(steam, 1.5)
        self.assertEqual(len(steam.ops), 2 * self.b.SINPUT_FIX_TRIES)
        # ... until the dongle comes back on its own (the controller reconnects).
        steam.ops.clear()
        steam.fails = 0
        steam.log(removed("/dev/hidraw2"), denied("/dev/hidraw2"), added("/dev/hidraw2", False))
        self.run_watch(steam, 0.5)
        self.assertEqual(len(steam.ops), 2)

    def test_stale_node_from_before_a_restart(self):
        # Lost before Steam (or the PC) restarted, which logs no removal; the
        # node is another device's now. It comes first and mustn't block the
        # dongle's current one.
        steam = FakeSteam(self.path, fails=0)
        steam.log(added("/dev/hidraw2", True), denied("/dev/hidraw11"), added("/dev/hidraw11", False),
                  removed("/dev/hidraw2"), denied("/dev/hidraw2"), added("/dev/hidraw2", False))
        self.run_watch(steam, 0.5)
        dev = "0003:2E8A:10C6.0012"
        self.assertEqual(steam.ops, [("unbind", dev), ("bind", dev)])

    def test_leaves_working_and_disabled_alone(self):
        steam = FakeSteam(self.path, fails=0)
        steam.log(added("/dev/hidraw2", True), removed("/dev/hidraw2"), added("/dev/hidraw2", False))
        self.run_watch(steam, 0.3)
        self.assertEqual(steam.ops, [])


if __name__ == "__main__":
    unittest.main()
