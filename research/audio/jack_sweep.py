#!/usr/bin/env python3
"""Try Opus frame formats inside the audio-out header, measuring the jack.

[00][0F][32][20 ms mono frame] sometimes plays from the controller's body,
never the jack. The 0x17/0x02 setup says 48 kHz, 0x02 (channels?) and 240
samples per frame (5 ms), which doesn't describe the mic's 20 ms mono
frames, so maybe it describes the output: 5 ms stereo. Each format plays
a 1 kHz tone for 3 s while record.py captures the jack.

Usage: research/audio/wifi-off.sh .venv/bin/python research/audio/jack_sweep.py [--level=0.03]
"""
import sys
import time

import numpy as np

from audio_stream import DATA, connect, init, tone
from opus import Encoder
from s2 import CHAR_AUDIO_IN, CHAR_AUDIO_OUT, CHAR_CMD, CHAR_CMD_RSP, Commander

RATE = 48000

# (name, frame ms, channels, bitrate, state byte, length byte or None for the real length)
FORMATS = [
    ("20 ms mono 50 B, state 0F (the one that played from the body)", 20, 1, 20000, 0x0F, None),
    ("5 ms STEREO 50 B, state 0F", 5, 2, 80000, 0x0F, None),
    ("5 ms STEREO 50 B, state 0D (headphones)", 5, 2, 80000, 0x0D, None),
    ("5 ms STEREO 50 B, state 07", 5, 2, 80000, 0x07, None),
    ("5 ms mono 50 B, state 0F", 5, 1, 80000, 0x0F, None),
    ("10 ms STEREO 50 B, state 0F", 10, 2, 40000, 0x0F, None),
    ("20 ms STEREO 50 B, state 0F", 20, 2, 20000, 0x0F, None),
    ("5 ms STEREO 40 B, state 0F", 5, 2, 64000, 0x0F, None),
    ("5 ms STEREO 60 B, state 0F", 5, 2, 96000, 0x0F, None),
    ("5 ms STEREO 40 B padded to 50, length byte 32", 5, 2, 64000, 0x0F, 0x32),
]


def main():
    level = next((float(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--level=")), 0.03)
    DATA.mkdir(exist_ok=True)
    log = open(DATA / f"jack-{time.strftime('%Y%m%d-%H%M%S')}.log", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, out)
    try:
        cmd = Commander(att, *chars[CHAR_CMD], chars[CHAR_CMD_RSP][0], lambda s: log.write(f"{time.time():.3f} {s}\n"))
        cmd.start()
        init(cmd)
        cmd.send(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        cmd.send(0x18, 0x01)
        cmd.send(0x18, 0x03, b"\x07")
        att.subscribe(chars[CHAR_AUDIO_IN][0], lambda t, d: None)
        time.sleep(1.0)
        handle = chars[CHAR_AUDIO_OUT][0]
        for k, (name, ms, ch, br, state, lenbyte) in enumerate(FORMATS, 1):
            if att.closed.is_set():
                break
            enc = Encoder(channels=ch, bitrate=br)
            n = RATE * ms // 1000
            pcm = tone(3, level=level)
            period = ms / 1000
            first = None
            out(f">> {k}. {name}")
            t0 = time.monotonic()
            for i in range(len(pcm) // n):
                frame = enc.encode(np.repeat(pcm[i * n:(i + 1) * n], ch))
                if lenbyte is not None:
                    frame = frame.ljust(lenbyte, b"\0")
                pkt = bytes([0x00, state, lenbyte if lenbyte is not None else len(frame)]) + frame
                first = first or pkt
                att.write_cmd(handle, pkt)
                ahead = (i + 1) * period - (time.monotonic() - t0)
                if ahead > 0.02:
                    time.sleep(ahead - 0.02)
            out(f"== gap (sent {i + 1} packets in {time.monotonic() - t0:.1f} s; first {first[:6].hex(' ')}…, {len(first)} B)")
            time.sleep(1.0)
        out("== done")
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        log.close()


if __name__ == "__main__":
    main()
