from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "research" / "audio"))
import audio_preflight
import fw_push
import lifecycle_probe


class AudioPreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.sys = root / "sound"
        self.proc = root / "asound"
        self.device = self.sys / "card2" / "device"
        self.device.mkdir(parents=True)
        (self.device / "product").write_text("Switch2-Pico SInput\n")
        self.status = self.proc / "card2" / "pcm0p" / "sub0" / "status"
        self.status.parent.mkdir(parents=True)
        self.status.write_text("closed\n")

    def check_idle(self):
        audio_preflight.require_idle(self.sys, self.proc)

    def test_closed_handles_pass(self):
        self.check_idle()

    def test_running_prepared_and_suspended_handles_refuse(self):
        for state in ("RUNNING", "PREPARED", "SUSPENDED", "SETUP"):
            with self.subTest(state=state):
                self.status.write_text(f"state: {state}\nowner_pid: 123\n")
                with self.assertRaises(SystemExit):
                    self.check_idle()

    def test_capture_is_checked_independently(self):
        capture = self.proc / "card2" / "pcm0c" / "sub0" / "status"
        capture.parent.mkdir(parents=True)
        capture.write_text("state: RUNNING\n")
        with self.assertRaises(SystemExit):
            self.check_idle()

    def test_missing_status_refuses(self):
        self.status.unlink()
        with self.assertRaises(SystemExit):
            self.check_idle()

    def test_other_sound_cards_are_not_touched(self):
        (self.device / "product").write_text("HDMI Audio")
        self.status.write_text("state: RUNNING\n")
        self.check_idle()

    def test_usb_product_can_be_on_parent_device(self):
        (self.device / "product").unlink()
        (self.device.parent / "product").write_text("Switch2-Pico SInput")
        self.status.write_text("state: RUNNING\n")
        with self.assertRaises(SystemExit):
            self.check_idle()

    def test_config_only_device_has_no_audio_handles(self):
        audio_preflight.require_idle(self.sys / "absent", self.proc)

    def test_unresponsive_audio_service_refuses(self):
        with patch.object(audio_preflight.subprocess, "run", side_effect=subprocess.TimeoutExpired("wpctl", 5)):
            with self.assertRaises(SystemExit):
                audio_preflight.require_healthy_audio()

    def test_failed_audio_query_refuses(self):
        result = subprocess.CompletedProcess([], 1, "", "settings unavailable")
        with patch.object(audio_preflight.subprocess, "run", return_value=result):
            with self.assertRaises(SystemExit):
                audio_preflight.require_healthy_audio()

    def test_both_health_queries_must_complete(self):
        result = subprocess.CompletedProcess([], 0, "Value: false", "")
        with patch.object(audio_preflight.subprocess, "run", return_value=result) as run:
            audio_preflight.require_healthy_audio()
            self.assertEqual(run.call_count, 2)
            self.assertTrue(all(c.kwargs["timeout"] == 5 for c in run.call_args_list))

    def test_update_does_not_write_if_audio_reopens(self):
        with patch.object(fw_push, "require_idle", side_effect=SystemExit("audio open")), \
             patch.object(fw_push, "request") as request:
            with self.assertRaises(SystemExit):
                fw_push.post("/api/fw/chunk?off=0", b"firmware")
            request.assert_not_called()

    def test_restart_refusal_is_not_reported_as_success(self):
        with patch.object(fw_push, "require_idle"), \
             patch.object(fw_push, "request", return_value=(409, b"audio open")):
            with self.assertRaises(SystemExit):
                fw_push.post("/api/action?do=reboot")

    def test_hardware_probe_sends_no_post_without_active_guarded_build(self):
        with patch.object(lifecycle_probe, "require_healthy_audio"), \
             patch.object(lifecycle_probe, "find", return_value=(1, 2)), \
             patch.object(lifecycle_probe, "request", return_value=(200, b'{"audio_active":false}')) as request:
            with self.assertRaises(SystemExit):
                lifecycle_probe.check_guards()
            request.assert_called_once_with("GET", "/api/status")

    def test_hardware_probe_requests_an_actual_settings_change(self):
        bodies = []

        def request(method, path, body=b""):
            if path == "/api/status":
                return 200, b'{"audio_active":true}'
            if method == "GET":
                return 200, b'{"usb_interval":1}'
            if path == "/api/settings":
                bodies.append(body)
            return 409, b"audio open"

        with patch.object(lifecycle_probe, "require_healthy_audio"), \
             patch.object(lifecycle_probe, "find", return_value=(1, 2)), \
             patch.object(lifecycle_probe, "request", side_effect=request), \
             patch("builtins.print"):
            lifecycle_probe.check_guards()
        self.assertEqual(bodies, [b"usb_interval=2"])


if __name__ == "__main__":
    unittest.main()
