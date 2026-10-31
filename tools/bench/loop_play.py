#!/usr/bin/env python3
"""Play the Adam samples out of the Scarlett in a loop until killed, for listening on the box.

Usage: build/loop/venv/bin/python tools/bench/loop_play.py [wav ...]
With no arguments it plays the three clips in samples/ with half a second of silence between them.
The Scarlett is found by name; the clips are mono 48 kHz and go to both outputs at their own level.
"""

import sys
import wave
from pathlib import Path

import numpy as np
import sounddevice as sd

SR = 48000
GAP_S = 0.5


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        if w.getframerate() != SR or w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit(f"{path}: want mono 48 kHz 16-bit")
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return data.astype(np.float32) / 32768.0


def main():
    root = Path(__file__).resolve().parents[2]
    paths = [Path(p) for p in sys.argv[1:]] or sorted((root / "samples").glob("adam_vox_sample*.wav"))
    if not paths:
        raise SystemExit("no clips")
    dev = None
    for i, d in enumerate(sd.query_devices()):
        if "Scarlett" in d["name"] and d["max_output_channels"] >= 2:
            dev = i
    if dev is None:
        raise SystemExit("no Scarlett on the bus")
    gap = np.zeros(int(GAP_S * SR), dtype=np.float32)
    mono = np.concatenate([np.concatenate([read_wav(p), gap]) for p in paths])
    out = np.repeat(mono[:, None], 2, axis=1)
    print(f"looping {len(paths)} clips, {len(mono) / SR:.1f} s per pass, device {dev}", flush=True)
    while True:
        sd.play(out, samplerate=SR, device=dev, blocking=True)


if __name__ == "__main__":
    main()
