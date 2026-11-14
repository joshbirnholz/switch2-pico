#!/usr/bin/env python3
"""Make the nRF52840 .uf2 that also carries the S140 SoftDevice.

Usage: add_softdevice_uf2.py SOFTDEVICE.hex APP.uf2 OUT.uf2

SOFTDEVICE.hex is any Intel HEX file holding S140 v6.1.1, such as the
bootloader + SoftDevice .hex files the Adafruit nRF52 core ships. Only the
SoftDevice part (0x1000-0x26000) is taken: the MBR below it and the
bootloader above it stay as they are on the board.

The Adafruit UF2 bootloader writes the SoftDevice from a .uf2 like any other
part of the application, so this one file works on boards that already have
the SoftDevice and on boards that don't (some ship without it, and firmware
such as Nordic's nRF Sniffer overwrites it).
"""
import struct
import sys

SD_START = 0x1000          # after the MBR
SD_END = 0x26000           # where the application starts (S140 v6)
SD_MAGIC = 0x51B1E5DB      # SoftDevice info structure, at SD_START + 0x2000
SD_FWID_S140_611 = 0x00B6
FAMILY = 0xADA52840
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x2000
PAYLOAD = 256


def read_hex(path, lo, hi):
    """Bytes of an Intel HEX file between lo and hi, as {address: byte}."""
    mem = {}
    base = 0
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if line[0] != ":":
                raise ValueError("%s:%d: not Intel HEX" % (path, n))
            rec = bytes.fromhex(line[1:])
            if sum(rec) & 0xFF:
                raise ValueError("%s:%d: bad checksum" % (path, n))
            count, addr, kind = rec[0], (rec[1] << 8) | rec[2], rec[3]
            data = rec[4:4 + count]
            if kind == 0:
                for i, b in enumerate(data):
                    a = base + addr + i
                    if lo <= a < hi:
                        mem[a] = b
            elif kind == 1:
                break
            elif kind == 2:
                base = int.from_bytes(data, "big") << 4
            elif kind == 4:
                base = int.from_bytes(data, "big") << 16
    return mem


def softdevice_image(path):
    mem = read_hex(path, SD_START, SD_END)
    word = lambda a: int.from_bytes(bytes(mem.get(a + i, 0xFF) for i in range(4)), "little")
    info = SD_START + 0x2000
    if word(info + 4) != SD_MAGIC:
        raise ValueError("%s: no SoftDevice at 0x%X" % (path, SD_START))
    fwid = word(info + 0xC) & 0xFFFF
    if fwid != SD_FWID_S140_611:
        raise ValueError("%s: SoftDevice is not S140 v6.1.1 (FWID 0x%04X)" % (path, fwid))
    # A complete S140 v6.1.1 reaches past 0x25000; a truncated file would
    # leave the board with a SoftDevice that looks installed but crashes.
    if max(mem) < 0x25000:
        raise ValueError("%s: SoftDevice looks incomplete (ends at 0x%X)" % (path, max(mem) + 1))
    end = max(mem) + 1
    return bytes(mem.get(a, 0xFF) for a in range(SD_START, end))


def read_uf2(path):
    data = open(path, "rb").read()
    if len(data) % 512:
        raise ValueError("%s: not a .uf2" % path)
    blocks = []
    for o in range(0, len(data), 512):
        m0, m1, flags, addr, size, _no, _num, fam = struct.unpack_from("<8I", data, o)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1:
            raise ValueError("%s: bad block at %d" % (path, o))
        if flags & FLAG_FAMILY and fam != FAMILY:
            raise ValueError("%s: not an nRF52840 image" % path)
        if addr < SD_END:
            raise ValueError("%s: application starts below 0x%X" % (path, SD_END))
        blocks.append((addr, data[o + 32:o + 32 + size]))
    return blocks


def write_uf2(path, blocks):
    out = bytearray()
    for n, (addr, payload) in enumerate(blocks):
        out += struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, FLAG_FAMILY, addr, len(payload), n, len(blocks), FAMILY)
        out += payload.ljust(476, b"\0")
        out += struct.pack("<I", UF2_MAGIC_END)
    open(path, "wb").write(out)


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__.strip().splitlines()[2])
    sd = softdevice_image(argv[1])
    blocks = [(SD_START + o, sd[o:o + PAYLOAD]) for o in range(0, len(sd), PAYLOAD)]
    blocks += read_uf2(argv[2])
    write_uf2(argv[3], blocks)
    print("%s: SoftDevice 0x%X-0x%X + %s (%d blocks)" % (argv[3], SD_START, SD_START + len(sd), argv[2], len(blocks)))


if __name__ == "__main__":
    main(sys.argv)
