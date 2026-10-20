#!/usr/bin/env python3
"""Look for what switches the headset's mic and output on.

1. Asks the controller about each feature bit with 0x0C/0x01 (get feature
   info, read only; ndeadly's commands.md).
2. Then goes through configurations one after another. In each, a 1 kHz
   tone plays as mono Opus (the format that thumps) while you tap the mic,
   and the mic level is measured. Tell me which numbers you HEARD the tone
   in the headphones; the script tells which ones the mic woke up in.

Configurations change only known commands' parameters: the feature mask
(0x0C/0x02 + 0x0C/0x04, the bits the controller reported on), 0x17/0x02's
mode byte, and the order of 0x18/0x03 07 and 0x17/0x02. State carries over
from one configuration to the next.

Run it yourself, earbuds-with-mic in the controller, KDE Connect closed:
  research/audio/wifi-off.sh .venv/bin/python research/audio/probe_audio.py
"""
import sys
import time

import numpy as np

from audio_stream import DATA, MicCapture, connect, init, tone
from opus import Decoder, Encoder
from s2 import CHAR_AUDIO_IN, CHAR_AUDIO_OUT, CHAR_CMD, CHAR_CMD_RSP, Commander

BASE_MASK = 0x2F


def stream_setup(mode=0x02):
    return bytes.fromhex(f"80bb0000{mode:02x}f000")


class Levels:
    """Mic level per labelled window, from the decoded frames."""

    def __init__(self):
        self.dec = Decoder()
        self.label = None
        self.rms = {}

    def __call__(self, t, d):
        d = bytes(d)
        label = self.label   # the main thread changes it between windows
        if len(d) < 65 or not d[13] & 8 or label is None:
            return
        try:
            p = self.dec.decode(d[15:15 + d[14]]).astype(float)
        except ValueError:
            return
        self.rms.setdefault(label, []).append(np.sqrt((p ** 2).mean()))


def main():
    address = sys.argv[1] if len(sys.argv) > 1 else "3C:A9:AB:69:58:AF"
    DATA.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"probe-{stamp}.log", "w")
    raw = open(DATA / f"probe-{stamp}.raw", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(line + "\n")
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

        out("\n-- feature info, one bit at a time (0x0C/0x01, read only)")
        known = {}
        for bit in range(8):
            f = 1 << bit
            rsp = cmd.send(0x0C, 0x01, bytes([f, 0, 0, 0]))
            known[f] = rsp
            out(f"   {f:#04x}: {rsp[8:].hex(' ') if rsp else 'no response'}")
        rsp = cmd.send(0x0C, 0x01, bytes([0xFF, 0, 0, 0]))
        out(f"   0xff: {rsp[8:].hex(' ') if rsp else 'no response'}")

        # Bits outside the Pro mask whose info isn't all zero (the response is
        # 4 zero bytes, then the feature info).
        extra = [f for f, r in known.items() if not f & BASE_MASK and r and any(r[12:])]
        out(f"   bits outside {BASE_MASK:#04x} with non-zero info: {[hex(f) for f in extra]}")

        rsp = cmd.send(0x17, 0x02, stream_setup())
        cmd.send(0x18, 0x01)
        cmd.send(0x18, 0x03, b"\x07")

        levels = Levels()
        mic = MicCapture(raw, lambda s: None)

        def both(t, d):
            mic(t, d)
            levels(t, d)

        att.subscribe(chars[CHAR_AUDIO_IN][0], both)
        time.sleep(1.5)

        configs = [("baseline: as in the last run", [])]
        for f in extra:
            m = BASE_MASK | f
            configs.append((f"feature mask {m:#04x} (adds {f:#04x})",
                            [(0x0C, 0x02, bytes([m, 0, 0, 0])), (0x0C, 0x04, bytes([m, 0, 0, 0]))]))
        if extra:
            configs.append((f"back to feature mask {BASE_MASK:#04x}",
                            [(0x0C, 0x05, bytes([sum(extra), 0, 0, 0])),
                             (0x0C, 0x02, bytes([BASE_MASK, 0, 0, 0])), (0x0C, 0x04, bytes([BASE_MASK, 0, 0, 0]))]))
        configs += [
            ("0x18/0x03 07 sent again, then 0x17/0x02", [(0x18, 0x03, b"\x07"), (0x17, 0x02, stream_setup())]),
            ("0x17/0x02 mode 0x01", [(0x17, 0x02, stream_setup(0x01))]),
            ("0x17/0x02 mode 0x03", [(0x17, 0x02, stream_setup(0x03))]),
            ("0x17/0x02 mode 0x02 again", [(0x17, 0x02, stream_setup(0x02))]),
        ]

        out("\n== For each number: TAP the mic gently, over and over, and listen for a tone in the earbuds.")
        out("   (Hold them near your ear. Say afterwards which numbers had a tone, and thump or not.)")
        levels.label = "still (before)"
        out(">> keep still for 3 s")
        time.sleep(3)
        enc = Encoder(bitrate=20000)
        handle = chars[CHAR_AUDIO_OUT][0]
        for k, (name, cmds) in enumerate(configs, 1):
            if att.closed.is_set():
                break
            levels.label = None
            for c, s, d in cmds:
                rsp = cmd.send(c, s, d)
                out(f"   {c:02x}/{s:02x} {d.hex(' ')} -> {rsp[8:].hex(' ') if rsp else 'no response'}"
                    f"{'' if not rsp else f' (status {rsp[5]:#04x})'}")
            levels.label = f"{k}. {name}"
            out(f">> {k}. {name}: TAP the mic and listen")
            pcm = tone(6)
            t0 = time.monotonic()
            for i in range(len(pcm) // 960):
                att.write_cmd(handle, enc.encode(pcm[i * 960:(i + 1) * 960]))
                ahead = (i + 1) * 0.02 - (time.monotonic() - t0)
                if ahead > 0.02:
                    time.sleep(ahead - 0.02)
            levels.label = None
            time.sleep(1.5)

        out("\n-- mic level per window (RMS of decoded frames; the noise floor is ~6)")
        for label, v in levels.rms.items():
            v = np.array(v)
            out(f"   {label:50} rms {v.mean():7.1f}  max {v.max():8.1f}  {'MIC LIVE?' if v.max() > 60 else ''}")
        out(f"\nstream: {mic.summary()}")
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        raw.close()
        log.close()


if __name__ == "__main__":
    main()
