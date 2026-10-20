#!/usr/bin/env python3
"""Record from the USB sound card's input (the controller's jack, through
an aux cable) for the output tests, noting the wall-clock start time so
measure_tone.py can line it up with a test's log.

Usage: .venv/bin/python research/audio/record.py SECONDS [NAME]
(or Recorder(name).start() ... .stop() from a test)
Writes research/audio/data/NAME.wav and NAME.json (start time, rate).
Set the card up first: Auto Gain Control off, Mic Capture Volume ~30
(amixer -c Creation), or the input clips.
"""
import json
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd
import soundfile as sf

DATA = Path(__file__).resolve().parent / "data"
RATE = 48000


def find_device():
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and "Cable Creation" in d["name"]:
            return i
    raise SystemExit("USB sound card (Cable Creation) not found")


class Recorder:
    """Records from the USB card in the background, from start() to stop().
    The tests start it once the controller has connected."""

    def __init__(self, name):
        self.name = name
        self.chunks = []
        self.start_time = None
        self.stream = sd.InputStream(samplerate=RATE, channels=1, dtype="int16", device=find_device(),
                                     callback=self._cb)

    def _cb(self, indata, frames, t, status):
        if self.start_time is None:
            # wall clock of this block's first sample
            self.start_time = time.time() - (t.currentTime - t.inputBufferAdcTime) - frames / RATE
        self.chunks.append(indata.copy())

    def start(self):
        self.stream.start()
        return self

    def stop(self):
        self.stream.stop()
        self.stream.close()
        x = np.concatenate(self.chunks) if self.chunks else np.zeros((0, 1), np.int16)
        DATA.mkdir(exist_ok=True)
        sf.write(DATA / f"{self.name}.wav", x, RATE)
        (DATA / f"{self.name}.json").write_text(json.dumps({"start": self.start_time, "rate": RATE}))
        return len(x) / RATE


def main():
    seconds = float(sys.argv[1])
    name = sys.argv[2] if len(sys.argv) > 2 else time.strftime("rec-%Y%m%d-%H%M%S")
    dev = find_device()
    DATA.mkdir(exist_ok=True)
    frames = int(seconds * RATE)
    start = time.time()
    x = sd.rec(frames, samplerate=RATE, channels=1, dtype="int16", device=dev)
    sd.wait()
    sf.write(DATA / f"{name}.wav", x, RATE)
    (DATA / f"{name}.json").write_text(json.dumps({"start": start, "rate": RATE, "device": sd.query_devices(dev)["name"]}))
    x = x[:, 0].astype(float)
    print(f"{name}: {seconds:.0f} s, rms {np.sqrt((x ** 2).mean()):.1f}, peak {np.abs(x).max():.0f}, "
          f"clipped {(np.abs(x) >= 32767).sum()}")


if __name__ == "__main__":
    main()
