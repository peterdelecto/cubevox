"""Chamfer two-arm right-angle corners: replace corner P with P-d*u1 -> P+d*u2.

usage: chamfer.py board corners.json out.json [dmax] [overrides.json]
corners.json: corner_check output. overrides: {"x,y": d} forces a leg (0 = skip).
Legs: min(dmax, room on each arm). Room = half the arm if its far end is another
corner, else arm length minus the pad's half-extent along the arm plus 0.3 when the
far end sits in a pad, else arm length minus 0.3. Corners whose leg would fall under
0.4 are skipped and reported.
"""
import json, math, os, sys
sys.path.insert(0, "/Users/j/Documents/Claude/Projects/ClaudeRouter")
import pcbnew
from foreman import kicad

board_path, corners_path, out_path = sys.argv[1:4]
dmax = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
over = json.load(open(sys.argv[5])) if len(sys.argv) > 5 else {}
board = kicad.load_board(board_path)
segs = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
pads = [p for fp in board.GetFootprints() for p in fp.Pads()]
mm = lambda v: v / 1e6
fm = lambda v: int(round(v * 1e6))


def at(p, x, y):
    return abs(mm(p.x) - x) < 0.002 and abs(mm(p.y) - y) < 0.002


corners = [c for c in json.load(open(corners_path)) if c["arms"] == 2]
keys = {(c["net"], c["layer"], round(c["x"], 3), round(c["y"], 3)) for c in corners}
pts = {(round(c["x"], 3), round(c["y"], 3)) for c in corners}


def pad_room(net, layer, x, y, ux, uy):
    """Half-extent of a same-net pad at (x,y) along (ux,uy), or None."""
    probe = pcbnew.SHAPE_CIRCLE(pcbnew.VECTOR2I(fm(x), fm(y)), 10)
    for p in pads:
        if p.GetNetname() == net and p.IsOnLayer(layer) and p.GetEffectiveShape(layer).Collide(probe, 0):
            bb = p.GetBoundingBox()
            hx, hy = mm(bb.GetWidth()) / 2, mm(bb.GetHeight()) / 2
            c = p.GetPosition()
            off = abs((mm(c.x) - x) * ux + (mm(c.y) - y) * uy)
            return abs(ux) * hx + abs(uy) * hy + off
    return None


plan, skipped, done = {}, [], []
for c in corners:
    x, y = c["x"], c["y"]
    arms = [t for t in segs if t.GetNetname() == c["net"] and board.GetLayerName(t.GetLayer()) == c["layer"]
            and (at(t.GetStart(), x, y) or at(t.GetEnd(), x, y))]
    if len(arms) != 2:
        skipped.append((c, "arms %d" % len(arms))); continue
    legs, info = [], []
    for t in arms:
        far = t.GetEnd() if at(t.GetStart(), x, y) else t.GetStart()
        fx, fy = mm(far.x), mm(far.y)
        L = math.hypot(fx - x, fy - y)
        ux, uy = (fx - x) / L, (fy - y) / L
        if (round(fx, 3), round(fy, 3)) in pts:
            room = L / 2
        else:
            pr = pad_room(c["net"], t.GetLayer(), fx, fy, ux, uy)
            room = L - (pr + 0.3) if pr is not None else L - 0.3
        legs.append(room); info.append((t, ux, uy))
    key = "%g,%g" % (x, y)
    d = over.get(key, min(dmax, *legs))
    if d < 0.4 - 1e-9:
        skipped.append((c, "room %.3f" % min(legs))); continue
    for t, ux, uy in info:
        end = "s" if at(t.GetStart(), x, y) else "e"
        plan.setdefault(t.m_Uuid.AsString(), [t, {}])[1][end] = (x + ux * d, y + uy * d)
    (t1, u1x, u1y), (t2, u2x, u2y) = info
    done.append({"net": c["net"], "layer": c["layer"], "w": min(t1.GetWidth(), t2.GetWidth()),
                 "a": (x + u1x * d, y + u1y * d), "b": (x + u2x * d, y + u2y * d), "d": d, "at": (x, y)})

new = []
for uid, (t, ends) in plan.items():
    s = ends.get("s", (mm(t.GetStart().x), mm(t.GetStart().y)))
    e = ends.get("e", (mm(t.GetEnd().x), mm(t.GetEnd().y)))
    new.append((t.GetNetname(), t.GetLayer(), t.GetWidth(), s, e))
doomed = [t for t, _ in plan.values()]
for t in doomed:
    board.Delete(t)


def add(net, layer, w, a, b):
    tr = pcbnew.PCB_TRACK(board)
    tr.SetStart(pcbnew.VECTOR2I(fm(round(a[0], 4)), fm(round(a[1], 4))))
    tr.SetEnd(pcbnew.VECTOR2I(fm(round(b[0], 4)), fm(round(b[1], 4))))
    tr.SetWidth(w); tr.SetLayer(layer); tr.SetNet(board.GetNetInfo().GetNetItem(net)); board.Add(tr)


for net, layer, w, s, e in new:
    add(net, layer, w, s, e)
for c in done:
    add(c["net"], board.GetLayerID(c["layer"]), c["w"], c["a"], c["b"])
kicad.refill(board)
kicad.save(board, board_path)
json.dump({"chamfered": [(c["net"], c["at"], round(c["d"], 3)) for c in done],
           "skipped": [(c["net"], c["layer"], c["x"], c["y"], why) for c, why in skipped]},
          open(out_path, "w"), indent=1)
sys.stdout.flush()
os._exit(0)
