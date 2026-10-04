"""5V pad entries onto pad centres (canon 132).

The buck output node (L101.2, C114.1, C115.1, C19.1, trunk via) becomes a solid 5V
pour: its pads are too large and too offset for centred traces. K102's via moves onto
K102.1's axis; C20.1 -> U3.3 -> U3.2 and U103.1 enter from pad centres.
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


# Look everything up before any Remove: a Remove invalidates the other proxies.

gone = [("F.Cu", (98.5, 44.4), (99.7, 44.4)), ("F.Cu", (101.3, 44.4), (102.7, 44.4)),
        ("F.Cu", (104.3, 45.1), (105.55, 45.1)), ("F.Cu", (105.55, 47.1), (105.55, 47.65)),
        ("F.Cu", (75.2, 74.01), (75.1, 74.11)), ("F.Cu", (75.1, 74.11), (75.1, 76.4)),
        ("F.Cu", (75.1, 76.4), (74.0, 77.5)), ("F.Cu", (74.0, 77.5), (72.77, 77.5)),
        ("F.Cu", (74.75, 73.05), (74.25, 73.05)), ("B.Cu", (74.25, 73.05), (80.85, 73.05)),
        ("F.Cu", (92.85, 59.8), (94.25, 59.8)), ("F.Cu", (94.25, 59.8), (95.2, 59.8)),
        ("F.Cu", (93.2, 53.15), (93.2, 53.65))]
doomed = [track(NET, layer, a, c) for layer, a, c in gone]
for item in doomed:
    TRACKS.remove(item)

# K102: via onto K102.1's axis; north run extends, east run moves to the via's row

via(NET, 74.25, 73.05).SetPosition(pt(74.25, 74.01))
set_end(track(NET, "B.Cu", (74.25, 68.65), (74.25, 73.05)), (74.25, 73.05), (74.25, 74.01))
add("B.Cu", 0.5, [(74.25, 74.01), (79.89, 74.01), (80.85, 73.05)])
add("F.Cu", 0.4, [(75.2, 74.01), (74.25, 74.01)])
add("F.Cu", 0.4, [(75.2, 74.01), (75.2, 76.4), (74.1, 77.5), (72.77, 77.5)])

# C20.1 -> U3.3 -> U3.2 from pad centres

add("F.Cu", 0.4, [(92.0, 59.425), (93.3, 59.425), (93.675, 59.8), (94.55, 59.8), (95.5, 59.8)])

# U103.1: via onto the pad's axis

via(NET, 93.2, 53.15).SetPosition(pt(93.05, 53.15))
set_end(track(NET, "B.Cu", (92.25, 53.15), (93.2, 53.15)), (93.2, 53.15), (93.05, 53.15))
set_end(track(NET, "B.Cu", (93.2, 53.15), (94.45, 53.15)), (93.2, 53.15), (93.05, 53.15))
add("F.Cu", 0.4, [(93.05, 53.15), (93.05, 54.1)])

# Buck output node: L101.2, C114.1, C115.1, C19.1 and the trunk via on one 5V pour

gnd = [z for z in b.Zones() if not z.GetIsRuleArea() and z.GetNetname() == "/GND" and z.IsOnLayer(pcbnew.In1_Cu)][0]
z = pcbnew.ZONE(b)
z.SetLayer(pcbnew.F_Cu)
z.SetNet(b.FindNet(NET))
z.SetZoneName("5V buck output")
z.SetLocalClearance(gnd.GetLocalClearance())
z.SetMinThickness(gnd.GetMinThickness())
z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
z.SetIslandRemovalMode(gnd.GetIslandRemovalMode())
outline = z.Outline()
outline.NewOutline()
for x, y in [(97.1, 43.6), (107.6, 43.6), (107.6, 48.2), (105.2, 48.2), (105.2, 45.4), (97.1, 45.4)]:
    outline.Append(MM(x), MM(y))
b.Add(z)

for item in doomed:
    b.Remove(item)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
