"""Right-angle corner check (canon 44: no 90 degree corners, 45 degrees or arcs).

Reports every point where two track segments of the same net and layer meet at a
90 degree turn, unless the point lies inside a same-net pad on that layer or on a
via. Usage: kicad-python corner_check.py board.kicad_pcb [--json out.json]
"""
import json
import math
import sys
from collections import defaultdict

import pcbnew

TOL_NM = 1000          # endpoints within 1 um are the same point
ANGLE_TOL_DEG = 1.0


def main(path, out=None):
    board = pcbnew.LoadBoard(path)
    segs = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
    pads = [p for fp in board.GetFootprints() for p in fp.Pads()]

    ends = defaultdict(list)
    for t in segs:
        for pt, other in ((t.GetStart(), t.GetEnd()), (t.GetEnd(), t.GetStart())):
            key = (t.GetNetCode(), t.GetLayer(), round(pt.x / TOL_NM), round(pt.y / TOL_NM))
            ends[key].append((t, pt, other))

    hits = []
    for (net, layer, _, _), arms in ends.items():
        if len(arms) < 2:
            continue
        pt = arms[0][1]
        probe = pcbnew.SHAPE_CIRCLE(pt, 10)
        if any(v.GetNetCode() == net and v.GetEffectiveShape(layer).Collide(probe, 0) for v in vias):
            continue
        if any(p.GetNetCode() == net and p.IsOnLayer(layer)
               and p.GetEffectiveShape(layer).Collide(probe, 0) for p in pads):
            continue
        dirs = []
        for t, p, o in arms:
            dx, dy = o.x - p.x, o.y - p.y
            n = math.hypot(dx, dy)
            if n:
                dirs.append((dx / n, dy / n))
        for i in range(len(dirs)):
            for j in range(i + 1, len(dirs)):
                dot = dirs[i][0] * dirs[j][0] + dirs[i][1] * dirs[j][1]
                ang = math.degrees(math.acos(max(-1.0, min(1.0, dot))))
                if abs(ang - 90.0) < ANGLE_TOL_DEG:
                    hits.append({"net": arms[0][0].GetNetname(), "layer": board.GetLayerName(layer),
                                 "x": round(pt.x / 1e6, 4), "y": round(pt.y / 1e6, 4),
                                 "arms": len(arms)})
    for h in hits:
        print("  %-28s %-5s (%.3f, %.3f)%s" % (h["net"], h["layer"], h["x"], h["y"],
                                               "  branch" if h["arms"] > 2 else ""))
    print("right-angle corners: %d" % len(hits))
    if out:
        json.dump(hits, open(out, "w"), indent=1)
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    out = args[args.index("--json") + 1] if "--json" in args else None
    sys.exit(main(args[0], out))
