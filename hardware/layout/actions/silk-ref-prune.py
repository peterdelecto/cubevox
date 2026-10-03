"""One visible designator per part. Silk-reference parts (U, RV, SW, J) keep the silk field and
drop the Fab copy; every other part keeps the centred Fab ${REFERENCE} text and hides the field."""
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
silk = {pcbnew.F_SilkS, pcbnew.B_SilkS}
drop = []
for fp in list(b.GetFootprints()):
    ref = fp.Reference()
    fab = [t for t in list(fp.GraphicalItems())
           if t.GetClass() == "PCB_TEXT" and "REFERENCE" in t.Cast().GetText()]
    if ref.GetLayer() in silk:
        drop += [(fp, t) for t in fab]
    elif fab:
        ref.SetVisible(False)
for fp, t in drop:
    fp.Remove(t)
b.Save(sys.argv[1])
print("dropped", len(drop))
