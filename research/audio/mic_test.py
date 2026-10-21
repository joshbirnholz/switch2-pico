#!/usr/bin/env python3
"""Does the controller's mic need Low Latency mode (18/03 02)?

The console's mic test recorded the earbuds' mic in Low Latency mode and
got nothing in Stable. Our Stable-mode tests got only the noise floor too.
This sends 18/04 01, 18/03 MODE and 17/02 on the command characteristic,
subscribes to audio-in (CCCD 0x002f) as the console does, keeps a silent
audio-out stream going (as the console's playback would), and asks you to
talk in three windows: Low Latency (02), Stable (07), Low Latency (02).
Decodes the mic frames and reports the level per window.

The controller must be paired with this laptop (--pair runs the 0x15
steps; it then needs pairing with the console again afterwards).

Usage: .venv/bin/python research/audio/mic_test.py [--pair] [--seconds=6]
"""
import sys
import threading
import time

import numpy as np
import soundfile as sf

from audio_stream import DATA, connect
from console_out import ConsoleCommander
from opus import Decoder, Encoder
from phase1 import pair_steps
from s2 import CHAR_AUDIO_IN, CHAR_AUDIO_OUT, memory_read_payload

PHASES = [("Low Latency", 0x02), ("Stable", 0x07), ("Low Latency again", 0x02)]


def main():
    seconds = next((float(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--seconds=")), 6.0)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"mic-{stamp}.log", "w")
    raw = open(DATA / f"mic-{stamp}.raw", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(f"{time.time():.3f} {line}\n")
        log.flush()

    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, out)
    frames = []            # (time, phase, frame)
    phase = {"name": None}
    states = {}

    half = {"first": None}

    def on_audio_in(t, d):
        # A mic frame is 100 B in two notifications 5 ms apart: byte 13 with
        # bit 3 set (0x2f, 0x1f) carries the first 50 B (starting with the
        # TOC), the next one with 50 B and bit 3 clear (0x17) the rest; then
        # two empty ones (0x27). Seen in the console's mic test.
        d = bytes(d)
        raw.write(f"{t:.4f} {d.hex()}\n")
        states[d[13]] = states.get(d[13], 0) + 1
        if d[14] != 50:
            return
        if d[13] & 0x08:
            half["first"] = d[15:65]
        elif half["first"] is not None:
            if phase["name"]:
                frames.append((t, phase["name"], half["first"] + d[15:65]))
            half["first"] = None

    stop = threading.Event()

    def silence_out():
        enc = Encoder(channels=2, bitrate=96000)
        frame = enc.encode(np.zeros(1920, np.int16)).ljust(240, b"\0")
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
        cmd.send_plain(0x07, 0x01)
        cmd.send_raw(memory_read_payload(0x13000, 0x40))
        cmd.send_plain(0x16, 0x01)
        if "--pair" in sys.argv:
            class C:
                send = staticmethod(lambda c, s, d=b"": cmd.send_plain(c, s, d))
            pair_steps(C, att, out)
        cmd.send_plain(0x18, 0x04, b"\x01")
        cmd.send_plain(0x18, 0x03, bytes([PHASES[0][1]]))
        cmd.send_plain(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        att.subscribe(chars[CHAR_AUDIO_IN][0], on_audio_in)
        threading.Thread(target=silence_out, daemon=True).start()
        time.sleep(1.0)
        for name, mode in PHASES:
            cmd.send_plain(0x18, 0x03, bytes([mode]))
            time.sleep(0.5)
            out(f">> {name} (18/03 {mode:02x}): TALK NOW for {seconds:.0f} s (count out loud)")
            phase["name"] = name
            time.sleep(seconds)
            phase["name"] = None
            out(f"== {name}: stop talking")
            time.sleep(1.5)
    finally:
        stop.set()
        time.sleep(0.1)
        att.close()
        raw.close()
        log.close()

    print(f"\nbyte 13 values: { {hex(k): v for k, v in sorted(states.items())} }")
    dec = Decoder()
    pcm = []
    by_phase = {}
    for t, name, f in frames:
        try:
            p = dec.decode(f).astype(float)
        except ValueError:
            continue
        pcm.append(p)
        by_phase.setdefault(name, []).append(p)
    if pcm:
        sf.write(DATA / f"mic-{stamp}.wav", np.concatenate(pcm).astype(np.int16), 48000)
    print("mic level per window (noise floor is about -70 dBFS):")
    for name, mode in PHASES:
        ps = by_phase.get(name)
        if not ps:
            print(f"  {name:18} no frames")
            continue
        x = np.concatenate(ps)
        r = np.sqrt((x ** 2).mean())
        peak = max(np.sqrt((p ** 2).mean()) for p in ps)
        print(f"  {name:18} (18/03 {mode:02x}): {len(ps)} frames, rms {20*np.log10(max(r,1e-9)/32768):6.1f} dBFS, "
              f"loudest frame {20*np.log10(max(peak,1e-9)/32768):6.1f} dBFS")
    print(f"decoded mic audio: {DATA / f'mic-{stamp}.wav'}")


if __name__ == "__main__":
    main()
