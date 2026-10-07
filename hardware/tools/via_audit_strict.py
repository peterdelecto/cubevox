"""Strict via audit: a GND via with no outer-layer track end or same-net pad on it is a return via only if a critical-net signal via lies within 2 mm; otherwise it is unnecessary (canon 17/20, owner 2026-10-07). Run with the KiCad python."""
import pcbnew, collections, sys
p='/Users/j/Documents/Claude/Projects/cubevox/hardware/cubevox.kicad_pcb'
b=pcbnew.LoadBoard(p)
mm=lambda v:v/1e6
tracks=[t for t in b.GetTracks() if t.GetClass()=='PCB_TRACK']
vias=[t for t in b.GetTracks() if t.GetClass()=='PCB_VIA']
pads=[(f.GetReference(),pd) for f in b.GetFootprints() for pd in f.Pads()]
TOL=int(0.05e6)
def near(a,c): return abs(a.x-c.x)<=TOL and abs(a.y-c.y)<=TOL
rows=[]
for v in vias:
    c=v.GetPosition(); lay=collections.Counter()
    for t in tracks:
        if near(t.GetStart(),c) or near(t.GetEnd(),c): lay[b.GetLayerName(t.GetLayer())]+=1
    padhit=[r+'.'+pd.GetNumber() for r,pd in pads if pd.HitTest(c) and pd.GetNetname()==v.GetNetname()]
    outer=lay['F.Cu']+lay['B.Cu']+len(padhit)
    rows.append((v.GetNetname(),mm(c.x),mm(c.y),dict(lay),padhit,outer))
orph=[r for r in rows if r[5]==0]
one=[r for r in rows if r[5]==1 and r[0]!='/GND']
gnd1=[r for r in rows if r[5]==1 and r[0]=='/GND']
print('vias',len(vias),'orphans',len(orph),'signal one-sided',len(one),'GND one-sided(return)',len(gnd1))
print('-- ORPHANS (no outer copper)')
for r in sorted(orph): print('  %-14s (%.2f,%.2f)'%(r[0],r[1],r[2]))
print('-- SIGNAL ONE-SIDED')
for r in sorted(one): print('  %-14s (%.2f,%.2f) %s %s'%(r[0],r[1],r[2],r[3],r[4]))
print('==== return-via check')
import math
sig=[(mm(v.GetPosition().x),mm(v.GetPosition().y),v.GetNetname()) for v in vias if v.GetNetname()!='/GND']
true_orph=[]; ret=[]
for r in orph:
    d,nn=min(((math.hypot(r[1]-x,r[2]-y),n) for x,y,n in sig), key=lambda t:t[0])
    (ret if d<=2.0 else true_orph).append((r,d,nn))
print('return vias (signal via within 2 mm):',len(ret))
print('TRUE ORPHANS:',len(true_orph))
for r,d,nn in sorted(true_orph,key=lambda t:t[1]): print('  (%.2f,%.2f) nearest signal via %.1f mm (%s)'%(r[1],r[2],d,nn))
print('-- return vias farther than 1.2 mm (loose pairing, check):')
for r,d,nn in sorted(ret,key=lambda t:-t[1]):
    if d>1.2: print('  (%.2f,%.2f) %.2f mm to %s'%(r[1],r[2],d,nn))
print('==== return vias by partner net')
c=collections.Counter(nn.split('/')[-1] for r,d,nn in ret+true_orph)
for n,k in sorted(c.items(), key=lambda t:-t[1]): print('  %2d  %s'%(k,n))
