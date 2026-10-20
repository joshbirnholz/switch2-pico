#!/usr/bin/env python3
"""Play audio to the controller's headphone jack the way a Switch 2 does.

From the decrypted console capture (sniff-20261020-191503-ttyACM1):
  - commands go inside the HD rumble + command report (0x0016):
    [00][16 B rumble L][16 B rumble R][command], rumble zeroed;
  - after the connect sequence: 0a/02 03, 09/07 01, 0c/02 27, (memory
    reads), 11/03, 0a/08 …, 11/01, 0c/04 27, 10/01, 01/0c, 18/01, a
    connection update to 5 ms, 18/04 01, 18/03 07, 17/02 80 bb 00 00 02 f0 00
    (the last two through the audio + command report 0x0032 as
    [00][command]);
  - it writes 85 00 to descriptor 0x0010 and subscribes to input report
    0x09 (CCCD 0x000f);
  - audio-out (0x002c) every 10 ms: [00][04][78][120-byte Opus frame],
    then [00][02][78][120 zeros]. Frames are Opus CELT FB 20 ms STEREO,
    CBR 120 bytes (48 kbit/s), TOC 0xFC; silence is fc ff fe + zeros.

The link here isn't encrypted (the console's is); if the jack stays silent
that's the next difference to look at. The controller must be in pairing
mode (Sync) unless it's still paired with this laptop; --pair runs the
0x15 steps as phase1.py does.

Records the USB card from connection and measures the tone at the end.

Usage: research/audio/wifi-off.sh .venv/bin/python research/audio/console_out.py [--pair] [--level=0.03] [--seconds=6]
(With the laptop on Ethernet and Wi-Fi already off, run it directly.)
"""
import sys
import time

import numpy as np

from audio_stream import DATA, connect, tone
from opus import Encoder
from phase1 import pair_steps
from s2 import CHAR_AUDIO_OUT, CHAR_CMD_RSP, build_command, memory_read_payload
import queue

H_RUMBLE_CMD = 0x0016
H_AUDIO_CMD = 0x0032
FRAME = 960


def opt(name, default, conv):
    for a in sys.argv[1:]:
        if a.startswith(f"--{name}="):
            return conv(a.split("=", 1)[1])
    return default


class ConsoleCommander:
    """Sends commands the console's way and waits for the response on 0x001a."""

    def __init__(self, att, out):
        self.att = att
        self.out = out
        self.q = queue.Queue()
        att.subscribe(0x001A, lambda t, d: self.q.put(bytes(d)))

    def _wait(self, cmd, sub, timeout=1.5):
        deadline = time.monotonic() + timeout
        while True:
            try:
                r = self.q.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                return None
            if r[0] == cmd and r[3] == sub:
                return r

    def send(self, cmd, sub, data=b"", via="rumble"):
        c = build_command(cmd, sub, data)
        return self.send_raw(c, via)

    def send_raw(self, c, via="rumble"):
        while not self.q.empty():
            self.q.get_nowait()
        if via == "rumble":
            self.att.write_cmd(H_RUMBLE_CMD, bytes(33) + c)
        else:
            self.att.write_cmd(H_AUDIO_CMD, bytes([0x00]) + c)
        r = self._wait(c[0], c[3])
        self.out(f"   {c[0]:02x}/{c[3]:02x} {c[8:].hex(' ')} ({via}) -> {r[8:].hex(' ') if r else 'no response'}")
        return r


def main():
    level = opt("level", 0.03, float)
    seconds = opt("seconds", 6.0, float)
    DATA.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = DATA / f"console-out-{stamp}.log"
    log = open(log_path, "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, out)
    from record import Recorder
    try:
        rec = Recorder(f"console-out-{stamp}").start()
        out("recording the USB card from now")
    except Exception as e:
        rec = None
        out(f"NOT recording: {e}")
    try:
        cmd = ConsoleCommander(att, out)
        # 0x001b is the CCCD of 0x001a (subscribed above); the console also
        # subscribes to 0x001f and 0x0023.
        att.write(0x001F, b"\x01\x00")
        att.write(0x0023, b"\x01\x00")
        cmd.send(0x07, 0x01)
        cmd.send_raw(memory_read_payload(0x13000, 0x40))
        cmd.send(0x10, 0x01)
        cmd.send(0x16, 0x01)
        if "--pair" in sys.argv:
            class C:   # pair_steps() wants .send(cmd, sub, data)
                send = staticmethod(lambda c, s, d=b"": cmd.send(c, s, d))
            pair_steps(C, att, out)
        cmd.send(0x0A, 0x02, bytes([0x03, 0, 0, 0]))
        cmd.send(0x09, 0x07, bytes([0x01, 0, 0, 0]))
        cmd.send(0x0C, 0x02, bytes([0x27, 0, 0, 0]))
        for addr, ln in ((0x13080, 0x40), (0x130C0, 0x40), (0x1FC040, 0x40), (0x13040, 0x10), (0x13100, 0x18)):
            cmd.send_raw(memory_read_payload(addr, ln))
        cmd.send(0x11, 0x03)
        cmd.send_raw(memory_read_payload(0x13060, 0x20))
        att.write(0x0010, b"\x85\x00")    # "report rate?" descriptor of input report 0x09
        att.write(0x000F, b"\x01\x00")    # subscribe to input report 0x09
        att.on_notify(0x000E, lambda t, d: None)
        cmd.send(0x0A, 0x08, bytes([0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                                    0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
        cmd.send(0x11, 0x01)
        cmd.send(0x0C, 0x04, bytes([0x27, 0, 0, 0]))
        cmd.send(0x10, 0x01)
        cmd.send(0x01, 0x0C)
        cmd.send(0x18, 0x01)
        cmd.send(0x18, 0x04, b"\x01")
        cmd.send(0x18, 0x03, b"\x07", via="audio")
        cmd.send(0x17, 0x02, bytes.fromhex("80bb000002f000"), via="audio")

        handle = chars[CHAR_AUDIO_OUT][0]
        enc = Encoder(channels=2, bitrate=48000)
        filler = bytes([0x00, 0x02, 0x78]) + bytes(120)

        def stream(pcm, label):
            out(f">> {label}")
            t0 = time.monotonic()
            for i in range(len(pcm) // FRAME):
                frame = enc.encode(np.repeat(pcm[i * FRAME:(i + 1) * FRAME], 2)).ljust(120, b"\0")[:120]
                att.write_cmd(handle, bytes([0x00, 0x04, 0x78]) + frame)
                ahead = (2 * i + 1) * 0.010 - (time.monotonic() - t0)
                if ahead > 0:
                    time.sleep(ahead)
                att.write_cmd(handle, filler)
                ahead = (2 * i + 2) * 0.010 - (time.monotonic() - t0)
                if ahead > 0:
                    time.sleep(ahead)
                if att.closed.is_set():
                    return
            out(f"== {label} done ({time.monotonic() - t0:.1f} s)")

        stream(np.zeros(FRAME * 50, np.int16), "1 s of silence frames, as the console starts")
        stream(tone(seconds, level=level), f"1 kHz tone, {seconds:.0f} s at {20 * np.log10(level):.0f} dBFS, stereo 120 B frames")
        stream(np.zeros(FRAME * 25, np.int16), "silence")
        out("== done")
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        log.close()
        if rec:
            secs = rec.stop()
            from measure_tone import report
            print(f"\nrecorded {secs:.1f} s; 1 kHz at the jack per step:")
            report(f"console-out-{stamp}", log_path, 1000.0,
                   out=lambda l: print(l) if (">>" in l or "recording" in l) else None)


if __name__ == "__main__":
    main()
