"""In2 becomes a second solid GND plane (canon 10, owner 2026-10-03): the five In2 power
islands are removed and In1's GND zone is duplicated onto In2. Rail vias stay as feed points
for the rail traces."""
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
in1, in2 = b.GetLayerID("In1.Cu"), b.GetLayerID("In2.Cu")
src = None
for z in list(b.Zones()):
    if z.GetIsRuleArea():
        continue
    if z.IsOnLayer(in2):
        b.Remove(z)
    elif z.IsOnLayer(in1) and z.GetNetname() == "/GND":
        src = z
z2 = pcbnew.ZONE(b)
z2.SetLayer(in2)
z2.SetNet(src.GetNet())
z2.SetOutline(src.Outline().CloneDropTriangulation() if hasattr(src.Outline(), "CloneDropTriangulation") else src.Outline())
for get, put in (("GetLocalClearance", "SetLocalClearance"), ("GetMinThickness", "SetMinThickness"),
                 ("GetPadConnection", "SetPadConnection"), ("GetThermalReliefGap", "SetThermalReliefGap"),
                 ("GetThermalReliefSpokeWidth", "SetThermalReliefSpokeWidth"), ("GetIslandRemovalMode", "SetIslandRemovalMode"),
                 ("GetAssignedPriority", "SetAssignedPriority")):
    if hasattr(src, get) and hasattr(z2, put):
        getattr(z2, put)(getattr(src, get)())
b.Add(z2)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
