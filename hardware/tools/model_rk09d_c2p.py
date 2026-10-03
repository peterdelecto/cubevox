"""RK09D1130C2P from Alps's RK09D1130C1B STEP: same drawing No. 4, shaft LM1 25 instead of 20.
Cut the uniform D-flat section at z 15.5, lift the top 5 mm, fill with D-section slices."""
import cadquery as cq
SRC="/Users/j/Documents/Claude/Projects/cubevox/hardware/cubevox.3dshapes/RK09D1130C1B_alps_aligned.step"
OUT="/Users/j/Documents/Claude/Projects/cubevox/hardware/cubevox.3dshapes/RK09D1130C2P_from_alps_c1b.step"
s=cq.importers.importStep(SRC).val()
box=lambda z0,z1: cq.Solid.makeBox(200,200,z1-z0,cq.Vector(-100,-100,z0))
CUT,EXT=15.5,5.0
lower=s.intersect(box(-50,CUT))
upper=s.intersect(box(CUT,50)).translate(cq.Vector(0,0,EXT))
slice_=s.intersect(box(13.6,16.1))          # 2.5 mm of uniform D-flat shaft
f1=slice_.translate(cq.Vector(0,0,CUT-13.6))
f2=slice_.translate(cq.Vector(0,0,CUT+2.5-13.6))
out=lower.fuse(f1).fuse(f2).fuse(upper).clean()
cq.exporters.export(cq.Workplane().add(out),OUT)
print("wrote",OUT)
