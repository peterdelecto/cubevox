#!/usr/bin/env python3
"""Cuts the golden frames of the OLED host tests out of an oled_motion raw stream.

  python3 -I tools/oled_golden.py IN.raw test/firmware/golden

oled_motion draws the proof screens through the firmware's canvas and fonts, so its frames are the
pixel reference of the runtime screens. Each golden is one frame's 1024 page-layout bytes, the
bytes a Canvas holds. The scene table sits beside IN.raw as IN.scenes. Run it again only when
the owner changes the proof; the host tests then follow the new frames.
"""

import struct
import sys
from pathlib import Path

HEADER = struct.Struct("<4s7I")
MAGIC = b"CVOM"
SCREEN_BYTES = 128 * 64 // 8
CAPTION_BYTES = 128 * 16 // 8
STRIDE = SCREEN_BYTES + CAPTION_BYTES

# name -> (scene number, frame offset in the scene). Scene 2 turns SLAPBACK TIME from 120 to 210
# MS, 3 turns SPRING REVERB MIX from 30 to 65 %, 4 turns OCTAVE SEMITONES from 0 to -12 ST, 5 turns
# GATE THRESHOLD from -40 to -24 DB, 9 mutes from the silent voice, and 10 moves from one readout
# to another at offset 100, and 11 flips slot 6's toggle on at 0.4 s and off at 1.4 s, so offsets
# 30 and 60 are the settled ON and OFF frames. Scene 12 is the voice screen on a scripted drive
# (input steady, output cycling), so its frame 79 holds the level bar and the ribbon together.
GOLDENS = {
    "settled-210ms": ("2", 99),
    "settled-65pct": ("3", 99),
    "settled-m12st": ("4", 99),
    "settled-m24db": ("5", 99),
    "muted-1": ("9", 0),
    "muted-2": ("9", 1),
    "muted-3": ("9", 2),
    "switch-1": ("10", 100),
    "switch-2": ("10", 101),
    "switch-3": ("10", 102),
    "toggle-on": ("11", 30),
    "toggle-off": ("11", 60),
    "voice-meter-ribbon": ("12", 79),
}
for offset in (8, 12, 16, 20, 24, 28, 32, 40, 52):
    GOLDENS[f"roll-time-{offset}"] = ("2", offset)
for offset in (6, 12, 16, 20, 24, 28, 32, 40):
    GOLDENS[f"roll-semi-{offset}"] = ("4", offset)


def fail(message):
    print(f"oled_golden: error: {message}", file=sys.stderr)
    sys.exit(2)


def read_scenes(raw_path):
    table = Path(raw_path).with_suffix(".scenes")
    if not table.exists():
        fail(f"scene table {table} not found")
    first = {}
    for line in table.read_text().splitlines():
        if line.strip():
            number, _slug, start, _end = line.split("\t")
            first[number] = int(start)
    return first


def main(argv):
    if len(argv) != 3:
        fail("usage: python3 -I tools/oled_golden.py IN.raw OUT_DIR")
    data = Path(argv[1]).read_bytes()
    magic, _version, width, height, _cap_w, _cap_h, _fps, count = HEADER.unpack_from(data)
    if magic != MAGIC or (width, height) != (128, 64):
        fail("not an oled_motion stream")
    out_dir = Path(argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    first = read_scenes(argv[1])
    for name, (scene, offset) in sorted(GOLDENS.items()):
        if scene not in first:
            fail(f"scene {scene} is not in the stream")
        index = first[scene] + offset
        if index >= count:
            fail(f"{name}: frame {index} is past the end of the stream")
        base = HEADER.size + index * STRIDE
        (out_dir / f"{name}.bin").write_bytes(data[base:base + SCREEN_BYTES])
    print(f"wrote {len(GOLDENS)} frames to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
