"""Pre-production routing audit, read-only. Run with the KiCad python:

  <kicad python3> -I hardware/tools/mega_audit.py hardware/cubevox.kicad_pcb [section ...]

Sections (canon rule in brackets):
  loops      copper cycles per net                                   [133]
  micro      segments under 0.5 mm outside pads                      [136]
  jogs       45 / short straight / 45 lane shifts                    [136]
  acute      arms meeting under 90 degrees                           [45]
  merge      collinear same-width segments that should be one        [craft]
  nearmiss   endpoints 0 < d <= 0.05 mm from a joint                 [craft]
  centre     track ends off a pad or via centre                      [132]
  parallel   long close parallel runs between nets, typed            [2, 55]
  viagap     via pairs under 0.8 mm centre to centre                 [craft]
  slots      via chains that merge their plane antipads              [10]
  decap      cap GND via sharing and plane-fed supply pads           [63, 68, 134]
  gndtrace   GND copper on a signal layer beyond pad-to-via stubs    [138]
  width      minority widths per net                                 [48]
  crystal    foreign copper under the crystal                        [41]
"""
import collections
import math
import sys

import pcbnew

F, B = pcbnew.F_Cu, pcbnew.B_Cu
M = pcbnew.ToMM

POWER = ('/3V3', '/5V', '/5VA', '/3V3A', '/VIN_9V', '/VREF', '/h7core_block/VBUS',
         '/h7core_block/VDDA', '/h7core_block/VCAP1', '/h7core_block/VCAP2', '/ADC_VCC',
         '/ADC_VREF', '/VNEG', '/BIAS_W', '/VREF_DIV')
DIGITAL = ('I2S_', 'ADC_SCKI', 'ADC_DOUT', 'SWD', 'SWCLK', 'SWO', 'USB_', 'MUX_S', 'ENC_',
           'BOOT0', 'NRST', 'PAD_EN', 'MUTE', 'TOGGLE', 'OLED', 'SPI', 'SCL', 'SDA', 'XSMT',
           'HSE', 'XTAL', 'LED', 'DFU', 'RST', 'CS', 'DC', 'MOSI', 'SCK')


def net_type(n):
    s = n.split('/')[-1]
    if n == '/GND':
        return 'gnd'
    if n in POWER:
        return 'power'
    if any(s.startswith(d) or d in s for d in DIGITAL):
        return 'digital'
    return 'analog'


class Seg:
    __slots__ = ('t', 'net', 'layer', 'x1', 'y1', 'x2', 'y2', 'w', 'L', 'ang')

    def __init__(self, t):
        self.t = t
        self.net = t.GetNetname()
        self.layer = t.GetLayer()
        self.x1, self.y1 = M(t.GetStart().x), M(t.GetStart().y)
        self.x2, self.y2 = M(t.GetEnd().x), M(t.GetEnd().y)
        self.w = M(t.GetWidth())
        self.L = math.hypot(self.x2 - self.x1, self.y2 - self.y1)
        self.ang = math.degrees(math.atan2(self.y2 - self.y1, self.x2 - self.x1)) % 180

    def where(self):
        return '%-24s %s (%.2f,%.2f)->(%.2f,%.2f)' % (self.net, LN[self.layer], self.x1, self.y1, self.x2, self.y2)


