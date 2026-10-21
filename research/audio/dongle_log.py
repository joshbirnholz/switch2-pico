#!/usr/bin/env python3
"""Print a switch2-pico dongle's firmware log (GET /api/log over the
"Switch2-Pico Config" USB interface, as decky-plugin/main.py does).

Needs access to the USB device: the project's udev rule
(tools/70-switch2-pico.rules) or sudo.
  .venv/bin/python -I research/audio/dongle_log.py [--follow] [--grep=TEXT]
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dongle_bootloader import (USBDEVFS_CLAIMINTERFACE, USBDEVFS_RELEASEINTERFACE,  # noqa: E402
                               bulk, find)
import fcntl  # noqa: E402


def request(method, path):
    bus, dev, itf, eps = find()
    fd = os.open(f"/dev/bus/usb/{bus:03d}/{dev:03d}", os.O_RDWR)
    try:
        fcntl.ioctl(fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", itf))
        payload = f"{method} {path}\n".encode()
        bulk(fd, eps["out"], b"S2PQ" + struct.pack("<I", len(payload)) + payload, 8 + len(payload))
        got = b""
        need = 12
        while len(got) < need:
            got += bulk(fd, eps["in"], None, 4096)
            if need == 12 and len(got) >= 12:
                need = 12 + struct.unpack_from("<I", got, 8)[0]
        return got[12:need].decode(errors="replace")
    finally:
        try:
            fcntl.ioctl(fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", itf))
        except OSError:
            pass
        os.close(fd)


def main():
    grep = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--grep=")), None)
    seen = set()
    while True:
        for line in request("GET", "/api/log").splitlines():
            if line in seen or (grep and grep not in line):
                continue
            seen.add(line)
            print(line, flush=True)
        if "--follow" not in sys.argv:
            break
        time.sleep(2)


if __name__ == "__main__":
    main()
