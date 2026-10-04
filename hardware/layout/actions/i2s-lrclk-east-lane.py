"""I2S_LRCLK's east lane from y 94.75 to 94.85: 0.7 mm centres to I2S_DATA, clear of 3W (canon 51)."""
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


gone = [(LRCLK, (103.0, 94.35), (103.4, 94.75)), (LRCLK, (103.4, 94.75), (126.4, 94.75)),
        (LRCLK, (126.4, 94.75), (126.8, 95.15)), (LRCLK, (126.8, 95.15), (126.8, 98.0))]
doomed = [track(n, "B.Cu", a, c) for n, a, c in gone]
for item in doomed:
    TRACKS.remove(item)
add("B.Cu", 0.2, [(103.0, 94.35), (103.5, 94.85), (126.4, 94.85), (126.8, 95.25), (126.8, 98.0)], LRCLK)
for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