def main(path, wanted):
    global LN
    b = pcbnew.LoadBoard(path)
    LN = {F: 'F.Cu', B: 'B.Cu'}
    segs = [Seg(t) for t in b.GetTracks() if t.GetClass() == 'PCB_TRACK' and t.GetLength() > 0]
    vias = [t for t in b.GetTracks() if t.GetClass() == 'PCB_VIA']
    vpos = [(M(v.GetPosition().x), M(v.GetPosition().y), v.GetNetname(), v) for v in vias]
    fps = list(b.GetFootprints())
    pads = [(fp.GetReference(), p) for fp in fps for p in p_iter(fp)]
    zone_nets = {z.GetNetname() for z in b.Zones() if z.IsOnCopperLayer() and z.GetNetname()}
    out = collections.OrderedDict()

    def run(name, fn):
        if wanted and name not in wanted:
            return
        rows = fn()
        out[name] = rows
        print('== %s: %d' % (name, len(rows)))
        for r in rows:
            print('  ' + r)

    def pad_at(x, y, net, layer):
        pt = pcbnew.VECTOR2I(int(x * 1e6), int(y * 1e6))
        for ref, p in pads:
            if p.GetNetname() == net and p.IsOnLayer(layer) and p.HitTest(pt):
                return ref, p
        return None

    def vrad(v):
        try:
            return M(v.GetWidth(F)) / 2
        except TypeError:
            return M(v.GetWidth()) / 2

    def via_at(x, y, net):
        for vx, vy, vn, v in vpos:
            if vn == net and math.hypot(vx - x, vy - y) <= vrad(v):
                return v
        return None

    # per-layer obstacles for the replacement test (canon 136)
    CLR = pcbnew.FromMM(0.2)
    obst = {L: [(p.GetEffectiveShape(L), p.GetNetname()) for ref, p in pads if p.IsOnLayer(L)]
            + [(s.t.GetEffectiveShape(), s.net) for s in segs if s.layer == L]
            + [(v.GetEffectiveShape(L), v.GetNetname()) for v in vias] for L in (F, B)}
    edge = [d for d in b.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts]

    def path_clear(pts, w, net, L):
        for a, c in zip(pts, pts[1:]):
            seg = pcbnew.SHAPE_SEGMENT(a, c, w)
            for sh, n in obst[L]:
                if n != net and sh.Collide(seg, CLR):
                    return False
            for d in edge:
                if d.GetEffectiveShape().Collide(seg, pcbnew.FromMM(0.3)):
                    return False
        return True

    def V(x, y):
        return pcbnew.VECTOR2I(int(round(x * 1e6)), int(round(y * 1e6)))

    def candidates(ax, ay, cx, cy):
        dx, dy = cx - ax, cy - ay
        sx, sy = (1 if dx > 0 else -1), (1 if dy > 0 else -1)
        m = min(abs(dx), abs(dy))
        out = []
        if abs(abs(dx) - abs(dy)) < 1e-6 or dx == 0 or dy == 0:
            out.append([V(ax, ay), V(cx, cy)])
        if abs(dx) > abs(dy):
            out.append([V(ax, ay), V(ax + sx * (abs(dx) - m), ay), V(cx, cy)])
            out.append([V(ax, ay), V(ax + sx * m, cy), V(cx, cy)])
        elif abs(dy) > abs(dx):
            out.append([V(ax, ay), V(ax, ay + sy * (abs(dy) - m)), V(cx, cy)])
            out.append([V(ax, ay), V(cx, ay + sy * m), V(cx, cy)])
        return out

    # joints: same net+layer endpoints within 1 um
    joints = collections.defaultdict(list)
    for s in segs:
        joints[(s.net, s.layer, round(s.x1, 3), round(s.y1, 3))].append((s, (s.x2, s.y2)))
        joints[(s.net, s.layer, round(s.x2, 3), round(s.y2, 3))].append((s, (s.x1, s.y1)))

    # ---- loops (canon 133) ------------------------------------------------
    def loops():
        """Graph: nodes are track endpoints (per net+layer, clustered to 1 um), vias and pads.
        A track is an edge between its endpoint nodes. An endpoint lying on a via or pad
        merges with that node. A via or pad touching a track away from its ends joins by a
        T edge. A union-find edge whose ends are already joined closes a cycle."""
        rows = []
        by_net = collections.defaultdict(lambda: ([], [], []))
        for s in segs:
            by_net[s.net][0].append(s)
        for v in vias:
            by_net[v.GetNetname()][1].append(v)
        for ref, p in pads:
            by_net[p.GetNetname()][2].append((ref, p))
        for net, (ss, vv, pp) in by_net.items():
            if net in zone_nets or not net:
                continue
            parent = {}

            def find(x):
                parent.setdefault(x, x)
                while parent[x] != x:
                    parent[x] = parent[parent[x]]
                    x = parent[x]
                return x

            def union(a, c):
                ra, rc = find(a), find(c)
                if ra == rc:
                    return False
                parent[ra] = rc
                return True
            # endpoint nodes
            def node(s, end):
                x, y = (s.x1, s.y1) if end == 0 else (s.x2, s.y2)
                return ('e', s.layer, round(x, 3), round(y, 3))
            # merge endpoints with the via / pad they sit on
            for s in ss:
                for end in (0, 1):
                    x, y = (s.x1, s.y1) if end == 0 else (s.x2, s.y2)
                    n = node(s, end)
                    for v in vv:
                        if math.hypot(M(v.GetPosition().x) - x, M(v.GetPosition().y) - y) <= vrad(v):
                            union(n, ('v', id(v)))
                    pt = pcbnew.VECTOR2I(int(x * 1e6), int(y * 1e6))
                    for ref, p in pp:
                        if p.IsOnLayer(s.layer) and p.HitTest(pt):
                            union(n, ('p', id(p)))
            # pads touching vias directly (via in or on a pad)
            for v in vv:
                for ref, p in pp:
                    if any(p.IsOnLayer(L) and v.GetEffectiveShape(L).Collide(p.GetEffectiveShape(L), 0) for L in (F, B)):
                        union(('v', id(v)), ('p', id(p)))
            # track edges
            for s in ss:
                a, c = node(s, 0), node(s, 1)
                if not union(a, c):
                    rows.append('%-24s closed by track %s' % (net, s.where()))
            # T contacts: via or pad touching a track away from both ends
            for s in ss:
                sh = s.t.GetEffectiveShape()
                for v in vv:
                    vx, vy = M(v.GetPosition().x), M(v.GetPosition().y)
                    r = vrad(v)
                    if math.hypot(vx - s.x1, vy - s.y1) <= r or math.hypot(vx - s.x2, vy - s.y2) <= r:
                        continue
                    if v.GetEffectiveShape(s.layer).Collide(sh, 0):
                        if not union(('v', id(v)), node(s, 0)):
                            rows.append('%-24s closed by via (%.2f,%.2f) mid-track %s' % (net, vx, vy, s.where()))
                for ref, p in pp:
                    if not p.IsOnLayer(s.layer):
                        continue
                    p1 = pcbnew.VECTOR2I(int(s.x1 * 1e6), int(s.y1 * 1e6))
                    p2 = pcbnew.VECTOR2I(int(s.x2 * 1e6), int(s.y2 * 1e6))
                    if p.HitTest(p1) or p.HitTest(p2):
                        continue
                    if p.GetEffectiveShape(s.layer).Collide(sh, 0):
                        if not union(('p', id(p)), node(s, 0)):
                            rows.append('%-24s closed by pad %s.%s mid-track %s' % (net, ref, p.GetNumber(), s.where()))
        return rows

    def desc(it):
        if it[0] == 't':
            t = it[1]
            return '%s (%.2f,%.2f)->(%.2f,%.2f)' % (LN[it[2]], M(t.GetStart().x), M(t.GetStart().y), M(t.GetEnd().x), M(t.GetEnd().y))
        if it[0] == 'v':
            return 'via (%.2f,%.2f)' % (M(it[1].GetPosition().x), M(it[1].GetPosition().y))
        return 'pad %s.%s' % (it[3], it[1].GetNumber())

    # ---- micro segments ------------------------------------------------------
    def micro():
        rows = []
        for s in segs:
            if s.L >= 0.5:
                continue
            inside = pad_at(s.x1, s.y1, s.net, s.layer) and pad_at(s.x2, s.y2, s.net, s.layer)
            if inside:
                continue
            rows.append('%s L %.2f' % (s.where(), s.L))
        return rows

    # ---- jogs: 45 / straight / 45 with parallel diagonals -------------------
    def jogs():
        rows = []
        seen = set()
        for s in segs:
            if abs(s.ang % 90) > 0.5:          # s must be the straight
                continue
            arms = []
            for (x, y) in ((s.x1, s.y1), (s.x2, s.y2)):
                js = [o for o, far in joints[(s.net, s.layer, round(x, 3), round(y, 3))] if o is not s]
                arms.append(js)
            if len(arms[0]) != 1 or len(arms[1]) != 1:
                continue
            a, c = arms[0][0], arms[1][0]
            if abs(abs(a.ang - 45) % 90) > 0.5 and abs(a.ang % 90 - 45) > 0.5:
                continue
            if abs(a.ang - c.ang) > 0.5:       # diagonals parallel -> Z shift
                continue
            if abs(a.ang % 90 - 45) > 0.5:
                continue
            if s.L > 3.0:
                continue
            k = tuple(sorted([id(a), id(c)]))
            if k in seen:
                continue
            seen.add(k)
            # far ends of the two diagonals
            def far(d, jx, jy):
                return (d.x2, d.y2) if abs(d.x1 - jx) < 1e-6 and abs(d.y1 - jy) < 1e-6 else (d.x1, d.y1)
            ax, ay = far(a, s.x1, s.y1) if a in [o for o, _ in joints[(s.net, s.layer, round(s.x1, 3), round(s.y1, 3))]] else far(a, s.x2, s.y2)
            cx, cy = far(c, s.x2, s.y2) if c in [o for o, _ in joints[(s.net, s.layer, round(s.x2, 3), round(s.y2, 3))]] else far(c, s.x1, s.y1)
            w = int(round(s.w * 1e6))
            ok = [i for i, pts in enumerate(candidates(ax, ay, cx, cy)) if path_clear(pts, w, s.net, s.layer)]
            verdict = 'REPLACEABLE shape %s' % ok if ok else 'no simpler path clear'
            rows.append('%s straight %.2f, diagonals %.2f/%.2f, ends (%.2f,%.2f)-(%.2f,%.2f): %s' % (s.where(), s.L, a.L, c.L, ax, ay, cx, cy, verdict))
        return rows

    # ---- acute angles (canon 45) --------------------------------------------
    def acute():
        rows = []
        for (net, layer, x, y), arms in joints.items():
            if len(arms) < 2:
                continue
            for i in range(len(arms)):
                for j in range(i + 1, len(arms)):
                    (s1, f1), (s2, f2) = arms[i], arms[j]
                    v1 = (f1[0] - x, f1[1] - y)
                    v2 = (f2[0] - x, f2[1] - y)
                    n1, n2 = math.hypot(*v1), math.hypot(*v2)
                    if n1 == 0 or n2 == 0:
                        continue
                    cosang = max(-1, min(1, (v1[0] * v2[0] + v1[1] * v2[1]) / (n1 * n2)))
                    ang = math.degrees(math.acos(cosang))
                    if ang < 89.5:
                        where = 'in pad' if pad_at(x, y, net, layer) else ('on via' if via_at(x, y, net) else 'OPEN')
                        rows.append('%-24s %s (%.2f,%.2f) %.0f deg %s' % (net, LN[layer], x, y, ang, where))
        return rows

    # ---- mergeable collinear segments ---------------------------------------
    def merge():
        rows = []
        for (net, layer, x, y), arms in joints.items():
            if len(arms) != 2:
                continue
            (s1, _), (s2, _) = arms
            if s1 is s2 or abs(s1.ang - s2.ang) > 0.1 or abs(s1.w - s2.w) > 1e-6:
                continue
            if via_at(x, y, net) or pad_at(x, y, net, layer):
                continue
            rows.append('%-24s %s (%.2f,%.2f) %.1f deg' % (net, LN[layer], x, y, s1.ang))
        return rows

    # ---- near-miss joins ------------------------------------------------------
    def nearmiss():
        rows = []
        pts = collections.defaultdict(list)
        for s in segs:
            pts[(s.net, s.layer)].append((s.x1, s.y1, s))
            pts[(s.net, s.layer)].append((s.x2, s.y2, s))
        for (net, layer), lst in pts.items():
            for i in range(len(lst)):
                for j in range(i + 1, len(lst)):
                    d = math.hypot(lst[i][0] - lst[j][0], lst[i][1] - lst[j][1])
                    if 1e-6 < d <= 0.05 and lst[i][2] is not lst[j][2]:
                        rows.append('%-24s %s (%.3f,%.3f) vs (%.3f,%.3f) d %.3f' % (net, LN[layer], lst[i][0], lst[i][1], lst[j][0], lst[j][1], d))
            for x, y, s in lst:
                for vx, vy, vn, v in vpos:
                    if vn == net:
                        d = math.hypot(vx - x, vy - y)
                        if 1e-6 < d <= 0.05:
                            rows.append('%-24s %s end (%.3f,%.3f) via (%.3f,%.3f) d %.3f' % (net, LN[layer], x, y, vx, vy, d))
        return rows

    # ---- pad / via centre (canon 132) ----------------------------------------
    def centre():
        rows = []
        for s in segs:
            for (x, y) in ((s.x1, s.y1), (s.x2, s.y2)):
                hit = pad_at(x, y, s.net, s.layer)
                if hit:
                    ref, p = hit
                    if p.GetShape() == pcbnew.PAD_SHAPE_CIRCLE and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
                        pass
                    cx, cy = M(p.GetPosition().x), M(p.GetPosition().y)
                    d = math.hypot(cx - x, cy - y)
                    if d > 0.01:
                        # a track running through the pad along its axis is a pass-through; report the lateral offset
                        rows.append('%s end (%.2f,%.2f) off %s.%s centre by %.3f' % (s.where(), x, y, ref, p.GetNumber(), d))
                    continue
                v = via_at(x, y, s.net)
                if v:
                    vx, vy = M(v.GetPosition().x), M(v.GetPosition().y)
                    d = math.hypot(vx - x, vy - y)
                    if d > 0.01:
                        rows.append('%s end (%.2f,%.2f) off via centre by %.3f' % (s.where(), x, y, d))
        # pass-through: a pad centre lying within a track but no track end at the centre
        for ref, p in pads:
            if p.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
                continue
            cx, cy = M(p.GetPosition().x), M(p.GetPosition().y)
            net = p.GetNetname()
            ends_here = any(abs(s.x1 - cx) < 0.011 and abs(s.y1 - cy) < 0.011 or abs(s.x2 - cx) < 0.011 and abs(s.y2 - cy) < 0.011
                            for s in segs if s.net == net and s.layer == p.GetLayer())
            if ends_here:
                continue
            for s in segs:
                if s.net != net or s.layer != p.GetLayer():
                    continue
                # distance from centre to segment line, within its extent
                dx, dy = s.x2 - s.x1, s.y2 - s.y1
                tt = ((cx - s.x1) * dx + (cy - s.y1) * dy) / (s.L * s.L)
                if 0 < tt < 1:
                    px, py = s.x1 + tt * dx, s.y1 + tt * dy
                    d = math.hypot(px - cx, py - cy)
                    if d < s.w / 2 + max(M(p.GetSize().x), M(p.GetSize().y)) / 2 and d > 0.01:
                        rows.append('%s passes %s.%s %.3f off centre' % (s.where(), ref, p.GetNumber(), d))
        return rows

    # ---- parallel runs ---------------------------------------------------------
    def parallel():
        rows = []
        for L in (F, B):
            ss = [s for s in segs if s.layer == L and s.L >= 2.0]
            for i in range(len(ss)):
                a = ss[i]
                for j in range(i + 1, len(ss)):
                    c = ss[j]
                    if a.net == c.net:
                        continue
                    da = abs(a.ang - c.ang)
                    if min(da, 180 - da) > 1.0:
                        continue
                    ux, uy = (a.x2 - a.x1) / a.L, (a.y2 - a.y1) / a.L
                    # project c onto a's axis
                    def proj(x, y):
                        return (x - a.x1) * ux + (y - a.y1) * uy, abs(-(x - a.x1) * uy + (y - a.y1) * ux)
                    p1, g1 = proj(c.x1, c.y1)
                    p2, g2 = proj(c.x2, c.y2)
                    lo, hi = max(0, min(p1, p2)), min(a.L, max(p1, p2))
                    ov = hi - lo
                    gap = (g1 + g2) / 2 - a.w / 2 - c.w / 2
                    ta, tc = net_type(a.net), net_type(c.net)
                    kinds = {ta, tc}
                    if ov < 3.0 or gap > 1.0:
                        continue
                    if 'analog' in kinds and 'digital' in kinds:
                        tag = 'ANALOG|DIGITAL'
                    elif kinds == {'analog'} and gap < 0.5 and ov >= 5.0:
                        tag = 'analog|analog'
                    elif gap < 0.3 and ov >= 8.0:
                        tag = 'tight'
                    else:
                        continue
                    rows.append('%-14s %s %s | %s  gap %.2f over %.1f mm' % (tag, LN[L], a.net, c.net, gap, ov))
        rows.sort()
        return rows

    # ---- via spacing ------------------------------------------------------------
    def viagap():
        rows = []
        for i in range(len(vpos)):
            for j in range(i + 1, len(vpos)):
                d = math.hypot(vpos[i][0] - vpos[j][0], vpos[i][1] - vpos[j][1])
                if d < 0.8:
                    rows.append('%s (%.2f,%.2f) | %s (%.2f,%.2f) d %.2f' % (vpos[i][2], vpos[i][0], vpos[i][1], vpos[j][2], vpos[j][0], vpos[j][1], d))
        return rows

    # ---- plane slots: via chains with merged antipads ------------------------
    def slots():
        rows = []
        merge_d = 0.6 + 2 * 0.25   # via diameter + plane clearance each side
        n = len(vpos)
        parent = list(range(n))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i
        for i in range(n):
            for j in range(i + 1, n):
                if math.hypot(vpos[i][0] - vpos[j][0], vpos[i][1] - vpos[j][1]) < merge_d:
                    parent[find(i)] = find(j)
        groups = collections.defaultdict(list)
        for i in range(n):
            groups[find(i)].append(vpos[i])
        for g in groups.values():
            if len(g) < 3:
                continue
            xs = [p[0] for p in g]
            ys = [p[1] for p in g]
            span = math.hypot(max(xs) - min(xs), max(ys) - min(ys))
            nets = sorted({p[2].split('/')[-1] for p in g})
            rows.append('%d vias, span %.1f mm, at (%.1f,%.1f)-(%.1f,%.1f): %s' % (len(g), span, min(xs), min(ys), max(xs), max(ys), ' '.join(nets)))
        rows.sort(key=lambda r: -int(r.split()[0]))
        return rows

    # ---- decoupling caps ----------------------------------------------------------
    def decap():
        rows = []
        for ref, _ in pads:
            pass
        caps = collections.defaultdict(list)
        for ref, p in pads:
            if ref.startswith('C'):
                caps[ref].append(p)
        via_users = collections.defaultdict(list)
        for ref, plist in caps.items():
            nets = {p.GetNetname() for p in plist}
            if '/GND' not in nets or not (nets & set(POWER)):
                continue
            for p in plist:
                net = p.GetNetname()
                L = p.GetLayer()
                reach = walk(p, L)
                rv = [v for v in reach['vias']]
                rp = [q for q in reach['pads'] if q[0] != ref]
                if net == '/GND':
                    if not rv:
                        rows.append('%s GND pad: no via reached on %s' % (ref, LN[L]))
                    for v in rv:
                        via_users[id(v)].append((ref, v))
                    far = [v for v in rv if math.hypot(M(v.GetPosition().x) - M(p.GetPosition().x), M(v.GetPosition().y) - M(p.GetPosition().y)) > 2.0]
                    for v in far:
                        rows.append('%s GND via %.1f mm from the pad' % (ref, math.hypot(M(v.GetPosition().x) - M(p.GetPosition().x), M(v.GetPosition().y) - M(p.GetPosition().y))))
                else:
                    ics = [q for q in rp if q[0].startswith('U') or q[0].startswith('Y')]
                    if not ics and net in zone_nets:
                        continue
                    if not ics:
                        rows.append('%s %s pad reaches no IC pin on %s (via-fed: %d vias, pads %s)' % (ref, net.split('/')[-1], LN[L], len(rv), ' '.join(q[0] + '.' + q[1] for q in rp[:4])))
        for k, users in via_users.items():
            refs = sorted({u[0] for u in users})
            if len(refs) > 1:
                v = users[0][1]
                rows.append('shared GND via (%.2f,%.2f): %s' % (M(v.GetPosition().x), M(v.GetPosition().y), ' '.join(refs)))
        return rows

    def walk(p, L):
        """Same-layer copper reachable from pad p through tracks; stops at vias and other pads."""
        net = p.GetNetname()
        seen = set()
        frontier = [p.GetEffectiveShape(L)]
        cand = [s for s in segs if s.net == net and s.layer == L]
        chain = []
        while frontier:
            sh = frontier.pop()
            for s in cand:
                if id(s) in seen or not s.t.GetEffectiveShape().Collide(sh, 0):
                    continue
                seen.add(id(s))
                chain.append(s)
                frontier.append(s.t.GetEffectiveShape())
        rv, rp = [], []
        shapes = [s.t.GetEffectiveShape() for s in chain] + [p.GetEffectiveShape(L)]
        for v in vias:
            if v.GetNetname() == net and any(v.GetEffectiveShape(L).Collide(sh, 0) for sh in shapes):
                rv.append(v)
        for ref, q in pads:
            if q is p or q.GetNetname() != net or not q.IsOnLayer(L):
                continue
            if any(q.GetEffectiveShape(L).Collide(sh, 0) for sh in shapes):
                rp.append((ref, q.GetNumber()))
        return {'vias': rv, 'pads': rp, 'tracks': chain}

    # ---- GND traces (canon 138) ----------------------------------------------------
    def gndtrace():
        rows = []
        gs = [s for s in segs if s.net == '/GND']
        parent = {id(s): id(s) for s in gs}

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x
        for i in range(len(gs)):
            for j in range(i + 1, len(gs)):
                if gs[i].layer == gs[j].layer and gs[i].t.GetEffectiveShape().Collide(gs[j].t.GetEffectiveShape(), 0):
                    parent[find(id(gs[i]))] = find(id(gs[j]))
        cl = collections.defaultdict(list)
        for s in gs:
            cl[find(id(s))].append(s)
        for c in cl.values():
            L = c[0].layer
            length = sum(s.L for s in c)
            shapes = [s.t.GetEffectiveShape() for s in c]
            refs = sorted({ref for ref, p in pads if p.GetNetname() == '/GND' and p.IsOnLayer(L) and any(p.GetEffectiveShape(L).Collide(sh, 0) for sh in shapes)})
            nv = sum(1 for v in vias if v.GetNetname() == '/GND' and any(v.GetEffectiveShape(L).Collide(sh, 0) for sh in shapes))
            if len(refs) > 1 or length > 3.0:
                rows.append('%s %.1f mm, %d segs, pads %s, vias %d at (%.2f,%.2f)' % (LN[L], length, len(c), ' '.join(refs), nv, c[0].x1, c[0].y1))
        rows.sort(key=lambda r: -float(r.split()[1]))
        return rows

    # ---- off-pad T junctions (canon 44/45: any off-pad branch on the grid is a 90 or a 45) ----
    def tees():
        rows = []
        for (net, layer, x, y), arms in joints.items():
            if len(arms) < 3:
                continue
            if pad_at(x, y, net, layer) or via_at(x, y, net):
                continue
            rows.append('%-24s %s (%.2f,%.2f) %d arms' % (net, LN[layer], x, y, len(arms)))
        return rows

    # ---- bends at a pad or via edge (canon 132: change direction after leaving, never at the edge) ----
    def edgebend():
        rows = []
        for s in segs:
            if abs(s.ang % 90) < 0.5:
                continue                      # diagonals only
            a_in = pad_at(s.x1, s.y1, s.net, s.layer) or via_at(s.x1, s.y1, s.net)
            c_in = pad_at(s.x2, s.y2, s.net, s.layer) or via_at(s.x2, s.y2, s.net)
            if a_in or c_in:
                continue                      # ends inside copper: pad_entry / centre sections own it
            sh = s.t.GetEffectiveShape()
            for ref, p in pads:
                if p.GetNetname() == s.net and p.IsOnLayer(s.layer) and p.GetEffectiveShape(s.layer).Collide(sh, 0):
                    rows.append('%s diagonal touches %s.%s' % (s.where(), ref, p.GetNumber()))
            for vx, vy, vn, v in vpos:
                if vn == s.net and v.GetEffectiveShape(s.layer).Collide(sh, 0):
                    rows.append('%s diagonal touches via (%.2f,%.2f)' % (s.where(), vx, vy))
        # straight exits shorter than the pad reach: bend inside the pad
        for (net, layer, x, y), arms in joints.items():
            if len(arms) != 2 or via_at(x, y, net):
                continue
            hit = pad_at(x, y, net, layer)
            if not hit:
                continue
            ref, p = hit
            cx, cy = M(p.GetPosition().x), M(p.GetPosition().y)
            if math.hypot(cx - x, cy - y) > 0.01:
                rows.append('%-24s %s bend (%.2f,%.2f) inside %s.%s' % (net, LN[layer], x, y, ref, p.GetNumber()))
        return rows

    # ---- widths ---------------------------------------------------------------------
    def width():
        rows = []
        wid = collections.defaultdict(collections.Counter)
        for s in segs:
            wid[s.net][s.w] += 1
        for net, c in sorted(wid.items()):
            if len(c) > 1:
                rows.append('%-24s %s' % (net, ' '.join('%.2f x%d' % (w, k) for w, k in sorted(c.items()))))
        return rows

    # ---- crystal -----------------------------------------------------------------------
    def crystal():
        rows = []
        for fp in fps:
            if not fp.GetReference().startswith('Y'):
                continue
            own = {p.GetNetname() for p in fp.Pads()}
            bb = fp.GetCourtyard(F).BBox()
            for s in segs:
                if s.net in own or s.net == '/GND':
                    continue
                if bb.Contains(s.t.GetStart()) or bb.Contains(s.t.GetEnd()):
                    rows.append('%s under %s' % (s.where(), fp.GetReference()))
            for vx, vy, vn, v in vpos:
                if vn not in own and vn != '/GND' and bb.Contains(v.GetPosition()):
                    rows.append('via %s (%.2f,%.2f) under %s' % (vn, vx, vy, fp.GetReference()))
        return rows

    for name, fn in (('loops', loops), ('micro', micro), ('jogs', jogs), ('acute', acute), ('merge', merge),
                     ('nearmiss', nearmiss), ('centre', centre), ('parallel', parallel), ('viagap', viagap),
                     ('slots', slots), ('decap', decap), ('gndtrace', gndtrace), ('tees', tees), ('edgebend', edgebend),
                     ('width', width), ('crystal', crystal)):
        run(name, fn)
    print('== summary: ' + ' '.join('%s %d' % (k, len(v)) for k, v in out.items()))


def p_iter(fp):
    return list(fp.Pads())


if __name__ == '__main__':
    main(sys.argv[1], set(sys.argv[2:]))
