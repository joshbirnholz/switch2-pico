#!/usr/bin/env python3
"""Set the kernel's LE connection parameters for one device (run as root).

The kernel keeps a parameter entry per LE address. It is created with the
defaults of the moment, on the first connection, and outlives BlueZ
restarts (bluetoothd only clears entries when it loads more than one), so
MinConnectionInterval in main.conf doesn't reach a device we already
connected to. MGMT Load Connection Parameters with one entry updates it.

Usage: sudo .venv/bin/python -I research/audio/set_conn_params.py \
           [ADDRESS] [min max latency timeout]
Intervals in 1.25 ms units, timeout in 10 ms units. Defaults: the
controller, 6 12 0 200 (7.5-15 ms, no latency, 2 s supervision timeout).
"""
import ctypes
import socket
import struct
import sys

BTPROTO_HCI = 1
HCI_DEV_NONE = 0xFFFF
HCI_CHANNEL_CONTROL = 3
MGMT_OP_LOAD_CONN_PARAM = 0x0035
MGMT_EV_CMD_COMPLETE = 0x0001
MGMT_EV_CMD_STATUS = 0x0002
BDADDR_LE_PUBLIC = 1


class SockaddrHci(ctypes.Structure):
    _fields_ = [("family", ctypes.c_ushort), ("dev", ctypes.c_ushort), ("channel", ctypes.c_ushort)]


def main():
    args = sys.argv[1:]
    addr = args.pop(0) if args and ":" in args[0] else "3C:A9:AB:69:58:AF"
    mn, mx, lat, to = (int(x, 0) for x in args) if len(args) == 4 else (6, 12, 0, 200)
    index = 0   # hci0

    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, BTPROTO_HCI)
    libc = ctypes.CDLL(None, use_errno=True)
    sa = SockaddrHci(socket.AF_BLUETOOTH, HCI_DEV_NONE, HCI_CHANNEL_CONTROL)
    if libc.bind(s.fileno(), ctypes.byref(sa), ctypes.sizeof(sa)) < 0:
        e = ctypes.get_errno()
        sys.exit(f"bind to the management channel failed: {e} (run as root)")

    bdaddr = bytes.fromhex(addr.replace(":", ""))[::-1]
    entry = bdaddr + struct.pack("<BHHHH", BDADDR_LE_PUBLIC, mn, mx, lat, to)
    params = struct.pack("<H", 1) + entry
    s.send(struct.pack("<HHH", MGMT_OP_LOAD_CONN_PARAM, index, len(params)) + params)

    s.settimeout(2)
    while True:
        ev = s.recv(512)
        code, idx, ln = struct.unpack("<HHH", ev[:6])
        if code in (MGMT_EV_CMD_COMPLETE, MGMT_EV_CMD_STATUS):
            op, status = struct.unpack("<HB", ev[6:9])
            if op == MGMT_OP_LOAD_CONN_PARAM:
                break
    print(f"{addr}: min {mn * 1.25} ms, max {mx * 1.25} ms, latency {lat}, timeout {to * 10} ms "
          f"-> {'OK' if status == 0 else f'status {status:#04x}'}")


if __name__ == "__main__":
    main()
