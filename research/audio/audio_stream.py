#!/usr/bin/env python3
"""Phases 3-5: start the headset audio stream, capture the mic, and try
output formats, with libopus as the test equipment.

Sends only what's known: the dongle's connect sequence (no pairing; the
controller must already be paired, see phase1.py --pair), the 0x17/0x02
stream setup from the console, and writes to the audio-out
characteristic.

Run it yourself in a terminal (it asks you to talk and listen), with
earbuds-with-mic (4-pole) in the controller's jack, KDE Connect closed:
  research/audio/wifi-off.sh .venv/bin/python research/audio/audio_stream.py [ADDRESS]

Writes to research/audio/data/:
  audio-STAMP.raw      every audio-in notification: "t hex"
  audio-STAMP-full.wav mic frames decoded as sent (50-byte slots)
  audio-STAMP-trim.wav mic frames with trailing zero bytes cut off
  audio-STAMP.log      what happened
"""
import argparse
import sys
import threading
import time
from collections import Counter
from pathlib import Path

import numpy as np
import soundfile as sf

from att import Att
from opus import VERSION, Decoder, Encoder, toc_info
from s2 import (CHAR_AUDIO_IN, CHAR_AUDIO_OUT, CHAR_CMD, CHAR_CMD_RSP, Commander,
                memory_read_payload)

DATA = Path(__file__).resolve().parent / "data"
RATE = 48000
JACK = {0x00: "nothing", 0x05: "headphones", 0x07: "headset with mic"}


class MicCapture:
    """Collects audio-in notifications and decodes their frames."""

    def __init__(self, raw, out):
        self.raw = raw
        self.out = out
        self.dec_full = Decoder()
        self.dec_trim = Decoder()
        self.full, self.trim = [], []
        self.n = 0
        self.frames = 0
        self.bad = Counter()
        self.tocs = Counter()
        self.lens = Counter()
        self.jack = None
        self.last_seq = None
        self.lost = 0
        self.t_first = None
        self.recording = False
        self.lock = threading.Lock()

    def __call__(self, t, d):
        d = bytes(d)
        self.raw.write(f"{t:.4f} {d.hex()}\n")
        with self.lock:
            self.n += 1
            if self.t_first is None:
                self.t_first = t
            if len(d) < 15:
                return
            if self.last_seq is not None and (d[0] - self.last_seq) & 0xFF != 1:
                self.lost += (d[0] - self.last_seq - 1) & 0xFF
            self.last_seq = d[0]
            jack = d[13] & ~0x08
            if jack != self.jack:
                self.out(f"   jack: {JACK.get(jack, hex(jack))} (byte 13 = {d[13]:#04x})")
                self.jack = jack
            if not d[13] & 0x08:
                return
            ln = d[14]
            frame = d[15:15 + ln]
            self.frames += 1
            self.lens[ln] += 1
            if frame:
                self.tocs[frame[0]] += 1
            if not self.recording:
                return
            trimmed = frame.rstrip(b"\0") or frame[:1]
            for dec, buf, pkt, key in ((self.dec_full, self.full, frame, "full"),
                                       (self.dec_trim, self.trim, trimmed, "trim")):
                try:
                    buf.append(dec.decode(pkt))
                except ValueError:
                    self.bad[key] += 1

    def summary(self):
        with self.lock:
            dur = (time.monotonic() - self.t_first) if self.t_first else 0
            tocs = ", ".join(f"{t:#04x} ({toc_info(t)}) ×{c}" for t, c in self.tocs.most_common(6))
            return (f"{self.n} notifications ({self.n / max(dur, 1e-3):.1f}/s), {self.frames} with a frame, "
                    f"{self.lost} lost by sequence; frame lengths {dict(self.lens)}; TOCs: {tocs}; "
                    f"decode errors {dict(self.bad)}")


def connect(args, out):
    out(f"connecting to {args.address} (press a button on the controller to wake it)")
    deadline = time.monotonic() + 60
    while True:
        try:
            att = Att(args.address, connect_timeout=30, log=out)
            break
        except OSError as e:
            if time.monotonic() > deadline:
                raise
            out(f"   {e}; retrying")
    att.exchange_mtu()
    chars = {}
    for start, end, _ in att.services():
        for decl, value, props, cuuid in att.characteristics(start, end):
            chars[cuuid] = (value, props)
    out(f"connected, mtu {att.mtu}; audio in {chars[CHAR_AUDIO_IN][0]:#06x}, "
        f"audio out {chars[CHAR_AUDIO_OUT][0]:#06x}")
    return att, chars


