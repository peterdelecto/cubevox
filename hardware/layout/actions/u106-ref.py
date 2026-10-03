"""Moves U106's reference text west of the part, clear of C141's silk (foreman edit has no text op)."""
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
f = b.FindFootprintByReference("U106")
r = f.Reference()
r.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(157.6), pcbnew.FromMM(88.0)))
r.SetTextAngle(pcbnew.EDA_ANGLE(90.0, pcbnew.DEGREES_T))
b.Save(sys.argv[1])
print("U106 ref", pcbnew.ToMM(r.GetPosition().x), pcbnew.ToMM(r.GetPosition().y))
