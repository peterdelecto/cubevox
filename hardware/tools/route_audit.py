"""Read-only routing audit: stray traces, stray/unnecessary vias, via hops that F.Cu could carry.

Usage: kicad-python audit_routes.py board.kicad_pcb
"""
import sys, pcbnew, collections
M = pcbnew.ToMM; FMM = pcbnew.FromMM
b = pcbnew.LoadBoard(sys.argv[1])
F, B = pcbnew.F_Cu, pcbnew.B_Cu
tracks = [t for t in b.GetTracks() if t.Type() == pcbnew.PCB_TRACE_T]
vias = [t for t in b.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
pads = [p for f in b.GetFootprints() for p in f.Pads()]
CLR = FMM(0.2)
def P(pt): return f'({M(pt.x):.2f},{M(pt.y):.2f})'
def same(a, c): return abs(a.x - c.x) <= 50 and abs(a.y - c.y) <= 50   # 0.05 um
def nn(x): return x.GetNetname()

# ---- endpoint connectivity ------------------------------------------------
def end_connected(t, pt):
    L = t.GetLayer()
    for u in tracks:
        if u is t or u.GetLayer() != L or nn(u) != nn(t): continue
        if same(u.GetStart(), pt) or same(u.GetEnd(), pt): return 'track'
        if u.GetEffectiveShape().Collide(pcbnew.SHAPE_CIRCLE(pt, 10), 0): return 'mid-track'
    for v in vias:
        if nn(v) == nn(t) and v.IsOnLayer(L) and v.GetEffectiveShape(L).Collide(pcbnew.SHAPE_CIRCLE(pt, 10), 0): return 'via'
    for p in pads:
        if nn(p) == nn(t) and p.IsOnLayer(L) and p.GetEffectiveShape(L).Collide(pcbnew.SHAPE_CIRCLE(pt, 10), 0): return 'pad'
    return None

print('== 1. stray traces ==')
n1 = 0
for t in tracks:
    issues = []
    if t.GetLength() == 0: issues.append('zero-length')
    if t.GetNetCode() <= 0: issues.append('no-net')
    for pt, nm in ((t.GetStart(), 'start'), (t.GetEnd(), 'end')):
        c = end_connected(t, pt)
        if c is None: issues.append(f'dangling-{nm}')
        elif c == 'mid-track': issues.append(f'{nm}-joins-mid-track')
    for u in tracks:   # duplicate / collinear overlap beyond pads
        if u is t or u.GetLayer() != t.GetLayer() or id(u) < id(t): continue
        if same(u.GetStart(), t.GetStart()) and same(u.GetEnd(), t.GetEnd()) or same(u.GetStart(), t.GetEnd()) and same(u.GetEnd(), t.GetStart()):
            issues.append('duplicate-of ' + P(u.GetStart()))
    if issues:
        n1 += 1
        print(f'  {nn(t):26s} {t.GetLayerName():5s} {P(t.GetStart())}->{P(t.GetEnd())} w{M(t.GetWidth()):.2f}  {", ".join(issues)}')
# floating clusters: same net+layer union-find, cluster touching no pad and no via
parent = {id(t): id(t) for t in tracks}
def find(x):
    while parent[x] != x: parent[x] = parent[parent[x]]; x = parent[x]
    return x
for i, t in enumerate(tracks):
    for u in tracks[i+1:]:
        if u.GetLayer() == t.GetLayer() and nn(u) == nn(t) and t.GetEffectiveShape().Collide(u.GetEffectiveShape(), 0):
            parent[find(id(t))] = find(id(u))
clusters = collections.defaultdict(list)
for t in tracks: clusters[find(id(t))].append(t)
for cl in clusters.values():
    anchored = False
    for t in cl:
        L = t.GetLayer(); sh = t.GetEffectiveShape()
        if any(nn(v) == nn(t) and v.GetEffectiveShape(L).Collide(sh, 0) for v in vias): anchored = True; break
        if any(nn(p) == nn(t) and p.IsOnLayer(L) and p.GetEffectiveShape(L).Collide(sh, 0) for p in pads): anchored = True; break
    if not anchored:
        n1 += 1
        print(f'  FLOATING cluster {nn(cl[0])} {cl[0].GetLayerName()} {len(cl)} segs at {P(cl[0].GetStart())}')
print(f'  total {n1}')

# ---- via classification ----------------------------------------------------
def via_layers(v):
    out = {}
    for L in (F, B):
        sh = v.GetEffectiveShape(L)
        ts = [t for t in tracks if t.GetLayer() == L and nn(t) == nn(v) and t.GetEffectiveShape().Collide(sh, 0)]
        ps = [p for p in pads if p.IsOnLayer(L) and nn(p) == nn(v) and p.GetEffectiveShape(L).Collide(sh, 0)]
        out[L] = (ts, ps)
    return out
zone_nets = {z.GetNetname() for z in b.Zones() if z.IsOnCopperLayer()}
print('\n== 2. stray vias (nothing on a layer, or no layer change) ==')
n2 = 0
info = {}
for v in vias:
    vl = via_layers(v); info[id(v)] = vl
    nF = len(vl[F][0]) + len(vl[F][1]); nB = len(vl[B][0]) + len(vl[B][1])
    plane = nn(v) in zone_nets
    tag = None
    if nF == 0 and nB == 0: tag = 'ORPHAN'
    elif not plane and (nF == 0 or nB == 0): tag = f'one-sided F{nF} B{nB}'
    elif plane and nF == 0 and nB == 0: tag = 'plane-only'
    if tag:
        n2 += 1; print(f'  {nn(v):26s} {P(v.GetPosition())}  {tag}')
# stacked same-net vias within 1.0 mm
for i, v in enumerate(vias):
    for u in vias[i+1:]:
        if nn(u) == nn(v) and abs(M(u.GetPosition().x - v.GetPosition().x)) < 1.0 and abs(M(u.GetPosition().y - v.GetPosition().y)) < 1.0:
            n2 += 1; print(f'  {nn(v):26s} {P(v.GetPosition())} + {P(u.GetPosition())}  same-net vias <1 mm apart')
print(f'  total {n2}')

# ---- via hops on signal nets: can F.Cu carry them? -------------------------
def chain_from(v, L):
    """Walk same-net tracks on layer L from via v; return (tracks, vias reached, pads reached)."""
    seen, frontier, reached_v, reached_p = set(), [v.GetEffectiveShape(L)], {}, {}
    cand = [t for t in tracks if t.GetLayer() == L and nn(t) == nn(v)]
    while frontier:
        sh = frontier.pop()
        for t in cand:
            if id(t) in seen or not t.GetEffectiveShape().Collide(sh, 0): continue
            seen.add(id(t)); frontier.append(t.GetEffectiveShape())
    chain = [t for t in cand if id(t) in seen]
    for t in chain:
        for u in vias:
            if u is not v and nn(u) == nn(v) and u.GetEffectiveShape(L).Collide(t.GetEffectiveShape(), 0): reached_v[id(u)] = u
        for p in pads:
            if p.IsOnLayer(L) and nn(p) == nn(v) and p.GetEffectiveShape(L).Collide(t.GetEffectiveShape(), 0): reached_p[id(p)] = p
    return chain, list(reached_v.values()), list(reached_p.values())

fcu_obst = [(p.GetEffectiveShape(F), nn(p)) for p in pads if p.IsOnLayer(F)] + \
           [(t.GetEffectiveShape(), nn(t)) for t in tracks if t.GetLayer() == F] + \
           [(v.GetEffectiveShape(F), nn(v), v) for v in vias]
edge = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts]
def path_clear(pts, w, net, skip_vias):
    for a, c in zip(pts, pts[1:]):
        seg = pcbnew.SHAPE_SEGMENT(a, c, w)
        for ob in fcu_obst:
            if ob[1] == net: continue
            if len(ob) == 3 and id(ob[2]) in skip_vias: continue
            if ob[0].Collide(seg, CLR): return False
        for d in edge:
            if d.GetEffectiveShape().Collide(seg, FMM(0.3)): return False
    return True
