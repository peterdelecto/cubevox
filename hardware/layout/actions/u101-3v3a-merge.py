"""Merge U101's 3V3A decoupling onto one via (canon 71, 134).

Chain C28.1 -> via -> U101.24 -> C27.1 on F.Cu. C28 moves 0.08 mm south and C27
0.06 mm west so the chain is straight; their GND vias follow. Pin 24's via and its
B.Cu branch to the (147.3, 106.28) hub go, as does C27's via; the south-east branch
now leaves C28's via.
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


# Caps and their GND vias

b.FindFootprintByReference("C28").Move(pt(0, 0.08))
via("/GND", 139.375, 131.0).SetPosition(pt(139.375, 131.08))
t = track("/GND", "F.Cu", (140.225, 131.0), (139.375, 131.0))
t.SetStart(pt(140.225, 131.08)); t.SetEnd(pt(139.375, 131.08))
b.FindFootprintByReference("C27").Move(pt(-0.06, 0))
via("/GND", 144.5, 135.625).SetPosition(pt(144.44, 135.625))
t = track("/GND", "F.Cu", (144.5, 134.775), (144.5, 135.625))
t.SetStart(pt(144.44, 134.775)); t.SetEnd(pt(144.44, 135.625))

# Removed: pin 24's via and branch, C27's via and links

gone = [("F.Cu", (144.44, 131.08), (144.44, 131.634)),
        ("B.Cu", (144.44, 131.634), (146.766, 131.634)),
        ("B.Cu", (146.766, 131.634), (147.3, 131.1)),
        ("B.Cu", (147.3, 131.1), (147.3, 106.28)),
        ("F.Cu", (144.5, 133.225), (144.5, 132.375)),
        ("B.Cu", (144.5, 132.375), (143.075, 132.375)),
        ("B.Cu", (143.075, 132.375), (142.625, 131.925)),
        ("B.Cu", (142.625, 131.925), (142.625, 131.0)),
        ("B.Cu", (144.5, 133.2), (144.5, 132.375)),
        ("B.Cu", (147.8, 136.5), (144.5, 133.2)),
        ("B.Cu", (150.9, 136.5), (147.8, 136.5)),
        ("B.Cu", (141.7, 131.0), (142.625, 131.0))]
# Look everything up before any Remove: a Remove invalidates the other proxies.

doomed = [track(NET, layer, a, c) for layer, a, c in gone]
doomed += [via(NET, 144.44, 131.634), via(NET, 144.5, 132.375)]
for item in doomed:
    TRACKS.remove(item)

# C28's via onto pin 24's axis

via(NET, 142.625, 131.0).SetPosition(pt(142.625, 131.08))
set_end(track(NET, "B.Cu", (142.625, 110.175), (142.625, 131.0)), (142.625, 131.0), (142.625, 131.08))
t = track(NET, "B.Cu", (139.0, 133.7), (141.7, 131.0))
t.SetStart(pt(139.0, 133.62)); t.SetEnd(pt(141.54, 131.08))
for t in TRACKS:
    if (t.GetClass() == "PCB_TRACK" and t.GetNetname() == NET and t.GetLayerName() == "B.Cu"
            and (near(t.GetStart(), 139.0, 133.7) or near(t.GetEnd(), 139.0, 133.7))):
        set_end(t, (139.0, 133.7), (139.0, 133.62))
t = track(NET, "F.Cu", (141.775, 131.0), (142.625, 131.0))
t.SetStart(pt(141.775, 131.08)); t.SetEnd(pt(142.625, 131.08))
add("B.Cu", 0.3, [(141.54, 131.08), (142.625, 131.08)])

# New copper

add("F.Cu", 0.3, [(142.625, 131.08), (144.44, 131.08), (144.44, 133.225)])
add("B.Cu", 0.3, [(142.625, 131.08), (142.625, 132.5), (146.625, 136.5), (150.9, 136.5)])

for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
