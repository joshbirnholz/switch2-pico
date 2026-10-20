#!/usr/bin/env python3
"""Search a binary for codec fingerprints.

1. Exact known tables (int8/int16/int32, little and big endian).
2. Band-edge tables: strictly increasing int16 runs starting at 0. The
   last value is usually the frame's number of spectral lines, which
   gives the frame size even for a codec with no known table here.
3. Sine/cosine tables (MDCT windows and twiddles) in Q15, Q31 and float32,
   matched approximately, for common transform sizes.

Usage: .venv/bin/python -I research/audio/find_tables.py FILE [base]
(base: address of the file's first byte, for the report)
"""
import sys

import numpy as np

KNOWN = {
    "IMA ADPCM step table": [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45],
    "IMA ADPCM index table": [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8],
    "OKI/Dialogic ADPCM step table": [16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66],
    "G.722 QMF coefficients": [3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11],
    "G.722 low-band quantizer (q6)": [0, 35, 72, 110, 150, 190, 233, 276, 323, 370, 422, 473, 530, 587, 650, 714],
    "SBC loudness offset4": [-1, 0, 0, 0, -2, 0, 0, 1, -2, 0, 0, 1, -2, 0, 0, 1],
    "SBC loudness offset8": [-2, 0, 0, 0, 0, 0, 0, 1, -3, 0, 0, 0, 0, 0, 1, 2],
    "CELT eband5ms": [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 34, 40, 48, 60, 78, 100],
    "CELT/Opus e_prob_model (LM0 inter)": [72, 127, 65, 129, 66, 128, 65, 128, 64, 128, 62, 128, 64, 128],
    "Opus SILK NLSF CB1 (NB/MB) first row": [12, 35, 60, 83, 108, 132, 157, 180, 206, 228],
    "aptX quantize table (lf, first values)": [-9785, 7300, 23219, 40400, 59910, 82916],
    "mu-law exp lut": [0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3],
    "MS ADPCM adaptation table": [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230],
    "Nintendo DSP-ADPCM (none fixed; skip)": [],
}

TRANSFORM_SIZES = [16, 20, 30, 32, 40, 48, 60, 64, 80, 96, 120, 128, 160, 192, 240, 256, 320, 360,
                   384, 480, 512, 640, 720, 960, 1024, 1920]


def views(d):
    """(name, element size, array, byte offset of element 0) for each
    type and alignment."""
    out = []
    for off in range(2):
        n = (len(d) - off) // 2
        b = np.frombuffer(d, dtype="<i2", count=n, offset=off)
        out.append(("i16le", 2, b, off))
        out.append(("i16be", 2, np.frombuffer(d, dtype=">i2", count=n, offset=off), off))
    for off in range(4):
        n = (len(d) - off) // 4
        out.append(("i32le", 4, np.frombuffer(d, dtype="<i4", count=n, offset=off), off))
        out.append(("i32be", 4, np.frombuffer(d, dtype=">i4", count=n, offset=off), off))
    out.append(("i8", 1, np.frombuffer(d, dtype="i1"), 0))
    out.append(("u8", 1, np.frombuffer(d, dtype="u1"), 0))
    return out


def find_seq(arr, seq, tol=0, stride=1):
    """Indexes where arr[i + k*stride] ~= seq[k] for all k."""
    seq = np.asarray(seq, dtype=np.int64)
    L = len(seq)
    span = (L - 1) * stride + 1
    if len(arr) < span:
        return []
    a = arr.astype(np.int64)
    cand = np.nonzero(np.abs(a[: len(a) - span + 1] - seq[0]) <= tol)[0]
    for k in range(1, L):
        if not len(cand):
            break
        cand = cand[np.abs(a[cand + k * stride] - seq[k]) <= tol]
    return cand.tolist()


def search_known(d, base, report):
    for name, seq in KNOWN.items():
        if not seq:
            continue
        for vname, size, arr, off in views(d):
            if vname == "u8" and min(seq) < 0:
                continue
            if vname in ("i8", "u8") and max(abs(x) for x in seq) > 255:
                continue
            for stride in (1, 2):
                for i in find_seq(arr, seq, 0, stride):
                    report(base + off + i * size, f"{name} ({vname}{', stride 2' if stride == 2 else ''})")


