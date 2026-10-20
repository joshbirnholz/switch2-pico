#!/usr/bin/env python3
"""Decode a sniff.py capture (.pcap, nordic_ble records) offline.

Re-parsing records with SnifferAPI's Packet shifts the payload by a byte
(it adjusts the list it was given), so this reads the records directly:
board id, then the UART packet (length, protocol version, counter, id),
then the packet header (length, flags, channel, RSSI, event counter,
timestamp), then the BLE packet (access address, header, length,
payload). Flags: bit 0 CRC OK, bit 1 direction (central to peripheral),
bit 2 encrypted, bit 3 MIC OK. The firmware has already decrypted packets
it has the key for and dropped their MIC.

Prints connection traffic through sniff.py's Decoder (ATT per handle,
commands inside rumble+command, link-layer control).

Usage: .venv/bin/python research/audio/decode_pcap.py CAPTURE.pcap [--all]
(--all prints the high-rate reports too, one line each)
"""
import io
import struct
import sys
from pathlib import Path

sys.argv = sys.argv[:1] + [a for a in sys.argv[1:]]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import sniff  # noqa: E402


class P:
    """The parts of a SnifferAPI packet the Decoder uses."""


class B:
    pass


def records(path):
    d = Path(path).read_bytes()
    o = 24
    while o + 16 <= len(d):
        ts, tu, il, _ = struct.unpack("<IIII", d[o:o + 16])
        yield ts + tu / 1e6, d[o + 16:o + 16 + il]
        o += 16 + il


def parse(t, r):
    if len(r) < 23 or r[6] != 0x06:     # EVENT_PACKET_DATA_PDU / ADV PDU id varies; keep data PDUs
        pass
    hdr_len = r[7]
    flags = r[8]
    channel = r[9]
    ble = r[7 + hdr_len:] if hdr_len >= 10 else r[17:]
    aa = int.from_bytes(ble[0:4], "little")
    if aa == 0x8E89BED6:
        return None                      # advertising
    p = P()
    p.time = t
    p.crcOK = bool(flags & 1)
    p.direction = bool(flags & 2)
    p.encrypted = bool(flags & 4)
    p.micOK = bool(flags & 8)
    p.channel = channel
    b = B()
    b.type = 0
    b.llid = ble[4] & 3
    b.length = ble[5]
    b.payload = list(ble[6:6 + b.length])
    p.blePacket = b
    return p


def main():
    path = sys.argv[1]
    show_all = "--all" in sys.argv
    lines = []

    class NoSniffer:
        def sendLegacyLTK(self, k):
            pass

        def sendSCLTK(self, k):
            pass

    log = io.StringIO()
    dec = sniff.Decoder(lines.append, log, NoSniffer())
    # The firmware drops the MIC of packets it decrypted: don't strip it again.
    orig = dec.packet

    def packet(p):
        if p.encrypted and p.micOK:
            p.encrypted = False
        orig(p)
    dec.packet = packet
    for t, r in records(path):
        p = parse(t, r)
        if p:
            dec.packet(p)
    for line in lines:
        print(line)
    if show_all:
        print(log.getvalue(), end="")
    else:
        Path(path).with_suffix(".decoded.log").write_text(log.getvalue())
        print(f"(every ATT packet: {Path(path).with_suffix('.decoded.log')})")


if __name__ == "__main__":
    main()
