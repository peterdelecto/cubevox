"""Test pad space finder, read-only.

For every net (GND, VIN_9V and the 9 V nets excluded, nets that already have a TP marked) pick an
origin pad (IC pin first, then relay, connector, crystal, switch or pot, else the first pad) and
search a 0.5 mm grid within SEARCH_MM of it for a spot where TestPoint_Pad_D1.5mm fits: pad disc
plus clearance clear of foreign F.Cu copper, courtyard clear of every other courtyard and rule area,
inside the edge, and a straight or one-bend 0.3 mm stub from the origin pad clear at 0.2 mm.

  <kicad python3> -I hardware/tools/tp_space.py hardware/cubevox.kicad_pcb > table.md
"""
import math
import sys

import pcbnew

M = pcbnew.ToMM
FM = pcbnew.FromMM
F = pcbnew.F_Cu
SEARCH_MM = 8.0
PAD_R = 0.75
CLR = 0.25
CRTYD_R = 1.3
EDGE_MM = 1.5
STUB_W = 0.3
SKIP = {'/GND', '/VIN_9V', '/9V_JACK', '/9V_PLUG', ''}
PRIORITY = ('U', 'K', 'J', 'Y', 'SW', 'RV', 'ENC', 'D', 'Q', 'L', 'FB', 'R', 'C', 'TP')


def V(x, y):
    return pcbnew.VECTOR2I(int(round(x * 1e6)), int(round(y * 1e6)))