def search_band_edges(d, base, report, min_len=12):
    seen = set()
    for vname, size, arr, off in views(d):
        if vname not in ("i16le", "u8"):
            continue
        a = arr.astype(np.int64)
        zeros = np.nonzero(a == 0)[0]
        for z in zeros:
            if (vname, z) in seen:
                continue
            k = z
            while k + 1 < len(a) and a[k + 1] > a[k] and a[k + 1] - a[k] <= 64:
                k += 1
            run = a[z:k + 1]
            if len(run) < min_len:
                continue
            steps = np.diff(run)
            # band edges: small steps at the start, growing (roughly) later; not a plain counter
            if np.all(steps == steps[0]):
                continue
            if steps[: len(steps) // 2].mean() > steps[len(steps) // 2:].mean():
                continue
            seen.add((vname, z))
            report(base + off + int(z) * size,
                   f"band edges? {vname} n={len(run)} last={int(run[-1])}: "
                   f"{' '.join(str(int(x)) for x in run[:24])}{' …' if len(run) > 24 else ''}")


def search_sine(d, base, report):
    hits = {}
    for N in TRANSFORM_SIZES:
        k = np.arange(8)
        shapes = {
            f"sin(pi(k+.5)/(2*{N}))": np.sin(np.pi * (k + 0.5) / (2 * N)),
            f"sin(pi*k/(2*{N}))": np.sin(np.pi * k / (2 * N)),
            f"cos(pi(k+.5)/(2*{N}))": np.cos(np.pi * (k + 0.5) / (2 * N)),
            f"sin(2pi(k+1/8)/{N})": np.sin(2 * np.pi * (k + 0.125) / N),
            f"cos(2pi(k+1/8)/{N})": np.cos(2 * np.pi * (k + 0.125) / N),
            f"sin(2pi*k/{N})": np.sin(2 * np.pi * k / N),
            f"cos(2pi*k/{N})": np.cos(2 * np.pi * k / N),
        }
        for shape, s in shapes.items():
            if np.all(np.abs(np.diff(s)) < 1e-3):
                continue   # too flat to tell apart
            for q, scale, tol, types in (("Q15", 32767, 2, ("i16le",)), ("Q31", 2147483647, 1 << 17, ("i32le",))):
                seq = np.round(s * scale).astype(np.int64)
                for vname, size, arr, off in views(d):
                    if vname not in types:
                        continue
                    for stride in (1, 2):
                        for i in find_seq(arr, seq, tol, stride):
                            hits.setdefault(base + off + i * size, []).append(f"{shape} {q}{' stride 2' if stride == 2 else ''}")
            f = np.frombuffer(d[: len(d) // 4 * 4], dtype="<f4") if len(d) >= 4 else None
            for off in range(4):
                fa = np.frombuffer(d, dtype="<f4", count=(len(d) - off) // 4, offset=off)
                fa = np.nan_to_num(fa.astype(np.float64), nan=9.0, posinf=9.0, neginf=9.0)
                for stride in (1, 2):
                    span = 7 * stride + 1
                    if len(fa) < span:
                        continue
                    cand = np.nonzero(np.abs(fa[: len(fa) - span + 1] - s[0]) < 1e-5)[0]
                    for j in range(1, 8):
                        cand = cand[np.abs(fa[cand + j * stride] - s[j]) < 1e-5]
                    for i in cand:
                        hits.setdefault(base + off + int(i) * 4, []).append(f"{shape} float32{' stride 2' if stride == 2 else ''}")
    for addr, what in sorted(hits.items()):
        report(addr, "sine table: " + "; ".join(sorted(set(what))))


def main():
    d = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    found = []

    def report(addr, what):
        found.append((addr, what))

    search_known(d, base, report)
    search_sine(d, base, report)
    search_band_edges(d, base, report)
    for addr, what in sorted(found):
        print(f"{addr:#09x} {what}")
    print(f"{len(found)} hits")


if __name__ == "__main__":
    main()
