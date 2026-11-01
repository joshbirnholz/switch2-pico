"""The plugin installs the same udev rule as tools/, and enables wake-up from
the dongle up to the USB controller."""
import importlib.util
import json
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
    spec = importlib.util.spec_from_file_location("test_decky_udev_backend", ROOT / "decky-plugin" / "main.py")
    backend = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {"decky": decky}):
        spec.loader.exec_module(backend)
    return backend


class UdevTests(unittest.TestCase):
    def setUp(self):
        self.backend = load_backend()

    def test_rule_matches_tools(self):
        self.assertEqual(self.backend.UDEV_RULE, (ROOT / "tools" / "70-switch2-pico.rules").read_text())

    def test_runs_as_root(self):
        # Decky runs a plugin as root only with the exact flag "root" (not
        # "_root"); without it the rule and power/wakeup writes are refused.
        flags = json.loads((ROOT / "decky-plugin" / "plugin.json").read_text())["flags"]
        self.assertIn("root", flags)

    def test_rule_has_wakeup(self):
        self.assertIn("power/wakeup", self.backend.UDEV_RULE)
        self.assertIn('ENV{SWITCH2_PICO_WAKE}=="1", RUN+=', self.backend.UDEV_RULE)

    def test_enable_wakeup_up_to_controller(self):
        with tempfile.TemporaryDirectory() as tmp:
            devices = os.path.join(os.path.realpath(tmp), "devices") + "/"
            chain = ["pci0000:00", "0000:00:08.1", "0000:04:00.3", "usb3", "3-1", "3-1.2"]
            state = {"pci0000:00": "disabled", "0000:00:08.1": "disabled", "0000:04:00.3": "disabled",
                     "usb3": "enabled", "3-1.2": "disabled"}   # 3-1: a hub without the attribute
            d = devices
            for name in chain:
                d = os.path.join(d, name)
                os.makedirs(os.path.join(d, "power"))
                if name in state:
                    with open(os.path.join(d, "power", "wakeup"), "w") as f:
                        f.write(state[name] + "\n")
            with patch.object(self.backend, "SYS_DEVICES", devices):
                changed = self.backend.enable_wakeup(d)
            self.assertEqual(changed, ["3-1.2", "0000:04:00.3"])
            d = devices
            got = {}
            for name in chain:
                d = os.path.join(d, name)
                v = self.backend._read(os.path.join(d, "power", "wakeup"))
                if v is not None:
                    got[name] = v
            self.assertEqual(got, {"pci0000:00": "disabled", "0000:00:08.1": "disabled", "0000:04:00.3": "enabled",
                                   "usb3": "enabled", "3-1.2": "enabled"})

    def test_wakeup_follows_setting(self):
        import asyncio
        from unittest.mock import AsyncMock
        with tempfile.TemporaryDirectory() as tmp:
            devices = os.path.join(os.path.realpath(tmp), "devices") + "/"
            hub = os.path.join(devices, "0000:02:00.0", "usb1")
            dongle = os.path.join(hub, "1-3")
            for d in (os.path.dirname(hub), hub, dongle):
                os.makedirs(os.path.join(d, "power"), exist_ok=True)
                with open(os.path.join(d, "power", "wakeup"), "w") as f:
                    f.write("enabled\n")
            plugin = self.backend.Plugin()
            plugin.dongle = Mock(info={"sys": dongle, "dev": 23})
            plugin.wakeup_for = None
            plugin._json = AsyncMock(return_value={"usb_wakeup": 0})
            with patch.object(self.backend, "SYS_DEVICES", devices):
                asyncio.run(plugin._apply_wakeup())
                # Off: only the dongle, not the hub or controller it shares.
                self.assertEqual(self.backend._read(os.path.join(dongle, "power", "wakeup")), "disabled")
                self.assertEqual(self.backend._read(os.path.join(hub, "power", "wakeup")), "enabled")
                plugin._json = AsyncMock(return_value={"usb_wakeup": 1})
                asyncio.run(plugin._apply_wakeup())
                self.assertEqual(self.backend._read(os.path.join(dongle, "power", "wakeup")), "enabled")


if __name__ == "__main__":
    unittest.main()