def main(path):
    b = pcbnew.LoadBoard(path)
    fps = list(b.GetFootprints())
    pads = [(fp, p) for fp in fps for p in fp.Pads()]
    tracks = [t for t in b.GetTracks() if t.GetClass() == 'PCB_TRACK' and t.GetLayer() == F]
    vias = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']
    # F.Cu copper obstacles with bboxes (mm)
    obst = []
    for fp, p in pads:
        if p.IsOnLayer(F):
            bb = p.GetBoundingBox()
            obst.append((p.GetNetname(), p.GetEffectiveShape(F), M(bb.GetLeft()), M(bb.GetTop()), M(bb.GetRight()), M(bb.GetBottom())))
    for t in tracks:
        bb = t.GetBoundingBox()
        obst.append((t.GetNetname(), t.GetEffectiveShape(), M(bb.GetLeft()), M(bb.GetTop()), M(bb.GetRight()), M(bb.GetBottom())))
    for v in vias:
        bb = v.GetBoundingBox()
        obst.append((v.GetNetname(), v.GetEffectiveShape(F), M(bb.GetLeft()), M(bb.GetTop()), M(bb.GetRight()), M(bb.GetBottom())))
    crt = []
    for fp in fps:
        poly = fp.GetCourtyard(pcbnew.F_CrtYd)
        if poly.OutlineCount() == 0:
            continue
        bb = poly.BBox()
        crt.append((fp.GetReference(), poly, M(bb.GetLeft()), M(bb.GetTop()), M(bb.GetRight()), M(bb.GetBottom())))
    rule_zones = [z for z in b.Zones() if z.GetIsRuleArea()]
    edge = b.GetBoardEdgesBoundingBox()
    ex0, ey0, ex1, ey1 = M(edge.GetLeft()) + EDGE_MM, M(edge.GetTop()) + EDGE_MM, M(edge.GetRight()) - EDGE_MM, M(edge.GetBottom()) - EDGE_MM
    edges = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts]

    def near(items, x, y, r):
        return [it for it in items if it[2] - r <= x <= it[4] + r and it[3] - r <= y <= it[5] + r]

    def spot_ok(x, y, net):
        if not (ex0 <= x <= ex1 and ey0 <= y <= ey1):
            return False
        disc = pcbnew.SHAPE_CIRCLE(V(x, y), FM(PAD_R))
        for n, sh, *_ in near(obst, x, y, PAD_R + CLR + 0.1):
            if n != net and sh.Collide(disc, FM(CLR)):
                return False
        cy = pcbnew.SHAPE_CIRCLE(V(x, y), FM(CRTYD_R))
        for ref, poly, *_ in near(crt, x, y, CRTYD_R + 0.1):
            if poly.Collide(cy, 0):
                return False
        for z in rule_zones:
            if z.Outline().Collide(cy, 0):
                return False
        for d in edges:
            if d.GetEffectiveShape().Collide(cy, FM(0.3)):
                return False
        return True

    def stub_ok(ax, ay, cx, cy, net):
        dx, dy = cx - ax, cy - ay
        m = min(abs(dx), abs(dy))
        sx, sy = (1 if dx > 0 else -1), (1 if dy > 0 else -1)
        cands = [[(ax, ay), (cx, cy)]] if (dx == 0 or dy == 0 or abs(abs(dx) - abs(dy)) < 1e-6) else []
        if abs(dx) > abs(dy):
            cands += [[(ax, ay), (ax + sx * (abs(dx) - m), ay), (cx, cy)], [(ax, ay), (ax + sx * m, cy), (cx, cy)]]
        elif abs(dy) > abs(dx):
            cands += [[(ax, ay), (ax, ay + sy * (abs(dy) - m)), (cx, cy)], [(ax, ay), (cx, ay + sy * m), (cx, cy)]]
        for pts in cands:
            good = True
            stub_ok.last = pts
            for a, c in zip(pts, pts[1:]):
                seg = pcbnew.SHAPE_SEGMENT(V(*a), V(*c), FM(STUB_W))
                lo_x, hi_x = min(a[0], c[0]), max(a[0], c[0])
                lo_y, hi_y = min(a[1], c[1]), max(a[1], c[1])
                for n, sh, bx0, by0, bx1, by1 in obst:
                    if n == net or bx1 < lo_x - 0.5 or bx0 > hi_x + 0.5 or by1 < lo_y - 0.5 or by0 > hi_y + 0.5:
                        continue
                    if sh.Collide(seg, FM(0.2)):
                        good = False
                        break
                if not good:
                    break
            if good:
                return True
        return False

    if '--try' in sys.argv:
        # NET=REF.PIN@x,y : report spot and straight-stub checks for a hand-picked position
        padmap = {fp.GetReference() + '.' + p.GetNumber(): p for fp, p in pads}
        for item in sys.argv[sys.argv.index('--try') + 1].split(';'):
            n, rest = item.split('=')
            o, xy = rest.split('@')
            x, y = (float(v) for v in xy.split(','))
            p = padmap[o]
            net = p.GetNetname()
            ox, oy = M(p.GetPosition().x), M(p.GetPosition().y)
            print('%-10s %-9s (%.2f,%.2f) spot %s stub %s' % (n, o, x, y, spot_ok(x, y, net), stub_ok(ox, oy, x, y, net) and stub_ok.last))
        return
    only = {}
    if '--nets' in sys.argv:
        for item in sys.argv[sys.argv.index('--nets') + 1].split(','):
            n, o = item.split('=')
            only[n] = o
    nets = {}
    for fp, p in pads:
        n = p.GetNetname()
        if n in SKIP or n.startswith('unconnected-'):
            continue
        nets.setdefault(n, []).append((fp.GetReference(), p))
    has_tp = {p.GetNetname(): fp.GetReference() for fp, p in pads if fp.GetReference().startswith('TP')}

    def rank(ref):
        for i, pre in enumerate(PRIORITY):
            if ref.startswith(pre):
                return i
        return len(PRIORITY)

    print('| net | origin | result | spot (x, y) | stub | other ICs on the net |')
    print('|---|---|---|---|---|---|')
    yes = no = 0
    for net in sorted(nets):
        if only and net.split('/')[-1] not in only:
            continue
        plist = sorted(nets[net], key=lambda rp: (rank(rp[0]), rp[0]))
        if only:
            want = only[net.split('/')[-1]]
            plist = [rp for rp in plist if rp[0] + '.' + rp[1].GetNumber() == want] or plist
        ref, p = plist[0]
        others = sorted({r for r, q in plist if r.startswith('U') and r != ref})
        if net in has_tp:
            print('| %s | %s.%s | has %s | | | %s |' % (net.split('/')[-1], ref, p.GetNumber(), has_tp[net], ' '.join(others)))
            continue
        best = None
        # origin pads in priority order; the first pad with a spot within SEARCH_MM wins, nearest ring first
        for oref, op in plist:
            ox, oy = M(op.GetPosition().x), M(op.GetPosition().y)
            r = 1.0
            while r <= SEARCH_MM and best is None:
                steps = max(8, int(2 * math.pi * r / 0.5))
                for k in range(steps):
                    a = 2 * math.pi * k / steps
                    x, y = round(ox + r * math.cos(a), 2), round(oy + r * math.sin(a), 2)
                    if spot_ok(x, y, net) and stub_ok(ox, oy, x, y, net):
                        best = (x, y, r, oref, op.GetNumber())
                        print('path', net, oref, op.GetNumber(), stub_ok.last)
                        break
                r += 0.5
            if best:
                break
        if best:
            yes += 1
            tag = 'yes' if (best[3], best[4]) == (ref, p.GetNumber()) else 'yes, at %s.%s' % (best[3], best[4])
            print('| %s | %s.%s | %s | (%.2f, %.2f) | %.1f mm | %s |' % (net.split('/')[-1], ref, p.GetNumber(), tag, best[0], best[1], best[2], ' '.join(others)))
        else:
            no += 1
            print('| %s | %s.%s | no | | | %s |' % (net.split('/')[-1], ref, p.GetNumber(), ' '.join(others)))
    print()
    print('yes %d, no %d, with a pad already %d' % (yes, no, len([n for n in nets if n in has_tp])))


if __name__ == '__main__':
    main(sys.argv[1])
