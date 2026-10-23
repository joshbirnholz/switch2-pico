#!/usr/bin/env python3
"""Check host audio health; optionally exercise active-audio refusal paths.

Only use --check-guards on the newly installed guarded build with a bounded
test stream running. Stop on the first failed check. Factory reset and the
bootloader are covered by host unit tests, not sent to hardware here.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_preflight import require_healthy_audio
from dongle_bootloader import find
from fw_push import request


def status():
    code, body = request("GET", "/api/status")
    if code != 200:
        raise SystemExit(f"Status failed: {code}")
    return json.loads(body)


def check_guards():
    require_healthy_audio()
    identity = find()[:2]
    before = status()
    if before.get("audio_active") is not True:
        raise SystemExit("Refusal probe requires a guarded build and an active test audio stream; no POST sent.")
    code, body = request("GET", "/api/settings")
    if code != 200:
        raise SystemExit("Cannot snapshot settings; no POST sent.")
    settings = json.loads(body)
    interval = settings["usb_interval"]
    # Re-submitting the current interval is a no-op and correctly returns
    # 200. Request a different valid interval to exercise the rejection.
    different_interval = 2 if interval == 1 else 1
    checks = [
        ("/api/fw/begin?size=0&crc=0", b""),
        ("/api/fw/chunk?off=0", b""),
        ("/api/fw/end", b""),
        ("/api/settings", f"usb_interval={different_interval}".encode()),
        ("/api/action?do=usb_reconnect", b""),
        ("/api/action?do=reboot", b""),
    ]
    for path, payload in checks:
        if find()[:2] != identity or status().get("audio_active") is not True:
            raise SystemExit("USB identity or stream state changed; stopping before the next POST.")
        code, body = request("POST", path, payload)
        if code != 409:
            raise SystemExit(f"FAIL {path}: expected 409, got {code}; stop tests and save logs.")
        print(f"PASS 409 {path}", flush=True)
    code, body = request("GET", "/api/settings")
    if code != 200 or json.loads(body) != settings:
        raise SystemExit("Settings changed during refused requests; stop tests.")
    if find()[:2] != identity or status().get("audio_active") is not True:
        raise SystemExit("USB device or stream changed during refused requests; stop tests.")
    require_healthy_audio()
    print("PASS host queries, unchanged USB identity/settings, audio still open", flush=True)


if __name__ == "__main__":
    if sys.argv[1:] == ["--check-guards"]:
        check_guards()
    elif not sys.argv[1:]:
        require_healthy_audio()
        s = status()
        print(json.dumps({"usb_address": list(find()[:2]), "usb_mounted": s["usb"]["mounted"],
                          "audio_active": s.get("audio_active"),
                          "link": s["link"]["state"], "usb_config_only": s["usb_config_only"]}))
    else:
        raise SystemExit("usage: lifecycle_probe.py [--check-guards]")
