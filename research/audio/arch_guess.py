#!/usr/bin/env python3
"""Guess a code blob's instruction set from how often each candidate's
common return/prologue encodings appear, against what random data would
give. A real match stands out by an order of magnitude or more.

Usage: .venv/bin/python -I research/audio/arch_guess.py FILE [start] [end]
"""
import re
import sys

# (architecture, what, regex over bytes, alignment the encoding needs)
PATTERNS = [
    ("Xtensa", "retw.n", rb"\x1d\xf0", 1),
    ("Xtensa", "ret.n", rb"\x0d\xf0", 1),
    ("Xtensa", "retw", rb"\x90\x00\x00", 1),
    ("Xtensa", "entry a1,N", rb"\x36[\x41\x61\x81\xa1\xc1\xe1]\x00", 1),
    ("ARM Thumb", "bx lr", rb"\x70\x47", 2),
    ("ARM Thumb", "push {..,lr}", rb"[\x00-\xff]\xb5", 2),
    ("ARM Thumb", "pop {..,pc}", rb"[\x00-\xff]\xbd", 2),
    ("ARM (A32)", "bx lr", rb"\x1e\xff\x2f\xe1", 4),
    ("RISC-V", "c.ret", rb"\x82\x80", 2),
    ("RISC-V", "ret", rb"\x67\x80\x00\x00", 4),
    ("Andes NDS32", "ret5 $lp", rb"\xdd\x9e", 2),
    ("Andes NDS32", "ret $lp", rb"\x4a\x00\x78\x20", 2),
    ("MIPS", "jr ra (LE)", rb"\x08\x00\xe0\x03", 4),
    ("MIPS16/microMIPS", "jr ra", rb"\xe8\x20", 2),
    ("CEVA-TeakLite", "ret (guess)", rb"\x00\x45", 2),
]


def main():
    d = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(d)
    d = d[start:end]
    n = len(d)
    print(f"{n} bytes")
    print(f"{'architecture':18} {'encoding':14} {'count':>7} {'random':>8} {'ratio':>7}")
    for arch, what, pat, align in PATTERNS:
        hits = [m.start() for m in re.finditer(pat, d, re.S)]
        hits = [h for h in hits if h % align == 0]
        # expected count in uniform random bytes at this alignment
        rx = re.compile(pat, re.S)
        length = len(rx.pattern.replace(b"\\x", b"")) // 2 if b"[" not in pat else 2
        probs = 1.0
        for part in re.findall(rb"\[[^\]]*\]|\\x..", pat):
            if part.startswith(b"["):
                body = part[1:-1]
                ranges = re.findall(rb"\\x(..)-\\x(..)|\\x(..)", body)
                cnt = 0
                for a, b, c in ranges:
                    cnt += int(b, 16) - int(a, 16) + 1 if a else 1
                probs *= cnt / 256
            else:
                probs /= 256
        expected = (n / align) * probs
        ratio = len(hits) / expected if expected else 0
        print(f"{arch:18} {what:14} {len(hits):7} {expected:8.1f} {ratio:7.1f}")


if __name__ == "__main__":
    main()
