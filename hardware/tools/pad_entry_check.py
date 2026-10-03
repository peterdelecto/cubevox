"""Owner rule: a trace enters a pad straight on.

For every track segment with an endpoint inside a pad, the segment angle must be
within 1 degree of a multiple of 90 relative to the pad. Round pads pass.
Read-only. Run with the KiCad python:
  PYTHONPATH=<ClaudeRouter> <kicad python> tools/pad_entry_check.py [board]
Exit 1 when any violation is found.
"""
import math
import sys

import pcbnew

TOL_DEG = 1.0


def mm(v):
    return pcbnew.ToMM(v)


def main(path):
    board = pcbnew.LoadBoard(path)
    pads = [(fp.GetReference(), p) for fp in board.GetFootprints() for p in fp.Pads()]
    violations = 0
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            continue
        a, b = t.GetStart(), t.GetEnd()
        if a == b:
            continue
        seg = math.degrees(math.atan2(mm(b.y) - mm(a.y), mm(b.x) - mm(a.x)))
        for end in (a, b):
            for ref, pad in pads:
                if not pad.IsOnLayer(t.GetLayer()) or pad.GetNetCode() != t.GetNetCode():
                    continue
                if pad.GetShape() == pcbnew.PAD_SHAPE_CIRCLE:
                    continue
                if not pad.HitTest(end):
                    continue
                rel = (seg - pad.GetOrientationDegrees()) % 90.0
                off = min(rel, 90.0 - rel)
                if off > TOL_DEG:
                    violations += 1
                    print("%s %s %s.%s (%.3f,%.3f)-(%.3f,%.3f) angle %.1f"
                          % (t.GetNetname(), board.GetLayerName(t.GetLayer()), ref,
                             pad.GetNumber(), mm(a.x), mm(a.y), mm(b.x), mm(b.y), rel))
    print("pad entry violations: %d" % violations)
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "cubevox.kicad_pcb"))
