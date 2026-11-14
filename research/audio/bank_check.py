#!/usr/bin/env python3
"""Offline check of the two firmware banks in a Pro Controller flash dump.

It prints only sizes, offsets and which checksum rules match. It never
prints bytes from the dump, checksum values or hashes, so its output holds
nothing that identifies the controller (serial, MAC, keys) and nothing of
Nintendo's firmware. It reads one local file, uses only the standard
library and opens no network connections.

Usage: python3 -I research/audio/bank_check.py research/audio/data/flash.bin
       [--bank 0x020000:0x050000 --bank 0x080000:0x0B0000]

The default banks are the two equal-sized high-entropy regions from the
flash map in research/headset-audio.md (Phase 2).
"""
import argparse
import hashlib
import struct
import sys
import zlib

DEFAULT_BANKS = [(0x020000, 0x050000), (0x080000, 0x0B0000)]
SYS_AREA = (0x000000, 0x020000)       # " SYS" header block at offset 0
SKIP = [(0x013000, 0x014000)]         # device info (serial, colours): never searched
DSP_HDR = 0x175000                    # DSPH header, CRC32 known good: self-test
HEADER_SIZES = [0x0, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x1000]
TRAILER_SIZES = [0, 4, 8, 16, 32]


# --- checksum algorithms ---------------------------------------------------

def _crc_table(poly, reflect, width):
    tbl = []
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    for i in range(256):
        if reflect:
            c = i
            for _ in range(8):
                c = (c >> 1) ^ poly if c & 1 else c >> 1
        else:
            c = i << (width - 8)
            for _ in range(8):
                c = ((c << 1) ^ poly) & mask if c & top else (c << 1) & mask
        tbl.append(c)
    return tbl


_T32C = _crc_table(0x82F63B78, True, 32)
_T32MPEG = _crc_table(0x04C11DB7, False, 32)
_T16CCITT = _crc_table(0x1021, False, 16)
_T16MODBUS = _crc_table(0xA001, True, 16)


