"""Pot-row 3V3A supply as a clean tree (canon 133, 132, 136).

Straight buses at y 147.4 (west half, fed from x 139) and y 143.5 (east half, fed at
x 163). Each pin 3 joins its bus by one straight stub; where an upper and a lower pot
share a column the vertical passes through pin 3. Replaces overlapping stubs, V-forks
and a bus that wandered between y 143.5 and 143.7.
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

# West half: bus y 147.4, fed through RV107.3's column

add("B.Cu", 0.3, [(67.0, 145.5), (67.0, 146.9), (67.5, 147.4), (91.0, 147.4), (115.0, 147.4), (139.0, 147.4)])
add("B.Cu", 0.3, [(91.0, 145.5), (91.0, 147.4)])
add("B.Cu", 0.3, [(115.0, 145.5), (115.0, 147.4)])
add("B.Cu", 0.3, [(139.0, 145.5), (139.0, 147.4), (139.0, 171.5)])

# East half: bus y 143.5 from the x 163 feed

add("B.Cu", 0.3, [(163.0, 142.1), (163.0, 143.5), (163.0, 145.5), (163.0, 171.5)])
add("B.Cu", 0.3, [(163.0, 143.5), (187.0, 143.5), (211.0, 143.5), (234.5, 143.5), (235.0, 144.0),
                  (235.0, 145.5), (235.0, 171.5)])
add("B.Cu", 0.3, [(187.0, 143.5), (187.0, 145.5), (187.0, 171.5)])
add("B.Cu", 0.3, [(211.0, 143.5), (211.0, 145.5), (211.0, 171.5)])

for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
print("removed", len(doomed))
