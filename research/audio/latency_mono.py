#!/usr/bin/env python3
"""Output latency in Stable (18/03 07) vs Low Latency (02), and whether the
controller accepts mono Opus frames, measured at the jack with the USB card.

Latency: on one continuous stream, 60 ms bursts of 1 kHz every 0.6 s; the
wall-clock time each burst's first frame is written is logged, and the
recording (same clock, record.Recorder) shows when it reached the jack.
This includes the laptop's own Bluetooth queue, so it's an upper bound and
best compared between modes.

Mono: 2 s of 440 Hz as mono CELT frames (TOC 0xF8) in the same 240-byte
two-half packets.

Usage: .venv/bin/python research/audio/latency_mono.py
"""
import json
import time

import numpy as np
import soundfile as sf

from audio_stream import DATA, connect, tone
from console_out import ConsoleCommander
from mic_ablate import GROUPS
from opus import Encoder
from record import Recorder
from s2 import CHAR_AUDIO_OUT

RATE = 48000
FRAME = 960


def main():
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = f"latency-{stamp}"

    class A:
        pass
    args = A()
    args.address = "3C:A9:AB:69:58:AF"
    att, chars = connect(args, print)
    rec = Recorder(base).start()
    sends = []          # (mode, wall time of the burst's first frame)
    mono_window = None
    try:
        cmd = ConsoleCommander(att, print)
        for st in GROUPS["full"]:
            if st[0] == "cmd":
                cmd.send_plain(st[1], st[2], st[3])
            else:
                att.write(st[1], st[2])
                if st[1] == 0x000F:
                    att.on_notify(0x000E, lambda t, d: None)
        cmd.send_plain(0x18, 0x04, b"\x01")
        cmd.send_plain(0x18, 0x03, b"\x07")
        cmd.send_plain(0x17, 0x02, bytes.fromhex("80bb000002f000"))
        handle = chars[CHAR_AUDIO_OUT][0]
        stereo = Encoder(channels=2, bitrate=96000)
        mono = Encoder(channels=1, bitrate=96000)
        t0 = time.monotonic()
        n = [0]

        def send(frame):
            i = n[0]
            frame = frame.ljust(240, b"\0")[:240]
            att.write_cmd(handle, bytes([0x00, 0x04, 0x78]) + frame[:120])
            time.sleep(max(0.0, i * 0.020 + 0.005 - (time.monotonic() - t0)))
            att.write_cmd(handle, bytes([0x00, 0x02, 0x78]) + frame[120:])
            n[0] += 1
            time.sleep(max(0.0, n[0] * 0.020 - (time.monotonic() - t0)))

        burst = tone(0.06, freq=1000, level=0.1)
        silence = np.zeros(FRAME, np.int16)

        def stereo_frame(pcm):
            return stereo.encode(np.repeat(pcm, 2))

        for _ in range(50):          # 1 s of silence to get the stream going
            send(stereo_frame(silence))
        for mode in (0x07, 0x02):
            cmd.send_plain(0x18, 0x03, bytes([mode]))
            for _ in range(25):
                send(stereo_frame(silence))
            for b in range(10):
                sends.append((mode, time.time()))
                for k in range(3):
                    send(stereo_frame(burst[k * FRAME:(k + 1) * FRAME]))
                for _ in range(27):
                    send(stereo_frame(silence))
        # mono frames, Low Latency still on
        for _ in range(25):
            send(stereo_frame(silence))
        pcm = tone(2.0, freq=440, level=0.03)
        mono_window = [time.time(), None]
        for i in range(len(pcm) // FRAME):
            send(mono.encode(pcm[i * FRAME:(i + 1) * FRAME]))
        mono_window[1] = time.time()
        for _ in range(25):
            send(stereo_frame(silence))
    finally:
        att.close()
        rec.stop()

    start = json.loads((DATA / f"{base}.json").read_text())["start"]
    x, sr = sf.read(DATA / f"{base}.wav", dtype="int16")
    x = x.astype(float)
    # 1 kHz envelope in 1 ms steps
    t = np.arange(len(x)) / sr
    lo = np.sin(2 * np.pi * 1000 * t) * x
    lq = np.cos(2 * np.pi * 1000 * t) * x
    w = 48
    k = len(x) // w
    env = np.sqrt(lo[:k * w].reshape(k, w).mean(1) ** 2 + lq[:k * w].reshape(k, w).mean(1) ** 2)
    floor = np.median(env)
    thresh = max(floor * 10, env.max() * 0.2)
    print("\nlatency per burst (first frame written -> 1 kHz at the jack):")
    lat = {0x07: [], 0x02: []}
    for mode, ts in sends:
        i0 = int((ts - start) * 1000)
        seg = env[i0:i0 + 500]
        hit = np.nonzero(seg > thresh)[0]
        if len(hit):
            lat[mode].append(hit[0])
    for mode, name in ((0x07, "Stable (07)"), (0x02, "Low Latency (02)")):
        v = lat[mode]
        if v:
            print(f"  {name:17} {len(v)}/10 bursts heard, latency median {np.median(v):.0f} ms "
                  f"(min {min(v)}, max {max(v)})")
        else:
            print(f"  {name:17} no bursts heard")
    if mono_window and mono_window[1]:
        a = int((mono_window[0] - start + 0.6) * sr)
        b = int((mono_window[1] - start) * sr)
        seg = x[a:b]
        win = np.hanning(len(seg))
        sp = np.abs(np.fft.rfft(seg * win)) ** 2
        f = np.fft.rfftfreq(len(seg), 1 / sr)
        band = sp[(f > 430) & (f < 450)].sum()
        lvl = 20 * np.log10(max(np.sqrt(4 * band / (len(seg) * (win ** 2).sum())) / np.sqrt(2), 1e-9) / 32768)
        print(f"\nmono frames (TOC 0xF8): 440 Hz at the jack {lvl:.1f} dBFS, share {band / sp[1:].sum():.2f} "
              f"({'PLAYS' if lvl > -40 else 'silent'})")


if __name__ == "__main__":
    main()
