#!/usr/bin/env python3
"""Measure a test tone in a recording (record.py), step by step, lined
up with a test's log (lines that start with a wall-clock time).

Usage: .venv/bin/python -I research/audio/measure_tone.py REC_NAME LOG [freq]
Prints, for every logged step, the tone's level (dBFS) and how much of
the signal it is, plus a per-second trace.
"""
import json
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

DATA = Path(__file__).resolve().parent / "data"


def tone_level(x, rate, freq):
    """(tone level dBFS, tone share of total power, total dBFS)."""
    if len(x) < rate // 20:
        return None
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * w)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / rate)
    band = (f > freq - 20) & (f < freq + 20)
    total = spec[1:].sum() + 1e-12
    # Parseval: a sine of amplitude A puts N * A^2/4 * sum(w^2) into the
    # positive-frequency bins around it.
    amp = np.sqrt(4 * spec[band].sum() / (len(x) * (w ** 2).sum()))
    rms_total = np.sqrt((x ** 2).mean())
    return (20 * np.log10(max(amp / np.sqrt(2), 1e-9) / 32768), spec[band].sum() / total,
            20 * np.log10(max(rms_total, 1e-9) / 32768))


def main():
    rec, log = sys.argv[1], Path(sys.argv[2])
    freq = float(sys.argv[3]) if len(sys.argv) > 3 else 1000.0
    meta = json.loads((DATA / f"{rec}.json").read_text())
    x, rate = sf.read(DATA / f"{rec}.wav", dtype="int16")
    x = x.astype(float)
    t0 = meta["start"]

    events = []
    for line in log.read_text().splitlines():
        parts = line.split(" ", 1)
        try:
            events.append((float(parts[0]) - t0, parts[1] if len(parts) > 1 else ""))
        except ValueError:
            continue
    print(f"recording {len(x) / rate:.1f} s; log events {len(events)}")
    print("\nper logged step (from this line to the next):")
    for (a, what), (b, _) in zip(events, events[1:] + [(len(x) / rate, "")]):
        if b <= 0 or a >= len(x) / rate or not what.strip().startswith((">>", "second", "==")):
            continue
        seg = x[int(max(a, 0) * rate):int(min(b, len(x) / rate) * rate)]
        r = tone_level(seg, rate, freq)
        if r:
            lvl, share, tot = r
            mark = "TONE" if share > 0.5 and lvl > -70 else ""
            print(f"  {a:7.2f}-{b:6.2f} s  {freq:.0f} Hz {lvl:6.1f} dBFS  share {share:4.2f}  total {tot:6.1f} dBFS  "
                  f"{mark:4}  {what.strip()[:60]}")


if __name__ == "__main__":
    main()
