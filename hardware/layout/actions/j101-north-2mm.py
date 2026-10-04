"""Move J101 (Neutrik combo) 2 mm north (owner 2026-10-04) and carry its traces along.

Pad end legs are vertical, so they stretch. The TRS_T run (y 45.83) and the XLR_2 run
(y 51.4, threaded between the pad rows) shift north with the pads; their far corners move
up too so the bends stay at 45 degrees.
"""
import sys

import pcbnew

DY = pcbnew.FromMM(-2.0)
b = pcbnew.LoadBoard(sys.argv[1])
fp = b.FindFootprintByReference("J101")
pads = [p.GetPosition() for p in fp.Pads() if p.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH]
fp.Move(pcbnew.VECTOR2I(0, DY))


def near(a, b):
    return abs(a.x - b.x) < pcbnew.FromMM(0.02) and abs(a.y - b.y) < pcbnew.FromMM(0.02)


# Run endpoints that shift with the jack: (net, x range, y range) in mm.

SHIFT = [("/TRS_T", (184.0, 213.6), (45.8, 47.5)),
         ("/XLR_2", (182.3, 207.1), (50.9, 52.2))]
for t in b.GetTracks():
    if t.GetClass() != "PCB_TRACK":
        continue
    net = t.GetNetname()
    for get, put in ((t.GetStart, t.SetStart), (t.GetEnd, t.SetEnd)):
        e = get()
        x, y = pcbnew.ToMM(e.x), pcbnew.ToMM(e.y)
        if any(net == n and xr[0] <= x <= xr[1] and yr[0] <= y <= yr[1] for n, xr, yr in SHIFT):
            put(pcbnew.VECTOR2I(e.x, e.y + DY))
        elif any(near(e, p) for p in pads):
            put(pcbnew.VECTOR2I(e.x, e.y + DY))
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
