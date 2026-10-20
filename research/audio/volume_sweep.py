#!/usr/bin/env python3
"""Map 0x18/0x03 (headphone volume) and 0x18/0x04 on the jack.

ablate.py showed: 17/02 alone plays a -30 dBFS tone at -24 dBFS; with
18/03 07 first, at -19 (the console's value); with 18/04 01 first, at -50.
This sends 17/02 once, then steps 18/03 through values and toggles 18/04,
playing 1 s of 440 Hz after each, in one connection, and measures the jack.

Usage: .venv/bin/python research/audio/volume_sweep.py [1803|1804]
"""
import time

import numpy as np

from ablate import DATA, FRAME
from audio_stream import connect, tone
from console_out import ConsoleCommander
from opus import Encoder
from record import Recorder
from s2 import CHAR_AUDIO_OUT

# First run (volume-20261020-*): 18/03 00 silent, 01-14 all -18.9 dBFS, 1f -23.9;
# 18/04 00 silent, 01 plays, and 18/04 00 stays silent through 18/03 07.
SWEEPS = {
    "1803": [("18/03", v) for v in list(range(0x11)) + [0x14, 0x1F]] +
            [("18/04", 0), ("18/04", 1), ("18/04", 0), ("18/03", 7)],
    "1804": [("18/03", 7)] + [("18/04", v) for v in list(range(0x11)) + [0x14, 0x1F]] + [("18/04", 1)],
}
STEPS = SWEEPS["1803"]


def main():
    import sys
    steps = SWEEPS[sys.argv[1]] if len(sys.argv) > 1 else STEPS
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = f"volume-{stamp}"
    log = open(DATA / f"{base}.log", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, out)
    rec = Recorder(base).start()
    try:
        cmd = ConsoleCommander(att, out)
        cmd.send_plain(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        handle = chars[CHAR_AUDIO_OUT][0]
        enc = Encoder(channels=2, bitrate=96000)
        pcm = tone(1.0, freq=440, level=0.03)
        for n, (what, v) in enumerate(steps, 1):
            c, s = (0x18, 0x03) if what == "18/03" else (0x18, 0x04)
            cmd.send_plain(c, s, bytes([v]))
            out(f">> {n:2d} {what} {v:02x}: tone")
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
            out(f"== {n:2d} {what} {v:02x}: gap")
            if att.closed.is_set():
                break
    finally:
        att.close()
        log.close()
        rec.stop()
    import ablate
    ablate.summarize(base)


if __name__ == "__main__":
    main()
