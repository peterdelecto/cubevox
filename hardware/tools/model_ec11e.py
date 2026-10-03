"""EC11E 24.5 mm models from Alps's EC11N (L 19.5) STEPs: same body, bushing and PCB layout
(Alps EC11 drawings 4/5 vs 6/7); shaft lengthened 5 mm inside the uniform D-flat section."""
import cadquery as cq
D="/Users/j/Documents/Claude/Projects/cubevox/hardware/cubevox.3dshapes/"
box=lambda z0,z1: cq.Solid.makeBox(200,200,z1-z0,cq.Vector(-100,-100,z0))
for src,dst in (("EC11N1525404_alps_aligned.step","EC11E15244B2_from_alps_ec11n.step"),
                ("EC11N1520401_alps_aligned.step","EC11E15204A3_from_alps_ec11n.step")):
    s=cq.importers.importStep(D+src).val()
    CUT,EXT=16.5,5.0
    lower=s.intersect(box(-50,CUT)); upper=s.intersect(box(CUT,50)).translate(cq.Vector(0,0,EXT))
    sl=s.intersect(box(15.2,17.7))
    out=lower.fuse(sl.translate(cq.Vector(0,0,CUT-15.2))).fuse(sl.translate(cq.Vector(0,0,CUT+2.5-15.2))).fuse(upper).clean()
    cq.exporters.export(cq.Workplane().add(out),D+dst); print("wrote",dst)
