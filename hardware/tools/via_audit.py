"""Classify every via: layer change, pad GND via, signal return, or orphan.

Usage: kicad-python tools/via_audit.py board.kicad_pcb [--all]
"""
import sys, pcbnew
M = pcbnew.ToMM
b = pcbnew.LoadBoard(sys.argv[1]); show_all = '--all' in sys.argv
tracks = [t for t in b.GetTracks() if t.Type() == pcbnew.PCB_TRACE_T]
vias = [t for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
pads = [(f.GetReference(), p) for f in b.GetFootprints() for p in f.Pads()]
sig_vias = [v for v in vias if v.GetNetname() != '/GND']
def touches(pt, p, r):
    return abs(M(pt.x - p.x)) <= r and abs(M(pt.y - p.y)) <= r

def reach_pad(p, net, depth=4, seen=()):
    # Follow short GND stubs (<=2.5 mm each) up to `depth` segments to a pad.
    found = []
    for t in tracks:
        if t.GetNetname() != net or M(t.GetLength()) > 2.5: continue
        for a, e in ((t.GetStart(), t.GetEnd()), (t.GetEnd(), t.GetStart())):
            if not touches(a, p, 0.3) or (e.x, e.y) in seen: continue
            found += [r for r, pd in pads if pd.GetNetname() == net and pd.HitTest(e)]
            if not found and depth > 1: found += reach_pad(e, net, depth - 1, seen + ((a.x, a.y),))
    return found
rows = []
for v in vias:
    p = v.GetPosition(); net = v.GetNetname()
    layers = {t.GetLayerName() for t in tracks if t.GetNetname() == net and (touches(t.GetStart(), p, 0.3) or touches(t.GetEnd(), p, 0.3))}
    pad_hit = [r for r, pd in pads if pd.GetNetname() == net and pd.HitTest(p)]
    kinds = []
    if {'F.Cu', 'B.Cu'} <= layers or (pad_hit and layers): kinds.append('layer-change')
    if net == '/GND':
        stub_pad = reach_pad(p, '/GND')
        if stub_pad or pad_hit: kinds.append('pad-gnd:' + ','.join(stub_pad or pad_hit))
        near = [s for s in sig_vias if abs(M(s.GetPosition().x - p.x)) <= 2 and abs(M(s.GetPosition().y - p.y)) <= 2]
        if near: kinds.append('return:' + ','.join(s.GetNetname().split('/')[-1] for s in near))
    if not kinds: kinds.append('ORPHAN layers=' + ','.join(sorted(layers)))
    rows.append((kinds[0].startswith('ORPHAN'), net, round(M(p.x), 2), round(M(p.y), 2), ' '.join(kinds)))
orph = [r for r in rows if r[0]]
print(f'vias {len(vias)}  orphan {len(orph)}')
for r in sorted(orph if not show_all else rows, key=lambda r: (r[1], r[2])): print(f'  {r[1]:28s} {r[2]:7.2f} {r[3]:7.2f}  {r[4]}')
