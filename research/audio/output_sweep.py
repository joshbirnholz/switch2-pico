#!/usr/bin/env python3
"""Try packet layouts on the audio-out characteristic (…b06, handle 0x2c).

The audio-in report carries a 0x34-byte audio block at offset 0x0D:
[state][0x32][50 bytes], where state alternates 0x07/0x0F with a headset
(bit 3 = a frame is in this report), 0x05/0x0D with headphones
(ndeadly's bluetooth_interface.md). The frames are Opus CELT FB 20 ms
mono, 50 bytes CBR. This writes the same kind of frame (a 1 kHz tone) in
layouts built around that block, 3 s each, so you can say which one
plays. Bare frames (thump, no sound) and [len][frame] (nothing) were
tried before.

Sends the same setup as audio_stream.py (no new commands) and writes only
to audio-out. Run it yourself, headphones in the controller's jack, held
near your ear:
  research/audio/wifi-off.sh .venv/bin/python research/audio/output_sweep.py [--states | --timing]

First sweep: layout 6, [00][0F][32][frame], played the tone from the
controller's body (actuators?), not the jack; the others did nothing.
--states keeps that packet and tries every state byte 0x00-0x1F, 0x2F,
0x4F and 0x8F, 1.5 s each. The tone is at -40 dBFS unless --level=X
(linear, 0.1 = -20 dBFS) says otherwise.

Both sweeps showed the same thing: per connection, the first stream with
the leading 00 is silent, the second plays, none after that do. --timing
tests whether that's warm-up then a stop (one 20 s stream with a seconds
counter) or the gaps (2 s bursts, each after resending 0x17/0x02).
"""
import math
import sys
import time

from audio_stream import DATA, MicCapture, connect, init, tone
from opus import Encoder
from s2 import CHAR_AUDIO_IN, CHAR_AUDIO_OUT, CHAR_CMD, CHAR_CMD_RSP, Commander

FRAME = 960   # 20 ms at 48 kHz


def mirror_report(seq, frame):
    """A 112-byte report shaped like audio-in: counter, 0x20, buttons and
    sticks zero, state 0x0F, length, frame, then zeros."""
    r = bytearray(112)
    r[0] = seq & 0xFF
    r[1] = 0x20
    r[0x0D] = 0x0F
    r[0x0E] = len(frame)
    r[0x0F:0x0F + len(frame)] = frame
    return bytes(r)


LAYOUTS = [
    ("[08][32][frame]", lambda seq, f: bytes([0x08, len(f)]) + f),
    ("[0F][32][frame]", lambda seq, f: bytes([0x0F, len(f)]) + f),
    ("[0D][32][frame]", lambda seq, f: bytes([0x0D, len(f)]) + f),
    ("[07][32][frame]", lambda seq, f: bytes([0x07, len(f)]) + f),
    ("[00][08][32][frame]", lambda seq, f: bytes([0x00, 0x08, len(f)]) + f),
    ("[00][0F][32][frame]", lambda seq, f: bytes([0x00, 0x0F, len(f)]) + f),
    ("[00][32][frame]", lambda seq, f: bytes([0x00, len(f)]) + f),
    ("[00][frame]", lambda seq, f: bytes([0x00]) + f),
    ("[seq][08][32][frame]", lambda seq, f: bytes([seq & 0xFF, 0x08, len(f)]) + f),
    ("[0x50|seq][08][32][frame]", lambda seq, f: bytes([0x50 | (seq & 0x0F), 0x08, len(f)]) + f),
    ("112-byte copy of an audio-in report", mirror_report),
]


def state_layouts():
    """Layout 6 of the first sweep, [00][0F][32][frame], played the tone from
    the controller's body (not the jack). Same packet, every state byte."""
    states = list(range(0x20)) + [0x2F, 0x4F, 0x8F]
    return [(f"[00][{st:02X}][32][frame]", (lambda st: lambda seq, f: bytes([0x00, st, len(f)]) + f)(st))
            for st in states]


