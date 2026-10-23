#!/usr/bin/env python3
"""Read-only Linux checks before updating/restarting a dongle during research.

Refuse open ALSA playback/capture handles, even if merely prepared/suspended,
and refuse unresponsive PipeWire/WirePlumber queries. Never stop applications,
change routing/profiles, or restart audio services to make a check pass.
"""
from pathlib import Path
import subprocess


def require_idle(sys_sound=Path("/sys/class/sound"), proc_sound=Path("/proc/asound")):
    for card in sys_sound.glob("card[0-9]*"):
        device = (card / "device").resolve()
        # ALSA's device is normally the USB interface, below the USB device.
        products = [p / "product" for p in (device, *device.parents)]
        try:
            product = next((p.read_text().strip() for p in products if p.is_file()), "")
            if not product.startswith("Switch2-Pico"):
                continue
            pcm = proc_sound / card.name
            statuses = list(pcm.glob("pcm*/sub*/status"))
            if not statuses:
                raise SystemExit(f"Cannot verify {card.name}'s audio handles; update refused.")
            for status in statuses:
                text = status.read_text().strip()
                if text != "closed":
                    state = text.splitlines()[0] if text else "unknown"
                    raise SystemExit(
                        f"Dongle audio is open ({status}: {state}). Close playback and recording "
                        "applications before updating; no firmware was restarted."
                    )
        except OSError as exc:
            raise SystemExit(f"Cannot verify dongle audio is closed: {exc}") from exc


def require_healthy_audio():
    for args in (["wpctl", "status", "-n"],
                 ["wpctl", "settings", "monitor.alsa.enable-external-volume-control"]):
        try:
            result = subprocess.run(args, capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise SystemExit(f"Audio health check failed ({' '.join(args)}): {exc}. Update refused.") from exc
        if result.returncode:
            raise SystemExit(f"Audio health check failed: {' '.join(args)}: {result.stderr.strip()}. Update refused.")


if __name__ == "__main__":
    require_idle()
    require_healthy_audio()
    print("Dongle audio handles are closed; host audio queries respond. This does not verify audible playback.")
