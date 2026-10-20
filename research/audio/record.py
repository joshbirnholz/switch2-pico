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
import signal
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
    raise RuntimeError("USB sound card (Cable Creation) not found, or in use (another record.py?)")


class Recorder:
    """Records from the USB card in the background, from start() to stop(),
    writing to disk as it goes (a killed recording keeps what it had). The
    tests start it once the controller has connected. The card can only be
    open once: a running record.py blocks it."""

    def __init__(self, name):
        self.name = name
        self.frames = 0
        self.start_time = None
        DATA.mkdir(exist_ok=True)
        self.file = sf.SoundFile(DATA / f"{name}.wav", "w", samplerate=RATE, channels=1, subtype="PCM_16")
        self.stream = sd.InputStream(samplerate=RATE, channels=1, dtype="int16", device=find_device(),
                                     callback=self._cb)

    def _cb(self, indata, frames, t, status):
        if self.start_time is None:
            # wall clock of this block's first sample
            self.start_time = time.time() - (t.currentTime - t.inputBufferAdcTime) - frames / RATE
            (DATA / f"{self.name}.json").write_text(json.dumps({"start": self.start_time, "rate": RATE}))
        self.file.write(indata.copy())
        self.frames += frames

    def start(self):
        self.stream.start()
        return self

    def stop(self):
        self.stream.stop()
        self.stream.close()
        self.file.close()
        return self.frames / RATE


def main():
    seconds = float(sys.argv[1])
    name = sys.argv[2] if len(sys.argv) > 2 else time.strftime("rec-%Y%m%d-%H%M%S")
    signal.signal(signal.SIGTERM, lambda *a: (_ for _ in ()).throw(KeyboardInterrupt()))
    r = Recorder(name).start()
    try:
        time.sleep(seconds)
    except KeyboardInterrupt:
        pass
    secs = r.stop()
    x, _ = sf.read(DATA / f"{name}.wav", dtype="int16")
    x = x.astype(float)
    print(f"{name}: {secs:.0f} s, rms {np.sqrt((x ** 2).mean()):.1f}, peak {np.abs(x).max():.0f}, "
          f"clipped {(np.abs(x) >= 32767).sum()}")


if __name__ == "__main__":
    main()
