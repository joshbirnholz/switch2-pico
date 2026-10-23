"""Decky delegates the same bounded test as the web page, with validated inputs."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, Mock, patch

class MicTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        decky = types.ModuleType("decky")
        decky.logger = Mock()
        spec = importlib.util.spec_from_file_location("test_mic_backend", Path(__file__).resolve().parents[1] / "decky-plugin/main.py")
        self.backend = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {"decky": decky}):
            spec.loader.exec_module(self.backend)
        self.plugin = self.backend.Plugin()
        self.plugin._json = AsyncMock(return_value={"session": 7, "phase": "ready"})

    async def test_shared_route(self):
        result = await self.plugin.mic_test("record", 7)
        self.assertTrue(result["ok"])
        self.plugin._json.assert_awaited_once_with("POST", "/api/mic-test?session=7&do=record")
        await self.plugin.mic_test("status", 7)
        self.plugin._json.assert_awaited_with("GET", "/api/mic-test?session=7")

    async def test_invalid_inputs_never_reach_device(self):
        for operation, session in [("reboot", 7), ("record", "7&do=reboot"), ("cancel", -1), ("cancel", 2**32)]:
            self.assertFalse((await self.plugin.mic_test(operation, session))["ok"])
        self.plugin._json.assert_not_awaited()

    async def test_refusal_message(self):
        self.plugin._json.side_effect = self.backend.DongleError("Close other playback first.")
        result = await self.plugin.mic_test("open", 0)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "Close other playback first.")

if __name__ == "__main__":
    unittest.main()
