#!/usr/bin/env python3
"""Find the minimal command set for headphone output.

console_out.py plays a clean tone from the jack after the console's whole
command sequence. This runs variants with less, each on a new connection
(the controller's audio state lasts as long as the link), playing 2 s of
440 Hz in the working format (240 B frames in two halves) after each, and
measures the jack through the USB sound card. The controller must be
paired with this laptop (console_out.py --pair, phase1.py --pair).

Usage: .venv/bin/python research/audio/ablate.py [VARIANT ...]
       (no names: all of them, in order)

The first run (full first, then less) played the tone in every variant,
even "bare"; but a variant may inherit state from an earlier one, so run
the least first on a controller that has been asleep: nothing 1702-plain
1702-rumble full.
"""
import sys
import time

import numpy as np

from audio_stream import DATA, connect, tone
from console_out import ConsoleCommander
from opus import Encoder
from record import Recorder
from s2 import CHAR_AUDIO_OUT, memory_read_payload

FRAME = 960
SETUP = bytes.fromhex("80bb000002f000")
VIB_DATA = bytes([0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                  0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00])

# Steps: ("cmd", cmd, sub, data, via) via "rumble" (0x0016), "audio" (0x0032)
# or "plain" (the command characteristic 0x0014); ("write", handle, value);
# ("read", address, length).
BASE = [("cmd", 0x07, 0x01, b"", "rumble"), ("read", 0x13000, 0x40),
        ("cmd", 0x10, 0x01, b"", "rumble"), ("cmd", 0x16, 0x01, b"", "rumble")]
CONSOLE_MID = [("cmd", 0x0A, 0x02, b"\x03\x00\x00\x00", "rumble"), ("cmd", 0x09, 0x07, b"\x01\x00\x00\x00", "rumble"),
               ("cmd", 0x0C, 0x02, b"\x27\x00\x00\x00", "rumble"), ("cmd", 0x11, 0x03, b"", "rumble"),
               ("write", 0x0010, b"\x85\x00"), ("write", 0x000F, b"\x01\x00"),
               ("cmd", 0x0A, 0x08, VIB_DATA, "rumble"), ("cmd", 0x11, 0x01, b"", "rumble"),
               ("cmd", 0x0C, 0x04, b"\x27\x00\x00\x00", "rumble"), ("cmd", 0x01, 0x0C, b"", "rumble"),
               ("cmd", 0x18, 0x01, b"", "rumble")]
R1804 = ("cmd", 0x18, 0x04, b"\x01", "rumble")
A1803 = ("cmd", 0x18, 0x03, b"\x07", "audio")
A1702 = ("cmd", 0x17, 0x02, SETUP, "audio")
R1803 = ("cmd", 0x18, 0x03, b"\x07", "rumble")
R1702 = ("cmd", 0x17, 0x02, SETUP, "rumble")
P1804 = ("cmd", 0x18, 0x04, b"\x01", "plain")
P1803 = ("cmd", 0x18, 0x03, b"\x07", "plain")
P1702 = ("cmd", 0x17, 0x02, SETUP, "plain")

VARIANTS = {
    "full": BASE + CONSOLE_MID + [R1804, A1803, A1702],       # what works
    "no-mid": BASE + [R1804, A1803, A1702],                     # without 11/xx, 01/0c, features, 85 00 …
    "no-1804": BASE + [A1803, A1702],
    "no-1803": BASE + [R1804, A1702],
    "only-1702": BASE + [A1702],
    "rumble-report": BASE + [R1804, R1803, R1702],              # 0x0032 not needed?
    "plain-char": BASE + [P1804, P1803, P1702],                 # the command characteristic
    "bare": [R1804, A1803, A1702],                              # not even the connect commands
    # Run least first (later variants may inherit what earlier ones set up):
    "nothing": [],                                              # just the audio stream
    "1702-plain": [P1702],
    "1702-rumble": [R1702],
}