def crc32c(b):
    c = 0xFFFFFFFF
    for x in b:
        c = _T32C[(c ^ x) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def crc32_mpeg2(b):
    c = 0xFFFFFFFF
    for x in b:
        c = ((c << 8) & 0xFFFFFFFF) ^ _T32MPEG[((c >> 24) ^ x) & 0xFF]
    return c


def crc16_ccitt_false(b):
    c = 0xFFFF
    for x in b:
        c = ((c << 8) & 0xFFFF) ^ _T16CCITT[((c >> 8) ^ x) & 0xFF]
    return c


def crc16_modbus(b):
    c = 0xFFFF
    for x in b:
        c = _T16MODBUS[(c ^ x) & 0xFF] ^ (c >> 8)
    return c


def sum32_words(b):
    n = len(b) // 4
    return sum(struct.unpack_from(f"<{n}I", b)) & 0xFFFFFFFF


def xor32_words(b):
    n = len(b) // 4
    v = 0
    for w in struct.unpack_from(f"<{n}I", b):
        v ^= w
    return v


# (name, function, width in bytes). Wide values are searched widely; 16-bit
# ones only at fixed spots, since 2 random bytes match too easily.
ALGOS32 = [
    ("crc32 (zlib)", lambda b: zlib.crc32(b) & 0xFFFFFFFF),
    ("crc32 (zlib), inverted", lambda b: ~zlib.crc32(b) & 0xFFFFFFFF),
    ("crc32c", crc32c),
    ("crc32/mpeg-2", crc32_mpeg2),
    ("adler32", lambda b: zlib.adler32(b) & 0xFFFFFFFF),
    ("sum32 of LE words", sum32_words),
    ("sum32 of bytes", lambda b: sum(b) & 0xFFFFFFFF),
    ("xor32 of LE words", xor32_words),
]
ALGOS16 = [
    ("crc16/ccitt-false", crc16_ccitt_false),
    ("crc16/modbus", crc16_modbus),
    ("sum16 of bytes", lambda b: sum(b) & 0xFFFF),
]
HASHES = [("sha256", 32), ("sha1", 20), ("md5", 16)]


# --- helpers ---------------------------------------------------------------

def used_end(d, start, end):
    """End of the bank's content: one past the last byte that isn't 0xFF."""
    i = end
    while i > start and d[i - 1] == 0xFF:
        i -= 1
    return i


def find_all(hay, needle, base, skip):
    out = []
    i = hay.find(needle)
    while i != -1:
        a = base + i
        if not any(s <= a < e for s, e in skip):
            out.append(a)
        i = hay.find(needle, i + 1)
    return out


def where(addr, bank_start, used):
    if SYS_AREA[0] <= addr < SYS_AREA[1]:
        return f"SYS area +0x{addr - SYS_AREA[0]:X}"
    if bank_start <= addr < bank_start + 0x1000:
        return f"bank header +0x{addr - bank_start:X}"
    if addr >= used - 0x100:
        return f"bank tail (end-0x{used - addr:X})"
    return f"0x{addr:06X}"


def spans(d, start, used, rel_lengths):
    """Candidate (label, lo, hi) spans a checksum could cover."""
    out = []
    for h in HEADER_SIZES:
        for t in TRAILER_SIZES:
            lo, hi = start + h, used - t
            if hi - lo >= 0x100:
                out.append((f"[+0x{h:X} .. end-{t}]", lo, hi))
    # spans given by length fields in the header
    for h in HEADER_SIZES:
        for ln in rel_lengths:
            lo, hi = start + h, start + h + ln
            if hi <= used + 32 and hi - lo >= 0x100:
                out.append((f"[+0x{h:X} .. +0x{h:X}+len@hdr]", lo, hi))
    return out


# --- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("flash")
    ap.add_argument("--bank", action="append",
                    help="START:END, hex ok; give it twice for two banks")
    a = ap.parse_args()
    d = open(a.flash, "rb").read()
    banks = ([tuple(int(x, 0) for x in s.split(":")) for s in a.bank]
             if a.bank else DEFAULT_BANKS)

    print(f"dump size: {len(d)} bytes")

    # Self-test on the DSP header, whose CRC32 is known to be right.
    if len(d) >= DSP_HDR + 24 and d[DSP_HDR:DSP_HDR + 4] == b"DSPH":
        size, crc = struct.unpack_from("<I", d, DSP_HDR + 8)[0], \
            struct.unpack_from("<I", d, DSP_HDR + 20)[0]
        ok = (zlib.crc32(d[DSP_HDR + 24:DSP_HDR + 24 + size]) & 0xFFFFFFFF) == crc
        print(f"self-test (DSP header CRC32): {'pass' if ok else 'FAIL'}")
    else:
        print("self-test (DSP header CRC32): DSPH header not found")

    sys_hay = d[SYS_AREA[0]:SYS_AREA[1]]
    contents = []
    for n, (start, end) in enumerate(banks):
        used = used_end(d, start, end)
        body = d[start:used]
        contents.append(body)
        ff = body.count(0xFF)
        print(f"\nbank {chr(65 + n)}: 0x{start:06X}-0x{end:06X} "
              f"(region {end - start} bytes)")
        print(f"  content up to last non-FF byte: {used - start} bytes")
        print(f"  erased tail: {end - used} bytes; 0xFF bytes inside content: {ff}")
        if used - start < 0x100:
            print("  (empty, skipped)")
            continue

        hdr = d[start:start + 0x40]
        hdr_u32 = struct.unpack_from("<16I", hdr)
        rel_lengths = sorted({v for v in hdr_u32 if 0x100 <= v <= end - start})
        ln_hits = [f"+0x{4 * i:X}" for i, v in enumerate(hdr_u32)
                   if v in (used - start, end - start)]
        print(f"  header words equal to the content or region size: "
              f"{', '.join(ln_hits) or 'none'}")
        sys_len = find_all(sys_hay, struct.pack("<I", used - start), 0, SKIP)
        sys_start = find_all(sys_hay, struct.pack("<I", start), 0, SKIP)
        print(f"  SYS area holds the content size (u32 LE): {'yes' if sys_len else 'no'};"
              f" the bank's start address: {'yes' if sys_start else 'no'}")

        search = (d[start:start + 0x1000], start)
        tail = (d[used - 0x100:used], used - 0x100)
        hits = []
        for label, lo, hi in spans(d, start, used, rel_lengths):
            blob = d[lo:hi]
            for name, fn in ALGOS32:
                v = fn(blob)
                for endian, fmt in (("LE", "<I"), ("BE", ">I")):
                    pat = struct.pack(fmt, v)
                    for hay, base in (search, tail, (sys_hay, SYS_AREA[0])):
                        for addr in find_all(hay, pat, base, SKIP):
                            if lo <= addr < hi:
                                continue  # inside its own span: can't be it
                            hits.append((name, label, endian, where(addr, start, used)))
            for name, fn in ALGOS16:
                v = fn(blob)
                for endian, fmt in (("LE", "<H"), ("BE", ">H")):
                    pat = struct.pack(fmt, v)
                    spots = list(range(start, start + 0x40, 2)) + [hi, hi + 2]
                    for addr in spots:
                        if not lo <= addr < hi and d[addr:addr + 2] == pat:
                            hits.append((name + " (16-bit, weak)", label, endian,
                                         where(addr, start, used)))
            for name, size in HASHES:
                h = hashlib.new(name, blob).digest()
                for hay, base in (search, tail, (sys_hay, SYS_AREA[0])):
                    for addr in find_all(hay, h, base, SKIP):
                        if not lo <= addr < hi:
                            hits.append((name, label, "-", where(addr, start, used)))

        # Self-checking rules: the stored value makes the whole thing come out fixed.
        whole = body
        if len(whole) % 4 == 0:
            if sum32_words(whole) == 0:
                hits.append(("sum32 of LE words == 0", "[whole content]", "-", "-"))
            if xor32_words(whole) == 0:
                hits.append(("xor32 of LE words == 0", "[whole content]", "-", "-"))
        if sum(whole) & 0xFF == 0:
            hits.append(("sum8 == 0 (weak)", "[whole content]", "-", "-"))
        if zlib.crc32(whole) & 0xFFFFFFFF == 0x2144DF1C:
            hits.append(("crc32 residue (CRC appended LE)", "[whole content]", "-", "-"))

        print("  checksum rules that match:" if hits else
              "  checksum rules that match: none")
        for name, label, endian, loc in sorted(set(hits)):
            print(f"    {name:34} over {label:28} {endian:2}  stored at {loc}")

    if len(contents) == 2:
        a_, b_ = contents
        print(f"\nbanks identical: {'yes' if a_ == b_ else 'no'}; "
              f"same content size: {'yes' if len(a_) == len(b_) else 'no'}")


if __name__ == "__main__":
    sys.exit(main())