def init(cmd):
    """The dongle's sequence for a controller that's already paired."""
    cmd.send(0x07, 0x01)
    cmd.send_raw(memory_read_payload(0x13000, 0x40))
    cmd.send(0x16, 0x01)
    cmd.send(0x0A, 0x02, bytes([0x03, 0, 0, 0]))
    cmd.send(0x09, 0x07, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]))
    cmd.send(0x0C, 0x02, bytes([0x2F, 0, 0, 0]))
    cmd.send(0x0A, 0x08, bytes([0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                                0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
    cmd.send(0x0C, 0x04, bytes([0x2F, 0, 0, 0]))


def tone(seconds, freq=1000.0, level=0.1):
    t = np.arange(int(seconds * RATE)) / RATE
    return (np.sin(2 * np.pi * freq * t) * level * 32767).astype(np.int16)


def output_variants(args):
    """(name, frame samples, function: pcm frame -> payload)."""
    e20 = Encoder(bitrate=20000)
    e5 = Encoder(bitrate=80000)
    e20b = Encoder(bitrate=20000)
    e5b = Encoder(bitrate=80000)
    return [
        ("Opus CELT 20 ms, 50 bytes, bare", 960, e20.encode),
        ("Opus CELT 5 ms, 50 bytes, bare", 240, e5.encode),
        ("Opus CELT 20 ms, 50 bytes, length byte first", 960, lambda p: (lambda x: bytes([len(x)]) + x)(e20b.encode(p))),
        ("Opus CELT 5 ms, 50 bytes, length byte first", 240, lambda p: (lambda x: bytes([len(x)]) + x)(e5b.encode(p))),
        ("PCM16 LE mono, 5 ms (480 bytes)", 240, lambda p: p.astype("<i2").tobytes()),
    ]


def play(att, handle, name, frame, encode, seconds, out):
    pcm = tone(seconds)
    period = frame / RATE
    n = len(pcm) // frame
    t0 = time.monotonic()
    sent = 0
    for i in range(n):
        payload = encode(pcm[i * frame:(i + 1) * frame])
        if len(payload) > att.mtu - 3:
            out(f"   {name}: payload {len(payload)} bytes is over the MTU; skipped")
            return
        att.write_cmd(handle, payload)
        sent += 1
        # pace in real time, a little ahead
        ahead = (i + 1) * period - (time.monotonic() - t0)
        if ahead > 0.02:
            time.sleep(ahead - 0.02)
        if att.closed.is_set():
            break
    out(f"   sent {sent} packets in {time.monotonic() - t0:.1f} s")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("address", nargs="?", default="3C:A9:AB:69:58:AF")
    p.add_argument("--mic-seconds", type=float, default=20)
    p.add_argument("--tone-seconds", type=float, default=4)
    p.add_argument("--setup", default="80bb000002f000",
                   help="0x17/0x02 payload (hex); the console's is 80 bb 00 00 02 f0 00")
    args = p.parse_args()

    DATA.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log = open(DATA / f"audio-{stamp}.log", "w")
    raw = open(DATA / f"audio-{stamp}.raw", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    out(f"libopus {VERSION}")
    att, chars = connect(args, out)
    try:
        cmd = Commander(att, *chars[CHAR_CMD], chars[CHAR_CMD_RSP][0], lambda s: log.write(s + "\n"))
        cmd.start()
        init(cmd)
        setup = bytes.fromhex(args.setup)
        rsp = cmd.send(0x17, 0x02, setup)
        out(f"0x17/0x02 {setup.hex(' ')} -> {rsp.hex(' ') if rsp else 'no response'}")
        rsp = cmd.send(0x18, 0x01)
        out(f"0x18/0x01 -> {rsp.hex(' ') if rsp else 'no response'}")

        mic = MicCapture(raw, out)
        att.subscribe(chars[CHAR_AUDIO_IN][0], mic)
        time.sleep(2)
        out(f"stream: {mic.summary()}")

        out("")
        out(f"== MIC: talk into the earbud mic for {args.mic_seconds:.0f} s, starting NOW "
            "(count out loud, then a few seconds of silence)")
        mic.recording = True
        for _ in range(int(args.mic_seconds)):
            time.sleep(1)
            if att.closed.is_set():
                break
        mic.recording = False
        out(f"   {mic.summary()}")
        for key, buf in (("full", mic.full), ("trim", mic.trim)):
            if buf:
                pcm = np.concatenate(buf)
                path = DATA / f"audio-{stamp}-{key}.wav"
                sf.write(path, pcm, RATE)
                out(f"   wrote {path.name}: {len(pcm) / RATE:.1f} s, peak {np.abs(pcm).max()}")

        out("")
        out("== OUTPUT: hold the earbuds NEAR your ear, not in it, in case a format comes out loud.")
        out(f"   Each format plays a 1 kHz tone for {args.tone_seconds:.0f} s, with 2 s of quiet between.")
        out_handle = chars[CHAR_AUDIO_OUT][0]
        for k, (name, frame, encode) in enumerate(output_variants(args), 1):
            if att.closed.is_set():
                break
            out(f"-- {k}. {name}: listen now")
            play(att, out_handle, name, frame, encode, args.tone_seconds, out)
            time.sleep(2)
        out("")
        out("Done. Note which numbers you heard a clean tone on (or noise, or a buzz in the grip).")
        out(f"stream at the end: {mic.summary()}")
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        raw.close()
        log.close()


if __name__ == "__main__":
    main()
