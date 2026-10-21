#!/usr/bin/env python3
"""Which of the console's commands does the mic need?

mic_test.py: with only 18/04 01, 18/03 02 and 17/02 the mic delivers flat
noise at about -45 dBFS whatever you do; after the console's whole
sequence (--full) it is clean: a -71 dBFS floor and the voice on top. So
the quiet floor tells the two apart without anyone talking. Each variant
runs on a new connection: one group of the console's commands, then
18/04 01, 18/03 02, 17/02 and the audio-in subscription, then 3 s of
quiet. Least first: a later variant may inherit what an earlier one set.

Usage: .venv/bin/python research/audio/mic_ablate.py [--reset-pair] [VARIANT ...]

First run (no reset): every variant clean, even "none", because the clean
state carried over from an earlier --full run (it survives disconnects and
sleep). --reset-pair runs the 0x15 pairing steps first in each variant.
"""
import sys
import threading
import time

import numpy as np

from audio_stream import DATA, connect
from console_out import ConsoleCommander
from opus import Decoder, Encoder
from s2 import CHAR_AUDIO_IN, CHAR_AUDIO_OUT

VIB_DATA = bytes([0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                  0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
GROUPS = {
    "none": [],
    "features": [("cmd", 0x0C, 0x02, b"\x27\x00\x00\x00"), ("cmd", 0x0C, 0x04, b"\x27\x00\x00\x00")],
    "11xx": [("cmd", 0x11, 0x03, b""), ("cmd", 0x11, 0x01, b"")],
    "010c": [("cmd", 0x01, 0x0C, b"")],
    "input09": [("write", 0x0010, b"\x85\x00"), ("write", 0x000F, b"\x01\x00")],
    "misc": [("cmd", 0x0A, 0x02, b"\x03\x00\x00\x00"), ("cmd", 0x09, 0x07, b"\x01\x00\x00\x00"),
             ("cmd", 0x0A, 0x08, VIB_DATA), ("cmd", 0x18, 0x01, b"")],
}
GROUPS["full"] = (GROUPS["misc"][:2] + GROUPS["features"][:1] + [GROUPS["11xx"][0]] + GROUPS["input09"] +
                  [GROUPS["misc"][2], GROUPS["11xx"][1], GROUPS["features"][1], GROUPS["010c"][0], GROUPS["misc"][3]])


def run(name, steps, out):
    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, out)
    frames = []
    half = {"first": None}

    def on_audio_in(t, d):
        d = bytes(d)
        if d[14] != 50:
            return
        if d[13] & 0x08:
            half["first"] = d[15:65]
        elif half["first"] is not None:
            frames.append((t, half["first"] + d[15:65]))
            half["first"] = None

    stop = threading.Event()

    def silence_out():
        frame = Encoder(channels=2, bitrate=96000).encode(np.zeros(1920, np.int16)).ljust(240, b"\0")
        handle = chars[CHAR_AUDIO_OUT][0]
        t0 = time.monotonic()
        i = 0
        while not stop.is_set() and not att.closed.is_set():
            att.write_cmd(handle, bytes([0x00, 0x04, 0x78]) + frame[:120])
            time.sleep(max(0.0, i * 0.020 + 0.005 - (time.monotonic() - t0)))
            att.write_cmd(handle, bytes([0x00, 0x02, 0x78]) + frame[120:])
            i += 1
            time.sleep(max(0.0, i * 0.020 - (time.monotonic() - t0)))

    try:
        cmd = ConsoleCommander(att, out)
        att.write(0x001F, b"\x01\x00")
        att.write(0x0023, b"\x01\x00")
        cmd.send_plain(0x07, 0x01)
        cmd.send_plain(0x16, 0x01)
        if "--reset-pair" in sys.argv:
            # The working mic state survives disconnects and sleep; the 0x15
            # pairing steps seem to reset it (noise right after --pair).
            from phase1 import pair_steps

            class C:
                send = staticmethod(lambda c, s, d=b"": cmd.send_plain(c, s, d))
            pair_steps(C, att, out)
        subscribed09 = False
        for st in steps:
            if st[0] == "cmd":
                cmd.send_plain(st[1], st[2], st[3])
            else:
                att.write(st[1], st[2])
                if st[1] == 0x000F:
                    att.on_notify(0x000E, lambda t, d: None)
                    subscribed09 = True
        cmd.send_plain(0x18, 0x04, b"\x01")
        cmd.send_plain(0x18, 0x03, b"\x02")
        cmd.send_plain(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        if subscribed09:
            att.write(0x000F, b"\x00\x00")
        att.subscribe(chars[CHAR_AUDIO_IN][0], on_audio_in)
        threading.Thread(target=silence_out, daemon=True).start()
        time.sleep(1.0)
        t_start = time.monotonic()
        out(f">> {name}: measuring 3 s of quiet")
        time.sleep(3.0)
    finally:
        stop.set()
        time.sleep(0.1)
        att.close()
    dec = Decoder()
    levels = []
    for t, f in frames:
        try:
            p = dec.decode(f).astype(float)
        except ValueError:
            continue
        if t >= t_start:
            levels.append(20 * np.log10(max(np.sqrt((p ** 2).mean()), 1e-9) / 32768))
    time.sleep(2.0)
    if not levels:
        return None, 0
    return float(np.median(levels)), len(levels)


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--")] or list(GROUPS)
    # names may repeat (none full none)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"mic-ablate-{stamp}.log", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    results = []
    for name in names:
        out(f"\n== variant {name}")
        try:
            floor, n = run(name, GROUPS[name], out)
        except Exception as e:
            out(f"   failed: {e!r}")
            floor, n = None, 0
        results.append((name, floor, n))
    log.close()
    print("\nquiet mic floor per variant (clean about -71 dBFS, broken about -45):")
    for name, floor, n in results:
        verdict = "?" if floor is None else ("CLEAN" if floor < -60 else "noise")
        print(f"  {name:10} {('%.1f dBFS' % floor) if floor is not None else 'no frames':>12}  ({n} frames)  {verdict}")


if __name__ == "__main__":
    main()
