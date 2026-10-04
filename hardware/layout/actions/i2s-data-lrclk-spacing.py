"""Space I2S_DATA from I2S_LRCLK to 3W and take LRCLK out from under Y1 (canon 51, 35).

West of x 103: DATA y 93.6 (its via 0.15 mm north onto that row), LRCLK y 94.35, clear
of Y1's courtyard. East: LRCLK steps to y 94.75 after the GND via at x 102.1, then DATA
steps to y 94.15 before the GND via at x 106.85, keeping 0.6 mm centre spacing.
"""
import sys

import pcbnew

MM = pcbnew.FromMM
DATA = "/h7core_block/I2S_DATA"
LRCLK = "/h7core_block/I2S_LRCLK"
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


def add(layer, w, pts, NET):
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


gone = [(DATA, (92.25, 93.45), (92.25, 93.875)), (DATA, (92.25, 93.875), (92.525, 94.15)),
        (DATA, (92.525, 94.15), (129.6, 94.15)),
        (LRCLK, (90.5, 93.9), (90.5, 94.4)), (LRCLK, (90.5, 94.4), (90.75, 94.65)),
        (LRCLK, (90.75, 94.65), (126.4, 94.65)), (LRCLK, (126.4, 94.65), (126.8, 95.05)),
        (LRCLK, (126.8, 95.05), (126.8, 98.0))]
doomed = [track(n, "B.Cu", a, c) for n, a, c in gone]
for item in doomed:
    TRACKS.remove(item)
via(DATA, 92.25, 93.45).SetPosition(pt(92.25, 93.6))
set_end(track(DATA, "F.Cu", (92.25, 93.05), (92.25, 93.45)), (92.25, 93.45), (92.25, 93.6))
add("B.Cu", 0.2, [(92.25, 93.6), (104.0, 93.6), (104.55, 94.15), (129.6, 94.15)], DATA)
add("B.Cu", 0.2, [(90.5, 93.9), (91.2, 93.9), (91.65, 94.35), (103.0, 94.35), (103.4, 94.75),
                  (126.4, 94.75), (126.8, 95.15), (126.8, 98.0)], LRCLK)
for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
