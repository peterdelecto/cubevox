"""Split a loop capture into tone harmonics and the rest.

Usage: python3 -I tools/bench/loop_spectrum.py capture.wav [f0 Hz]
Reads channel 2 (the box's output on the Scarlett), drops the first second, averages
8192-point Hann spectra at half overlap, then reports per band the RMS of the bins on the
harmonic grid of f0 (±2 bins) and the RMS of everything else, which is the noise.
"""
import sys
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        ch = w.getnchannels()
        sr = w.getframerate()
        width = w.getsampwidth()
        raw = w.readframes(n)
    dtype = {2: np.int16, 4: np.int32}[width]
    x = np.frombuffer(raw, dtype=dtype).astype(np.float64) / float(2 ** (8 * width - 1))
    x = x.reshape(-1, ch)
    return x[:, min(1, ch - 1)], sr


def db(v):
    return 20.0 * np.log10(max(v, 1e-12))


def main():
    path = sys.argv[1]
    f0 = float(sys.argv[2]) if len(sys.argv) > 2 else 220.0
    x, sr = read_wav(path)
    x = x[sr:]
    n = 8192
    hop = n // 2
    win = np.hanning(n)
    scale = np.sqrt(np.sum(win ** 2))
    acc = None
    count = 0
    for start in range(0, len(x) - n, hop):
        seg = x[start:start + n] * win
        spec = np.abs(np.fft.rfft(seg)) / scale
        acc = spec ** 2 if acc is None else acc + spec ** 2
        count += 1
    psd = acc / count
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    binw = sr / n
    harmonic = np.zeros(len(freqs), dtype=bool)
    k = 1
    while k * f0 < sr / 2:
        centre = int(round(k * f0 / binw))
        harmonic[max(0, centre - 2):centre + 3] = True
        k += 1
    total = np.sqrt(np.sum(psd) * 2 / n)
    print("%s: total RMS %.1f dBFS, %d spectra averaged" % (path.split('/')[-1], db(total), count))
    print("  band          harmonics   rest (noise)")
    for lo, hi in ((100, 2000), (2000, 6000), (6000, 8000), (8000, 12000), (12000, 16000), (16000, 20000)):
        sel = (freqs >= lo) & (freqs < hi)
        h = np.sqrt(np.sum(psd[sel & harmonic]) * 2 / n)
        r = np.sqrt(np.sum(psd[sel & ~harmonic]) * 2 / n)
        print("  %5d-%-5d   %7.1f dB   %7.1f dB" % (lo, hi, db(h), db(r)))


if __name__ == "__main__":
    main()
