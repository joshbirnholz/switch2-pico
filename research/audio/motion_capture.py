#!/usr/bin/env python3
"""Capture both motion formats at once, to decode the compact one.

Audio-in (the mic) carries the same compact motion block as input report
0x09 (from byte 65; in 0x09 from byte 15), and the controller stops input
report 0x05 while audio-in is on. 0x05 has plain int16 accel and gyro
(s2_parse_input_report()). This subscribes to 0x05 and 0x09 together and
logs every notification, while asking you to move the controller in known
ways, so the compact block can be matched against the plain values. The
controller sends only one of the two at a time (the later subscription
wins), so this switches between them every half second. A click (0A/02,
as in the connect sequence) marks each new step, so you can follow the
schedule without watching the screen.

Read-only apart from the dongle's usual init commands and CCCD writes. No
audio, no pairing: put the controller in pairing mode (hold sync), or it
won't accept a host it isn't paired with.

With --mic, audio-in takes 0x09's place: the dongle's audio commands
(18/04 01, 18/03 07, 17/02) and a silent audio-out stream, which the mic
needs; its notifications carry the motion block from byte 65.

Unpaired, the controller turned itself off when the audio stream started;
--pair runs the 0x15 pairing steps first (the controller then needs pairing
with the dongle again).

Usage: research/audio/wifi-off.sh .venv/bin/python research/audio/motion_capture.py [--mic] [--pair] [ADDRESS]
Writes data/motion-STAMP.raw: "t handle hex" per notification.
"""
import sys
import threading
import time

import numpy as np

from audio_stream import DATA, connect, init
from opus import Encoder
from phase1 import pair_steps
from s2 import (CHAR_AUDIO_IN, CHAR_AUDIO_OUT, CHAR_CMD, CHAR_CMD_RSP, CHAR_INPUT, CHAR_PRO_INPUT9, Commander,
                memory_read_payload)

PROMPTS = [
    (0, "lay the controller flat on the table, face up, and keep still"),
    (6, "pick it up and hold it still in front of you"),
    (10, "ROLL it slowly left and right, a few times (tilting the grips up and down)"),
    (18, "hold still"),
    (21, "PITCH it slowly: tilt the top towards you and away, a few times"),
    (29, "hold still"),
    (32, "YAW it slowly: turn it left and right like a steering wheel lying flat, a few times"),
    (40, "hold still"),
    (43, "SHAKE it quickly side to side"),
    (48, "put it back flat on the table, face up, and keep still"),
    (54, "turn it face DOWN on the table and keep still"),
    (60, "done"),
]


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    mic = "--mic" in sys.argv
    address = argv[0] if argv else "3C:A9:AB:69:58:AF"
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"motion-{stamp}.log", "w")
    raw = open(DATA / f"motion-{stamp}.raw", "w")
    lock = threading.Lock()

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    class A:
        pass
    args = A()
    args.address = address
    att, chars = connect(args, out)
    cmd = Commander(att, chars[CHAR_CMD][0], chars[CHAR_CMD][1], chars[CHAR_CMD_RSP][0], out)
    cmd.start()
    if "--pair" in sys.argv:
        # init()'s sequence with the 0x15 pairing steps where the dongle has them
        cmd.send(0x07, 0x01)
        cmd.send_raw(memory_read_payload(0x13000, 0x40))
        cmd.send(0x16, 0x01)
        out(f"   pairing {'complete' if pair_steps(cmd, att, out) else 'FAILED'}")
    init(cmd)

    counts = {}

    def logger(handle):
        def cb(t, d):
            with lock:
                raw.write(f"{t:.4f} {handle:04x} {bytes(d).hex()}\n")
                counts[handle] = counts.get(handle, 0) + 1
        return cb

    h5, h9 = chars[CHAR_INPUT][0], chars[CHAR_AUDIO_IN if mic else CHAR_PRO_INPUT9][0]
    stop = threading.Event()
    hold = threading.Event()     # pauses the silent stream around CCCD writes
    if mic:
        cmd.send(0x18, 0x04, b"\x01")
        cmd.send(0x18, 0x03, b"\x07")
        cmd.send(0x17, 0x02, bytes.fromhex("80bb000002f000"))

        def silence_out():
            frame = Encoder(channels=2, bitrate=96000).encode(np.zeros(1920, np.int16)).ljust(240, b"\0")
            handle = chars[CHAR_AUDIO_OUT][0]
            t0, i = time.monotonic(), 0
            while not stop.is_set() and not att.closed.is_set():
                if hold.is_set():
                    time.sleep(0.005)
                    t0, i = time.monotonic(), 0
                    continue
                att.write_cmd(handle, bytes([0x00, 0x04, 0x78]) + frame[:120])
                time.sleep(max(0.0, i * 0.020 + 0.005 - (time.monotonic() - t0)))
                att.write_cmd(handle, bytes([0x00, 0x02, 0x78]) + frame[120:])
                i += 1
                time.sleep(max(0.0, i * 0.020 - (time.monotonic() - t0)))
        threading.Thread(target=silence_out, daemon=True).start()
    att.on_notify(h5, logger(h5))
    att.on_notify(h9, logger(h9))
    out(f"input 0x05 is {h5:#06x}, {'audio-in' if mic else '0x09'} is {h9:#06x}; starting in 5 s")
    time.sleep(5)

    start = time.monotonic()
    try:
        run(cmd, att, out, lock, raw, counts, h5, h9, hold, start)
    finally:
        stop.set()
        time.sleep(0.1)
        att.close()
        with lock:
            raw.close()
        out(f"saved {raw.name}")


def run(cmd, att, out, lock, raw, counts, h5, h9, hold, start):
    current = None
    for at, text in PROMPTS:
        cmd.send(0x0A, 0x02, bytes([0x03, 0, 0, 0]))
        with lock:
            out(f"[{at:2d} s] {text}   (0x05: {counts.get(h5, 0)}, 0x09: {counts.get(h9, 0)})")
            raw.write(f"{time.monotonic():.4f} mark {at} {text.split()[0]}\n")
        nxt = next((a for a, _ in PROMPTS if a > at), at)
        while time.monotonic() - start < nxt:
            want = h5 if int((time.monotonic() - start) * 2) % 2 == 0 else h9
            if want != current:
                hold.set()
                time.sleep(0.05)
                try:
                    if current is not None:
                        att.write(current + 1, b"\x00\x00", timeout=5)
                    att.write(want + 1, b"\x01\x00", timeout=5)
                except TimeoutError as e:
                    out(f"{e}; link {'closed: ' + str(att.close_reason) if att.closed.is_set() else 'still up'}")
                    raise
                finally:
                    hold.clear()
                with lock:
                    raw.write(f"{time.monotonic():.4f} sub {want:04x}\n")
                current = want
            time.sleep(0.02)


if __name__ == "__main__":
    main()
