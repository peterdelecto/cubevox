"""Puts U106's reference text on its body between the pad rows, clear of the VA divider and C141."""
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
r = b.FindFootprintByReference("U106").Reference()
r.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(161.0), pcbnew.FromMM(88.0)))
r.SetTextAngle(pcbnew.EDA_ANGLE(0.0, pcbnew.DEGREES_T))
b.Save(sys.argv[1])
