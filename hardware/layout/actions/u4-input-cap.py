"""U4's input cap straight onto its IN pin (canon 71, 63).

C24 turns 180 degrees and moves 0.35 mm so C24.1 sits on U4.1's row: U4.1 -> C24.1 is
one 2.2 mm trace, the 5V feed lands on C24.1 from the east, and EN keeps its tie from
IN under the body. C24's GND via moves north of its new GND pad.
"""
import sys

import pcbnew

MM = pcbnew.FromMM
NET = "/5V"
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


gone = [(NET, (155.0, 50.55), (155.7, 50.55)), (NET, (155.7, 50.55), (156.1, 50.95)),
        (NET, (156.1, 50.95), (156.1, 59.375)), (NET, (152.8, 50.55), (155.0, 50.55)),
        ("/GND", (155.0, 52.1), (155.0, 52.95))]
doomed = [track(n, "F.Cu", a, c) for n, a, c in gone]
doomed.append(via("/GND", 155.0, 52.95))
for item in doomed:
    TRACKS.remove(item)

c24 = b.FindFootprintByReference("C24")
c24.Rotate(pt(155.0, 51.325), pcbnew.EDA_ANGLE(180, pcbnew.DEGREES_T))
c24.Move(pt(0, 0.35))
pads = {p.GetNumber(): (round(pcbnew.ToMM(p.GetPosition().x), 3), round(pcbnew.ToMM(p.GetPosition().y), 3)) for p in c24.Pads()}
print("C24 pads", pads)

add("F.Cu", 0.4, [(152.8, 52.45), (155.0, 52.45)])
add("F.Cu", 0.4, [(156.1, 59.375), (156.1, 52.85), (155.7, 52.45), (155.0, 52.45)])
v = pcbnew.PCB_VIA(b)
v.SetPosition(pt(155.0, 49.85))
v.SetWidth(MM(0.6))
v.SetDrill(MM(0.3))
v.SetNet(b.FindNet("/GND"))
b.Add(v)
g = pcbnew.PCB_TRACK(b)
g.SetLayer(pcbnew.F_Cu); g.SetWidth(MM(0.3)); g.SetNet(b.FindNet("/GND"))
g.SetStart(pt(155.0, 50.9)); g.SetEnd(pt(155.0, 49.85))
b.Add(g)

for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
