#!/usr/bin/env python3
"""Compares two directories of rendered WAVs (A reference, B candidate) case by case.

Prints, per case, the peak absolute difference and the residual level (B - A) relative to
A in dB. A case passes when the residual is at or below the tolerance; -200 means bit-exact.

  engine_ab_compare.py /tmp/cubevox-ab/a /tmp/cubevox-ab/b --tol -200
"""

import argparse
import os
import struct
import sys

import numpy as np


def load(path):
    """First channel of a PCM16 or float32 WAV as float64. The wave module rejects float."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"[ERROR] {path}: not a WAV")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        sys.exit(f"[ERROR] {path}: missing fmt or data chunk")
    tag, ch, _, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(pcm, np.float32).astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(pcm, np.int16).astype(np.float64) / 32768.0
    else:
        sys.exit(f"[ERROR] {path}: unsupported format tag {tag}, {bits} bits")
    return x.reshape(-1, ch)[:, 0]


def db(x):
    return 20.0 * np.log10(x) if x > 0 else -200.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--tol", type=float, default=-200.0, help="max residual dB re A; -200 = exact")
    args = ap.parse_args()

    names = sorted(f for f in os.listdir(args.a) if f.endswith(".wav"))
    failures = 0
    print(f"{'case':<12}{'peak diff':>12}{'residual dB':>14}  result")
    for f in names:
        a = load(os.path.join(args.a, f))
        b = load(os.path.join(args.b, f))
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        peak = float(np.max(np.abs(a - b))) if n else 0.0
        ref = float(np.sqrt(np.mean(a * a))) if n else 0.0
        res = float(np.sqrt(np.mean((a - b) ** 2))) if n else 0.0
        rel = db(res / ref) if ref > 0 else (-200.0 if res == 0 else 0.0)
        ok = rel <= args.tol
        failures += 0 if ok else 1
        print(f"{f[:-4]:<12}{peak:>12.2e}{rel:>14.1f}  {'ok' if ok else 'FAIL'}")
    print(f"[ab] {len(names)} cases, {failures} over tolerance {args.tol:g} dB")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
