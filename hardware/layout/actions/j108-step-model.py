"""J108 takes the PJ-611E STEP model the library footprint already uses (offset and rotation
checked against its courtyard), so STEP exports include the output jack."""
import sys, pcbnew
b=pcbnew.LoadBoard(sys.argv[1]); f=b.FindFootprintByReference('J108')
m=f.Models()[0]
m.m_Filename='${KIPRJMOD}/cubevox.3dshapes/AUDIO-TH_PJ-611E_1_easyeda.step'
m.m_Offset=pcbnew.VECTOR3D(10.69,-1.4,6.48); m.m_Rotation=pcbnew.VECTOR3D(0,0,270)
b.Save(sys.argv[1])