def candidates(a, c):
    dx, dy = c.x - a.x, c.y - a.y
    out = [[a, c], [a, pcbnew.VECTOR2I(c.x, a.y), c], [a, pcbnew.VECTOR2I(a.x, c.y), c]]
    sx, sy = (1 if dx > 0 else -1), (1 if dy > 0 else -1)
    m = min(abs(dx), abs(dy))
    if abs(dx) > abs(dy):   # horizontal + 45
        out += [[a, pcbnew.VECTOR2I(a.x + sx*(abs(dx)-m), a.y), c], [a, pcbnew.VECTOR2I(a.x + sx*m, c.y), c]]
    else:
        out += [[a, pcbnew.VECTOR2I(a.x, a.y + sy*(abs(dy)-m)), c], [a, pcbnew.VECTOR2I(c.x, a.y + sy*m), c]]
    return out

print('\n== 3. via hops (F.Cu -> via -> B.Cu -> via -> F.Cu) and whether an F.Cu path is clear ==')
n3 = n3c = 0; done = set()
for v in vias:
    if nn(v) in zone_nets or id(v) in done: continue
    chain, rv, rp = chain_from(v, B)
    if not chain or rp or len(rv) != 1: continue        # B.Cu side ends at a pad or fans out: not a simple hop
    u = rv[0]; done |= {id(v), id(u)}
    if nn(u) in zone_nets: continue
    blen = sum(M(t.GetLength()) for t in chain)
    w = chain[0].GetWidth()
    a, c = v.GetPosition(), u.GetPosition()
    ok = [i for i, pts in enumerate(candidates(a, c)) if path_clear(pts, w, nn(v), {id(v), id(u)})]
    n3 += 1
    if ok: n3c += 1
    print(f'  {nn(v):26s} {P(a)} -> {P(c)}  B.Cu {blen:5.2f} mm, w{M(w):.2f}  ' + (f'F.CU PATH CLEAR (shape {ok})' if ok else 'blocked'))
print(f'  hops {n3}, F.Cu-replaceable by straight/L/45 test {n3c}')
