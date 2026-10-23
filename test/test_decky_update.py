"""An explicit restart refusal must not turn into an update-success response."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, Mock, patch


class UpdateTests(unittest.IsolatedAsyncioTestCase):
    async def test_audio_busy_restart_is_reported(self):
        decky = types.ModuleType("decky")
        decky.emit = AsyncMock()
        decky.logger = Mock()
        path = Path(__file__).resolve().parents[1] / "decky-plugin" / "main.py"
        spec = importlib.util.spec_from_file_location("test_decky_backend", path)
        backend = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {"decky": decky}):
            spec.loader.exec_module(backend)
        plugin = backend.Plugin()
        plugin.updating = False
        plugin.latest = {"sha": "test"}
        plugin.check_update = AsyncMock(return_value={"ok": True, "file": "fw.uf2", "latest": "test"})
        plugin._json = AsyncMock(return_value={"family": 1, "base": 0, "max": 4096})

        async def request(method, path, *args):
            if path == "/api/action?do=reboot":
                return 409, b"Close headphone playback and microphone recording before restarting the dongle."
            return 200, b"ok"

        plugin._request = AsyncMock(side_effect=request)
        with patch.object(backend.asyncio, "to_thread", new=AsyncMock(return_value=b"uf2")), \
             patch.object(backend, "uf2_image", return_value=b"firmware"):
            result = await plugin.install_update()
        self.assertFalse(result["ok"])
        self.assertIn("Close headphone playback", result["error"])
        self.assertFalse(plugin.updating)


if __name__ == "__main__":
    unittest.main()
