"""Pot-row 3V3A supply as a daisy chain through each pin 3 (owner 2026-10-04; canon 133, 132).

East: RV109.3 (fed from the north) -> RV111.3 -> RV113.3 -> RV101.3. Each link leaves a
pin 3 east along the pin row (y 145.5), climbs over the next pot's pins 1-2 at y 143.5
and drops into its pin 3 from the north. West: RV107.3 feeds RV108 below; the lower row
link reaches RV106, a column rises into RV105.3, and links over the top enter RV104.3 and
RV103.3 from the east. Columns to the lower pots pass straight through each pin 3.
"""
import sys

import pcbnew

MM = pcbnew.FromMM
NET = "/3V3A"
b = pcbnew.LoadBoard(sys.argv[1])
TRACKS = list(b.GetTracks())


def pt(x, y):
    return pcbnew.VECTOR2I(MM(x), MM(y))


def near(p, x, y):
    return abs(pcbnew.ToMM(p.x) - x) < 0.02 and abs(pcbnew.ToMM(p.y) - y) < 0.02


def track(net, layer, a, c):
    for t in TRACKS:
        if t.GetClass() == "PCB_TRACK" and t.GetNetname() == net and t.GetLayerName() == layer:
            s, e = t.GetStart(), t.GetEnd()
            if (near(s, *a) and near(e, *c)) or (near(s, *c) and near(e, *a)):
                return t
    raise RuntimeError("no track %s %s %s-%s" % (net, layer, a, c))


def via(net, x, y):
    for t in TRACKS:
        if t.GetClass() == "PCB_VIA" and t.GetNetname() == net and near(t.GetPosition(), x, y):
            return t
    raise RuntimeError("no via %s %s,%s" % (net, x, y))


def set_end(t, old, new):
    if near(t.GetStart(), *old):
        t.SetStart(pt(*new))
    else:
        t.SetEnd(pt(*new))


def add(layer, w, pts):
    net = b.FindNet(NET)
    for a, c in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b)
        t.SetLayer(b.GetLayerID(layer))
        t.SetWidth(MM(w))
        t.SetNet(net)
        t.SetStart(pt(*a))
        t.SetEnd(pt(*c))
        b.Add(t)
        TRACKS.append(t)


KEEP = {((146.0, 140.6), (161.5, 140.6)), ((161.5, 140.6), (163.0, 142.1))}
COLS = (139.0, 163.0, 187.0, 211.0, 235.0)


def mm_(p):
    return (round(pcbnew.ToMM(p.x), 3), round(pcbnew.ToMM(p.y), 3))


def kept(a, c):
    return (a, c) in KEEP or (c, a) in KEEP


doomed = []
for t in TRACKS:
    if t.GetClass() != "PCB_TRACK" or t.GetNetname() != NET or t.GetLayerName() != "B.Cu":
        continue
    a, c = mm_(t.GetStart()), mm_(t.GetEnd())
    xs, ys = (a[0], c[0]), (a[1], c[1])
    in_bus = 66 <= min(xs) and max(xs) <= 236 and 140.5 <= min(ys) and max(ys) <= 147.5
    column = a[0] == c[0] and a[0] in COLS and min(ys) >= 142.0 and max(ys) == 171.5
    if (in_bus or column) and not kept(a, c):
        doomed.append(t)
for item in doomed:
    TRACKS.remove(item)

# East chain

add("B.Cu", 0.3, [(163.0, 142.1), (163.0, 145.5)])
for x in (163.0, 187.0, 211.0):
    n = x + 24.0
    add("B.Cu", 0.3, [(x, 145.5), (n - 8.0, 145.5), (n - 6.0, 143.5), (n - 1.0, 143.5), (n, 144.5), (n, 145.5)])
for x in (163.0, 187.0, 211.0, 235.0):
    add("B.Cu", 0.3, [(x, 145.5), (x, 171.5)])

# West chain, entered from below at RV105

add("B.Cu", 0.3, [(139.0, 145.5), (139.0, 171.5)])
add("B.Cu", 0.3, [(115.0, 171.5), (115.0, 145.5)])
for x in (115.0, 91.0):
    n = x - 24.0
    add("B.Cu", 0.3, [(x, 145.5), (x, 144.5), (x - 1.0, 143.5), (n + 4.0, 143.5), (n + 2.0, 145.5), (n, 145.5)])

for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
print("removed", len(doomed))