def timing_test(att, cmd, handle, level, out):
    """In each connection so far, the first stream with the leading 00 was
    silent, the second played and none after that did, whatever the state
    byte. A: one continuous stream with a seconds counter. B: short bursts,
    each after resending the 0x17/0x02 stream setup."""
    build = lambda seq, f: bytes([0x00, 0x0F, len(f)]) + f

    def stream(seconds, tick):
        enc = Encoder(bitrate=20000)
        pcm = tone(seconds, level=level)
        t0 = time.monotonic()
        for i in range(len(pcm) // FRAME):
            if tick and i % 50 == 0:
                out(f"   second {i // 50 + 1}")
            att.write_cmd(handle, build(i, enc.encode(pcm[i * FRAME:(i + 1) * FRAME])))
            ahead = (i + 1) * 0.02 - (time.monotonic() - t0)
            if ahead > 0.02:
                time.sleep(ahead - 0.02)
            if att.closed.is_set():
                return

    out("\n== A: one continuous 20 s stream. Note the seconds where you hear the tone.")
    stream(20, True)
    time.sleep(2)
    out("\n== B: four 2 s bursts, the stream setup resent before each. Note which play.")
    for k in range(1, 5):
        rsp = cmd.send(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        out(f">> burst {k} (0x17/0x02 -> {'ack' if rsp else 'no response'})")
        stream(2, False)
        time.sleep(1.5)


def main():
    args_in = [a for a in sys.argv[1:] if not a.startswith("--")]
    address = args_in[0] if args_in else "3C:A9:AB:69:58:AF"
    layouts = state_layouts() if "--states" in sys.argv else LAYOUTS
    seconds = 1.5 if "--states" in sys.argv else 3
    # Quiet by default: Josh works in an office, and the body of the
    # controller plays audio out loud. --level=0.1 is the old -20 dBFS.
    level = next((float(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--level=")), 0.01)
    DATA.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"sweep-{stamp}.log", "w")
    raw = open(DATA / f"sweep-{stamp}.raw", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")   # wall clock, for measure_tone.py
        log.flush()

    class A:
        pass
    args = A()
    args.address = address
    att, chars = connect(args, out)
    try:
        cmd = Commander(att, *chars[CHAR_CMD], chars[CHAR_CMD_RSP][0], lambda s: log.write(s + "\n"))
        cmd.start()
        init(cmd)
        cmd.send(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        cmd.send(0x18, 0x01)
        cmd.send(0x18, 0x03, b"\x07")
        mic = MicCapture(raw, out)
        att.subscribe(chars[CHAR_AUDIO_IN][0], mic)
        time.sleep(1.5)

        handle = chars[CHAR_AUDIO_OUT][0]
        if "--timing" in sys.argv:
            out(f"tone: 1 kHz at {20 * math.log10(level):.0f} dBFS")
            timing_test(att, cmd, handle, level, out)
            out(f"\nstream: {mic.summary()}")
            return
        pcm = tone(seconds, level=level)
        out(f"tone: 1 kHz at {20 * math.log10(level):.0f} dBFS, {seconds} s per step")
        out("\n== Listen for a 1 kHz tone in the headphones. Note each number with a tone, a thump, or anything else.")
        for k, (name, build) in enumerate(layouts, 1):
            if att.closed.is_set():
                break
            enc = Encoder(bitrate=20000)
            out(f">> {k:2}. {name}")
            t0 = time.monotonic()
            for i in range(len(pcm) // FRAME):
                frame = enc.encode(pcm[i * FRAME:(i + 1) * FRAME])
                att.write_cmd(handle, build(i, frame))
                ahead = (i + 1) * 0.02 - (time.monotonic() - t0)
                if ahead > 0.02:
                    time.sleep(ahead - 0.02)
            time.sleep(0.8 if "--states" in sys.argv else 1.5)
        out(f"\nstream: {mic.summary()}")
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        raw.close()
        log.close()


if __name__ == "__main__":
    main()
