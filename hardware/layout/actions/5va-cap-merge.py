"""Feed C118 and C134 on F.Cu from their partners instead of 13 mm B.Cu loops (canon 134, 71).

U104.8 -> C118.1 straight south (C118 0.1 mm west onto pin 8's axis); C134.1 -> R20.1
straight east (C134 0.125 mm north onto R20.1's axis). GND vias follow the caps. Each
cap's via and its B.Cu loop go.
"""
import sys

import pcbnew

MM = pcbnew.FromMM
NET = "/5VA"
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


# Caps onto their partners' axes; GND vias and stubs follow

b.FindFootprintByReference("C118").Move(pt(-0.1, 0))
via("/GND", 202.5, 85.5).SetPosition(pt(202.4, 85.5))
t = track("/GND", "F.Cu", (202.5, 84.7), (202.5, 85.5))
t.SetStart(pt(202.4, 84.7)); t.SetEnd(pt(202.4, 85.5))
b.FindFootprintByReference("C134").Move(pt(0, -0.125))
via("/GND", 132.65, 98.7).SetPosition(pt(132.65, 98.575))
t = track("/GND", "F.Cu", (133.5, 98.7), (132.65, 98.7))
t.SetStart(pt(133.5, 98.575)); t.SetEnd(pt(132.65, 98.575))

# Look everything up before any Remove: a Remove invalidates the other proxies.

loops = [[(201.1, 78.9), (201.1, 81.4)], [(201.6, 78.4), (201.1, 78.9)], [(204.7, 78.4), (201.6, 78.4)],
         [(205.2, 78.9), (204.7, 78.4)], [(205.2, 82.0), (205.2, 78.9)], [(204.7, 82.5), (205.2, 82.0)],
         [(202.5, 82.5), (204.7, 82.5)],
         [(133.5, 96.5), (132.0, 96.5)], [(132.0, 96.5), (131.5, 96.0)], [(131.5, 96.0), (131.5, 92.9)],
         [(131.5, 92.9), (132.0, 92.4)], [(132.0, 92.4), (135.1, 92.4)], [(135.1, 92.4), (135.65, 92.95)],
         [(135.65, 92.95), (135.65, 96.375)]]
doomed = [track(NET, "B.Cu", a, c) for a, c in loops]
doomed += [track(NET, "F.Cu", (202.5, 83.3), (202.5, 82.5)), track(NET, "F.Cu", (133.5, 97.3), (133.5, 96.5))]
doomed += [via(NET, 202.5, 82.5), via(NET, 133.5, 96.5)]
for item in doomed:
    TRACKS.remove(item)

# Pin 8 -> C118.1 and C134.1 -> R20.1 on F.Cu

add("F.Cu", 0.3, [(202.4, 81.4), (202.4, 83.3)])
add("F.Cu", 0.3, [(133.5, 97.175), (135.65, 97.175)])

for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
