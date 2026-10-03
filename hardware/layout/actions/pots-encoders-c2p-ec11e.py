"""Pots -> RK09D1130C2P (C361173), encoders -> EC11E15244B2 / EC11E15204A3 (C470754 / C470710).
Pads are identical (same Alps drawings), so only FPID, 3D model, value and part fields change."""
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
M = "${KIPRJMOD}/cubevox.3dshapes/"
def setf(f, name, text):
    hit = [fl for fl in f.GetFields() if fl.GetName() == name]
    if hit: hit[0].SetText(text)
    else:
        f.SetField(name, text); [fl for fl in f.GetFields() if fl.GetName() == name][0].SetVisible(False)
def swap(f, fpid, model, lcsc, mpn, value=None):
    f.SetFPIDAsString(fpid)
    f.Models()[0].m_Filename = M + model
    if value: f.SetValue(value)
    for k in ("LCSC", "LCSC Part"): setf(f, k, lcsc)
    setf(f, "MPN", mpn)
n = 0
for f in b.GetFootprints():
    r = f.GetReference()
    if r.startswith("RV"):
        swap(f, "cubevox:RES-ADJ-TH_RK09D1130C2P", "RK09D1130C2P_from_alps_c1b.step", "C361173", "RK09D1130C2P"); n += 1
    elif r == "ENC101":
        swap(f, "cubevox:SW-TH_EC11E", "EC11E15244B2_from_alps_ec11n.step", "C470754", "EC11E15244B2", "EC11E15244B2"); n += 1
    elif r in ("ENC102", "ENC103"):
        swap(f, "cubevox:SW-TH_EC11E", "EC11E15204A3_from_alps_ec11n.step", "C470710", "EC11E15204A3", "EC11E15204A3"); n += 1
b.Save(sys.argv[1]); print("swapped", n)
