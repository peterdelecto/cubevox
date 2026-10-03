"""Sets C164's BOM fields after add_footprint + netlist sync (foreman edit has no field op)."""
import sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
f = b.FindFootprintByReference("C164")
f.SetFPIDAsString("Capacitor_SMD:C_0805_2012Metric")
f.SetValue("10uF")
f.Value().SetVisible(False)
f.SetField("LCSC", "C15850")
next(fl for fl in f.GetFields() if fl.GetName() == "LCSC").SetVisible(False)
b.Save(sys.argv[1])
print("C164", f.GetValue(), f.GetFieldText("LCSC"), f.GetFPIDAsString())
