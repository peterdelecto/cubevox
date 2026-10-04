"""C101's 3V3A via onto the B.Cu trunk's row so every leg leaves it on an axis (canon 132)."""
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


doomed = [track(NET, "B.Cu", (147.3, 100.02), (147.82, 99.5))]
for item in doomed:
    TRACKS.remove(item)
via(NET, 147.3, 100.02).SetPosition(pt(147.3, 99.5))
set_end(track(NET, "B.Cu", (147.3, 106.28), (147.3, 100.02)), (147.3, 100.02), (147.3, 99.5))
set_end(track(NET, "F.Cu", (147.3, 100.87), (147.3, 100.02)), (147.3, 100.02), (147.3, 99.5))
set_end(track(NET, "B.Cu", (147.82, 99.5), (153.4, 99.5)), (147.82, 99.5), (147.3, 99.5))
for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
