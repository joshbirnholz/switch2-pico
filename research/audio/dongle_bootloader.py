#!/usr/bin/env python3
"""Restart a switch2-pico board into its UF2 bootloader over USB, the same
request the configuration page's update sends (POST /api/action?do=bootloader
over the "Switch2-Pico Config" interface, as decky-plugin/main.py does).
Used to put a spare nRF52840 board into UF2 mode for the BLE sniffer
firmware without bridging RST to GND.

Needs write access to the USB device: run with sudo unless the project's
udev rule (tools/70-switch2-pico.rules) is installed.
  sudo .venv/bin/python -I research/audio/dongle_bootloader.py
"""
import fcntl
import glob
import os
import struct
import ctypes
import sys

CONFIG_INTERFACE_NAME = "Switch2-Pico Config"
USBDEVFS_BULK = 0xC0185502
USBDEVFS_CLAIMINTERFACE = 0x8004550F
USBDEVFS_RELEASEINTERFACE = 0x80045510


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def find():
    for itf_dir in glob.glob("/sys/bus/usb/devices/*:*"):
        if read(os.path.join(itf_dir, "interface")) != CONFIG_INTERFACE_NAME:
            continue
        dev_dir = os.path.dirname(os.path.realpath(itf_dir))
        eps = {}
        for ep_dir in glob.glob(os.path.join(itf_dir, "ep_*")):
            if read(os.path.join(ep_dir, "type")) == "Bulk":
                a = int(read(os.path.join(ep_dir, "bEndpointAddress")), 16)
                eps["in" if a & 0x80 else "out"] = a
        return (int(read(os.path.join(dev_dir, "busnum"))), int(read(os.path.join(dev_dir, "devnum"))),
                int(read(os.path.join(itf_dir, "bInterfaceNumber")), 16), eps)
    raise SystemExit("no switch2-pico board found")


def bulk(fd, ep, data, length, timeout_ms=3000):
    buf = ctypes.create_string_buffer(length) if data is None else ctypes.create_string_buffer(data, length)
    arg = bytearray(struct.pack("IIIxxxxQ", ep, length, timeout_ms, ctypes.addressof(buf)))
    n = fcntl.ioctl(fd, USBDEVFS_BULK, arg)
    return buf.raw[:n]


def main():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from audio_preflight import require_idle
    require_idle()
    bus, dev, itf, eps = find()
    fd = os.open(f"/dev/bus/usb/{bus:03d}/{dev:03d}", os.O_RDWR)
    try:
        fcntl.ioctl(fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", itf))
        payload = b"POST /api/action?do=bootloader\n"
        msg = b"S2PQ" + struct.pack("<I", len(payload)) + payload
        bulk(fd, eps["out"], msg, len(msg))
        got = bulk(fd, eps["in"], None, 4096)
        status = struct.unpack_from("<H", got, 4)[0] if got[:4] == b"S2PR" else None
        print(f"board answered {status}: {got[12:].decode(errors='replace')}")
        if status != 200:
            raise SystemExit("Bootloader request was refused; no restart confirmed.")
        print("it will restart into its UF2 bootloader in half a second; a USB drive should appear")
    finally:
        try:
            fcntl.ioctl(fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", itf))
        except OSError:
            pass
        os.close(fd)


if __name__ == "__main__":
    main()