def run_variant(name, steps, out, level):
    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, out)
    try:
        cmd = ConsoleCommander(att, out)
        att.write(0x001F, b"\x01\x00")
        att.write(0x0023, b"\x01\x00")
        for st in steps:
            if st[0] == "cmd":
                _, c, s, d, via = st
                if via == "plain":
                    cmd.send_plain(c, s, d)
                else:
                    cmd.send(c, s, d, via=via)
            elif st[0] == "write":
                att.write(st[1], st[2])
                if st[1] == 0x000F:
                    att.on_notify(0x000E, lambda t, d: None)
            elif st[0] == "read":
                cmd.send_raw(memory_read_payload(st[1], st[2]))
        handle = chars[CHAR_AUDIO_OUT][0]
        enc = Encoder(channels=2, bitrate=96000)
        pcm = tone(2.0, freq=440, level=level)
        time.sleep(0.3)
        out(f">> {name}: tone")
        t0 = time.monotonic()
        for i in range(len(pcm) // FRAME):
            frame = enc.encode(np.repeat(pcm[i * FRAME:(i + 1) * FRAME], 2)).ljust(240, b"\0")[:240]
            att.write_cmd(handle, bytes([0x00, 0x04, 0x78]) + frame[:120])
            ahead = i * 0.020 + 0.005 - (time.monotonic() - t0)
            if ahead > 0:
                time.sleep(ahead)
            att.write_cmd(handle, bytes([0x00, 0x02, 0x78]) + frame[120:])
            ahead = (i + 1) * 0.020 - (time.monotonic() - t0)
            if ahead > 0:
                time.sleep(ahead)
        out(f"== {name}: gap")
    finally:
        att.close()
    time.sleep(2.0)   # let the link go down before the next variant


def main():
    names = [a for a in sys.argv[1:] if not a.startswith("--")] or list(VARIANTS)
    level = 0.03
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = DATA / f"ablate-{stamp}.log"
    log = open(log_path, "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    rec = Recorder(f"ablate-{stamp}").start()
    try:
        for name in names:
            out(f"\n== variant {name}: {len(VARIANTS[name])} steps")
            try:
                run_variant(name, VARIANTS[name], out, level)
            except Exception as e:
                out(f"   {name} failed: {e!r}")
    finally:
        log.close()
        rec.stop()
    summarize(f"ablate-{stamp}")


def summarize(base):
    """440 Hz at the jack from each ">> NAME: tone" line to its "== NAME: gap"
    (other log lines, e.g. unhandled notifications, fall in between)."""
    import json
    import soundfile as sf
    t0 = json.loads((DATA / f"{base}.json").read_text())["start"]
    x, sr = sf.read(DATA / f"{base}.wav", dtype="int16")
    x = x.astype(float)
    ev = []
    for line in open(DATA / f"{base}.log"):
        p = line.split(" ", 1)
        try:
            ev.append((float(p[0]) - t0, p[1].strip()))
        except (ValueError, IndexError):
            pass
    starts = {w[3:-6]: t for t, w in ev if w.startswith(">> ") and w.endswith(": tone")}
    ends = {w[3:-5]: t for t, w in ev if w.startswith("== ") and w.endswith(": gap")}
    print("\n440 Hz at the jack per variant:")
    for name, a in starts.items():
        b = ends.get(name, a + 2)
        seg = x[int((a + 0.3) * sr):int((b - 0.1) * sr)]
        w = np.hanning(len(seg))
        sp = np.abs(np.fft.rfft(seg * w)) ** 2
        f = np.fft.rfftfreq(len(seg), 1 / sr)
        band = sp[(f > 430) & (f < 450)].sum()
        share = band / (sp[1:].sum() + 1e-12)
        lvl = 20 * np.log10(max(np.sqrt(4 * band / (len(seg) * (w ** 2).sum())) / np.sqrt(2), 1e-9) / 32768)
        print(f"  {name:15} 440 Hz {lvl:6.1f} dBFS, share {share:.2f}  {'PLAYS' if lvl > -40 and share > 0.8 else 'silent'}")


if __name__ == "__main__":
    main()
