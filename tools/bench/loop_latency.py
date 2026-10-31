"""Round trip through the Scarlett on one clock: play a click train and record at once.

Usage: python3 -m venv build/loop/venv && build/loop/venv/bin/pip install sounddevice numpy; then build/loop/venv/bin/python tools/bench/loop_latency.py <label> [repeats]
One PortAudio duplex stream on the Scarlett plays 5 ms 1 kHz bursts, one a second, on both
outputs and records both inputs for 5 s; the lag of the recorded input 2 (the box) against
the played signal is the round trip through the Scarlett and the box. Run A with the box in
the loop minus run B with the cable straight into input 2 is the box's latency.
"""
import sys

import numpy as np
import sounddevice as sd

SR = 48000
DUR = 5.0
BURST = int(0.005 * SR)


def clicks():
    n = int(SR * DUR)
    x = np.zeros(n, dtype=np.float32)
    t = np.arange(BURST) / SR
    tone = (np.sin(2 * np.pi * 1000.0 * t) * np.hanning(BURST) * 10 ** (-12 / 20)).astype(np.float32)
    for s in range(SR // 2, n - BURST, SR):
        x[s:s + BURST] = tone
    return x


def carrier():
    # A steady 220 Hz tone at -20 dBFS under the clicks, so a pitch tracker has something to lock to.
    n = int(SR * DUR)
    t = np.arange(n) / SR
    return (np.sin(2 * np.pi * 220.0 * t) * 10 ** (-20 / 20)).astype(np.float32)


def lag_ms(played, recorded):
    n = len(played)
    fa = np.fft.rfft(played, 2 * n)
    fb = np.fft.rfft(recorded, 2 * n)
    xc = np.fft.irfft(fb * np.conj(fa), 2 * n)[:SR - 1]
    lag = int(np.argmax(xc))
    if 0 < lag < len(xc) - 1:
        y0, y1, y2 = xc[lag - 1], xc[lag], xc[lag + 1]
        d = y0 - 2 * y1 + y2
        lag += 0.5 * (y0 - y2) / d if d != 0 else 0.0
    return lag * 1000.0 / SR


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "run"
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    dev = None
    for i, d in enumerate(sd.query_devices()):
        if "Scarlett" in d["name"]:
            dev = i
    if dev is None:
        raise SystemExit("no Scarlett on the bus")
    x = clicks()
    played = x + carrier() if "--tone" in sys.argv else x
    out = np.repeat(played[:, None], 2, axis=1)
    results = []
    for r in range(repeats):
        rec = sd.playrec(out, samplerate=SR, channels=2, device=(dev, dev), blocking=True, dtype="float32")
        box = rec[:, 1].astype(np.float64)
        peak = float(np.max(np.abs(box)))
        if peak < 1e-3:
            print("%s %d: input 2 silent" % (label, r + 1))
            continue
        ms = lag_ms(x.astype(np.float64), box)
        results.append(ms)
        print("%s %d: %.2f ms, input 2 peak %.1f dBFS" % (label, r + 1, ms, 20 * np.log10(peak)))
    if results:
        print("%s median %.2f ms, spread %.2f ms" % (label, float(np.median(results)), float(np.max(results) - np.min(results))))


if __name__ == "__main__":
    main()
