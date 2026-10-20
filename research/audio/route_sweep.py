#!/usr/bin/env python3
"""Look for the command that routes audio-out to the headphone jack.

[00][0F][32][Opus frame] on audio-out sometimes plays from the controller's
body, never from the jack, though the jack works over USB. 0x18/0x03 takes
one byte and echoes it; the console sends 07 (ndeadly's commands.md),
which is also the "headset with mic" jack state. This steps 0x18/0x03
through values, reads 0x18/0x01 after each (its 8 bytes may hold the
state), and plays the tone, while record.py captures the jack through the
USB sound card. measure_tone.py then says which values reached the jack.

Nothing here writes flash, pairing or firmware.

Usage: research/audio/wifi-off.sh .venv/bin/python research/audio/route_sweep.py \
           [--values=0x00-0x1f] [--level=0.03] [--seconds=2]
"""
import sys
import time

from audio_stream import DATA, connect, init, tone
from opus import Encoder
from s2 import CHAR_AUDIO_IN, CHAR_AUDIO_OUT, CHAR_CMD, CHAR_CMD_RSP, Commander

FRAME = 960


def opt(name, default, conv):
    for a in sys.argv[1:]:
        if a.startswith(f"--{name}="):
            return conv(a.split("=", 1)[1])
    return default


def value_range(s):
    a, b = s.split("-")
    return list(range(int(a, 0), int(b, 0) + 1))


def main():
    values = opt("values", list(range(0x20)), value_range)
    level = opt("level", 0.03, float)
    seconds = opt("seconds", 2.0, float)
    address = "3C:A9:AB:69:58:AF"
    DATA.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"route-{stamp}.log", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    class A:
        pass
    args = A()
    args.address = address
    att, chars = connect(args, out)
    try:
        cmd = Commander(att, *chars[CHAR_CMD], chars[CHAR_CMD_RSP][0], lambda s: log.write(f"{time.time():.3f} {s}\n"))
        cmd.start()
        init(cmd)

        def ask(c, s, d=b""):
            r = cmd.send(c, s, d)
            out(f"   {c:02x}/{s:02x} {d.hex(' ')} -> {r[8:].hex(' ') if r else 'no response'}"
                f"{f'  (hdr {r[4]:02x} {r[5]:02x})' if r else ''}")
            return r

        ask(0x18, 0x01)
        # 0x18/0x02 with no payload made the controller drop the link (a
        # supervision timeout two seconds later; it came back by itself).
        # Neither it nor 0x18/0x04 is sent.
        ask(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        att.subscribe(chars[CHAR_AUDIO_IN][0], lambda t, d: None)
        time.sleep(1.0)

        handle = chars[CHAR_AUDIO_OUT][0]
        pcm = tone(seconds, level=level)
        for v in values:
            if att.closed.is_set():
                break
            ask(0x18, 0x03, bytes([v]))
            ask(0x18, 0x01)
            out(f">> 0x18/0x03 {v:02x}: tone")
            enc = Encoder(bitrate=20000)
            t0 = time.monotonic()
            for i in range(len(pcm) // FRAME):
                att.write_cmd(handle, bytes([0x00, 0x0F, 0x32]) + enc.encode(pcm[i * FRAME:(i + 1) * FRAME]))
                ahead = (i + 1) * 0.02 - (time.monotonic() - t0)
                if ahead > 0.02:
                    time.sleep(ahead - 0.02)
            out("== gap")
            time.sleep(0.7)
        ask(0x18, 0x03, b"\x07")   # back to the console's value
        out("== done")
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        log.close()


if __name__ == "__main__":
    main()
