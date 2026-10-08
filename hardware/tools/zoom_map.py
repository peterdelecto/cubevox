import pcbnew, sys
from PIL import Image, ImageDraw, ImageFont
import os
b=pcbnew.LoadBoard(os.environ.get('BOARD','hardware/cubevox.kicad_pcb'))
x0,y0,x1,y1=[float(v) for v in sys.argv[1:5]]; out=sys.argv[5]
S=int(sys.argv[6]) if len(sys.argv)>6 else 40  # px per mm
W,H=int((x1-x0)*S),int((y1-y0)*S)
im=Image.new('RGB',(W,H),(20,20,20)); d=ImageDraw.Draw(im)
P=lambda x,y:((x-x0)*S,(y-y0)*S)
F,B=pcbnew.F_Cu,pcbnew.B_Cu
def col(l,net=''):
    if 'TOGGLE' in net: return (255,255,0)
    if 'MUX_S' in net: return (0,255,255)
    return (220,60,60) if l==F else (70,110,255)
# grid every 1 mm
for gx in range(int(x0),int(x1)+1): d.line([P(gx,y0),P(gx,y1)],fill=(40,40,40))
for gy in range(int(y0),int(y1)+1): d.line([P(x0,gy),P(x1,gy)],fill=(40,40,40))
for fp in b.GetFootprints():
    for p in fp.Pads():
        c=p.GetPosition(); px,py=c.x/1e6,c.y/1e6
        if not(x0-3<px<x1+3 and y0-3<py<y1+3): continue
        sz=p.GetSize(); sx,sy=sz.x/1e6,sz.y/1e6
        if abs(fp.GetOrientationDegrees())%180==90 or abs(p.GetOrientationDegrees())%180==90: sx,sy=sy,sx
        onF=p.IsOnLayer(F); onB=p.IsOnLayer(B)
        fill=(140,140,140) if (onF and onB) else ((180,80,80) if onF else (80,100,200))
        d.rectangle([P(px-sx/2,py-sy/2),P(px+sx/2,py+sy/2)],outline=fill)
    c=fp.GetPosition(); px,py=c.x/1e6,c.y/1e6
    if x0<px<x1 and y0<py<y1: d.text(P(px,py),fp.GetReference(),fill=(200,200,200))
for t in b.GetTracks():
    if t.GetClass()=='PCB_VIA':
        c=t.GetPosition(); px,py=c.x/1e6,c.y/1e6
        if x0<px<x1 and y0<py<y1:
            r=t.GetWidth()/2e6; d.ellipse([P(px-r,py-r),P(px+r,py+r)],outline=(255,255,255))
        continue
    s,e=t.GetStart(),t.GetEnd()
    sx,sy,ex,ey=s.x/1e6,s.y/1e6,e.x/1e6,e.y/1e6
    if max(sx,ex)<x0 or min(sx,ex)>x1 or max(sy,ey)<y0 or min(sy,ey)>y1: continue
    d.line([P(sx,sy),P(ex,ey)],fill=col(t.GetLayer(),t.GetNetname()),width=max(1,int(t.GetWidth()/1e6*S)))
# zone outlines not drawn; label axes
for gx in range(int(x0),int(x1)+1,5): d.text(P(gx,y0),str(gx),fill=(255,255,255))
for gy in range(int(y0),int(y1)+1,5): d.text(P(x0,gy),str(gy),fill=(255,255,255))
im.save(out); print('saved',out,W,H)
