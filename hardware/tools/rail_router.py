"""Grid router for rail trees on one copper layer, writing a foreman edit plan.

Run with a python that has numpy and scipy (not KiCad's):
    rail_router.py GEOM.json --net /3V3 --width 0.4 [--layer B.Cu] [--source X,Y] --out PLAN.json

GEOM.json comes from tools/board_dump.py. Nodes are the net's vias plus its through-hole
pads on the layer. The tree grows from the source (default: the node nearest the first
--source point) by connecting the nearest unconnected node to the nearest tree node, node
to node, so every branch meets at a via or pad centre and no trace ends mid-run.

Geometry follows the foreman gates: 45 degree moves only, every end leaves its via or pad
on an axis with a straight of at least END_LEG_MM (canon 132), runs between bends of at
least MIN_RUN_MM, clearances from the board's other-net copper, NPTH holes, the edge and
crystal courtyards (canon 35).
"""
import argparse
import heapq
import json
import math

import numpy as np
from scipy import ndimage

RES = 0.1                 # mm per grid cell
CLEARANCE = 0.2           # copper to copper
HOLE_CLEARANCE = 0.25     # copper to NPTH drill
EDGE = 0.3                # copper to board edge
MARGIN = 0.08             # covers the end-alignment shift (at most RES / 2)
END_LEG_MM = 0.8          # straight leaving a via or pad (canon 132, edit's mitre floor)
MIN_RUN_MM = 0.6          # shortest diagonal
AXIS_RUN_MM = 3.1         # shortest straight between two diagonals: shorter is a jog (canon 136)
DIAG_MAX_MM = 6.0         # longest diagonal without a recorded reason (canon 136)
TURN_COST_MM = 2.0        # discourages jogs (canon 136)
HEUR_WEIGHT = 1.4         # weighted A*: near-shortest paths at a fraction of the search
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]


class Grid(object):
    def __init__(self, geom):
        pts = [p for ring in geom["outline"] for p in ring]
        self.x0 = math.floor(min(p[0] for p in pts)) - 1
        self.y0 = math.floor(min(p[1] for p in pts)) - 1
        self.nx = int((max(p[0] for p in pts) - self.x0) / RES) + 20
        self.ny = int((max(p[1] for p in pts) - self.y0) / RES) + 20

    def ij(self, x, y):
        return int(round((x - self.x0) / RES)), int(round((y - self.y0) / RES))

    def xy(self, i, j):
        return self.x0 + i * RES, self.y0 + j * RES


def _inside(ring, X, Y):
    inside = np.zeros(X.shape, bool)
    n = len(ring)
    for k in range(n):
        x1, y1 = ring[k]
        x2, y2 = ring[(k + 1) % n]
        cond = (y1 > Y) != (y2 > Y)
        with np.errstate(divide="ignore", invalid="ignore"):
            xint = (x2 - x1) * (Y - y1) / (y2 - y1) + x1
        inside ^= cond & (X < xint)
    return inside


def _window(g, x0, y0, x1, y1):
    i0, j0 = g.ij(x0, y0)
    i1, j1 = g.ij(x1, y1)
    i0, j0 = max(i0 - 1, 0), max(j0 - 1, 0)
    i1, j1 = min(i1 + 2, g.nx), min(j1 + 2, g.ny)
    I, J = np.meshgrid(np.arange(i0, i1), np.arange(j0, j1), indexing="ij")
    return (slice(i0, i1), slice(j0, j1)), g.x0 + I * RES, g.y0 + J * RES


def mark_capsule(g, grid, a, b, r):
    sl, X, Y = _window(g, min(a[0], b[0]) - r, min(a[1], b[1]) - r,
                       max(a[0], b[0]) + r, max(a[1], b[1]) + r)
    ax, ay = a
    dx, dy = b[0] - ax, b[1] - ay
    L2 = dx * dx + dy * dy
    t = np.zeros(X.shape) if L2 == 0 else np.clip(((X - ax) * dx + (Y - ay) * dy) / L2, 0, 1)
    d2 = (X - ax - t * dx) ** 2 + (Y - ay - t * dy) ** 2
    grid[sl] |= d2 < r * r


def mark_poly(g, grid, rings, r):
    for ring in rings:
        xs = [p[0] for p in ring]
        ys = [p[1] for p in ring]
        sl, X, Y = _window(g, min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r)
        inside = _inside(ring, X, Y)
        if r > 0:
            rad = int(math.ceil(r / RES))
            yy, xx = np.mgrid[-rad:rad + 1, -rad:rad + 1]
            disk = (xx * xx + yy * yy) * RES * RES <= r * r
            inside = ndimage.binary_dilation(inside, structure=disk)
        grid[sl] |= inside


def build_obstacles(g, geom, net, layer, width, node_pts):
    """Cells a trace centre of `width` on `layer` may not occupy."""
    occ = np.zeros((g.nx, g.ny), bool)
    half = width / 2.0
    for t in geom["tracks"]:
        if t["layer"] != layer:
            continue
        gap = CLEARANCE if t["net"] != net else 0.05
        mark_capsule(g, occ, t["a"], t["b"], t["w"] / 2 + gap + half + MARGIN)
    for v in geom["vias"]:
        if v["net"] == net and (round(v["x"], 3), round(v["y"], 3)) in node_pts:
            continue
        gap = CLEARANCE if v["net"] != net else 0.05
        mark_capsule(g, occ, (v["x"], v["y"]), (v["x"], v["y"]), v["d"] / 2 + gap + half + MARGIN)
    for p in geom["pads"]:
        if p["layer"] != layer:
            continue
        if p["net"] == net and (round(p["x"], 3), round(p["y"], 3)) in node_pts:
            continue
        gap = CLEARANCE if p["net"] != net else 0.05
        mark_poly(g, occ, p["poly"], gap + half + MARGIN)
    for h in geom["npth"]:
        mark_capsule(g, occ, (h["x"], h["y"]), (h["x"], h["y"]), h["d"] / 2 + HOLE_CLEARANCE + half + MARGIN)
    for ra in geom.get("rule_areas", []):
        if ra["layer"] == layer:
            mark_poly(g, occ, ra["poly"], half + MARGIN)
    for k in geom["keepouts"]:
        x0, y0, x1, y1 = k["box"]
        sl, X, Y = _window(g, x0 - half, y0 - half, x1 + half, y1 + half)
        occ[sl] |= (X > x0 - half) & (X < x1 + half) & (Y > y0 - half) & (Y < y1 + half)
    I, J = np.meshgrid(np.arange(g.nx), np.arange(g.ny), indexing="ij")
    X, Y = g.x0 + I * RES, g.y0 + J * RES
    inside = np.zeros((g.nx, g.ny), bool)
    for ring in geom["outline"]:
        inside ^= _inside(ring, X, Y)
    dist = ndimage.distance_transform_edt(inside) * RES
    occ |= dist < EDGE + half + MARGIN
    return occ


def node_keepclear(g, occ, node, width):
    """The node's own copper (pad or via) is free for its own net."""
    i, j = g.ij(node[0], node[1])
    rad = int(math.ceil((node[2] / 2) / RES))
    occ[max(i - rad, 0):i + rad + 1, max(j - rad, 0):j + rad + 1] = False


def leg_clear(occ, i, j, di, dj, n):
    for k in range(1, n + 1):
        a, b = i + di * k, j + dj * k
        if a < 0 or b < 0 or a >= occ.shape[0] or b >= occ.shape[1] or occ[a, b]:
            return False
    return True


def astar(g, occ, start, goals, max_states=300_000, base=None, goal_pad=0.6):
    """start: (x, y, size). goals: list of (x, y, size). Returns (polyline, goal) or None."""
    leg = int(math.ceil(END_LEG_MM / RES))
    run_min = int(math.ceil(MIN_RUN_MM / RES))
    si, sj = g.ij(start[0], start[1])
    goal_cells = [(g.ij(q[0], q[1]), q) for q in goals]

    def h(i, j):
        best = 1e18
        for (gi, gj), _q in goal_cells:
            dx, dy = abs(i - gi), abs(j - gj)
            best = min(best, (max(dx, dy) - min(dx, dy)) + math.sqrt(2) * min(dx, dy))
        return best * RES

    openq, came, cost = [], {}, {}
    axis_run = int(math.ceil(AXIS_RUN_MM / RES))
    diag_lens = list(range(int(math.ceil(MIN_RUN_MM / RES / math.sqrt(2))),
                           int(DIAG_MAX_MM / RES / math.sqrt(2)) + 1, 2))

    def walk(i, j, di, dj, n):
        for _k in range(n):
            ni, nj = i + di, j + dj
            if (ni < 0 or nj < 0 or ni >= g.nx or nj >= g.ny or occ[ni, nj]
                    or (di and dj and (occ[i + di, j] or occ[i, j + dj]))):
                return None
            i, j = ni, nj
        return i, j

    def goal_on_axis(i, j, d):
        di, dj = DIRS[d]
        for (gi, gj), q in goal_cells:
            along = (gi - i) * di + (gj - j) * dj
            perp = (gi - i) * dj - (gj - j) * di
            near = int(math.ceil((q[2] / 2 + goal_pad) / RES)) + 1   # the goal's own blocked disk
            if perp == 0 and along >= leg and leg_clear(occ, i, j, di, dj, max(along - near, 0)):
                bi, bj = i + di * max(along - near, 0), j + dj * max(along - near, 0)
                if base is None or leg_clear(base, bi, bj, di, dj, along - max(along - near, 0) - 1):
                    return q
        return None

    def push(ns, nc, prev):
        if nc < cost.get(ns, 1e18):
            cost[ns] = nc
            came[ns] = prev
            heapq.heappush(openq, (nc + HEUR_WEIGHT * h(ns[0], ns[1]), ns))

    for d in (0, 2, 4, 6):
        di, dj = DIRS[d]
        if leg_clear(occ, si, sj, di, dj, leg):
            push((si + di * leg, sj + dj * leg, d), leg * RES, None)
    seen, closed = 0, set()
    while openq:
        _f, st = heapq.heappop(openq)
        if st in closed:
            continue
        closed.add(st)
        seen += 1
        if seen > max_states:
            return None
        i, j, d = st
        c0 = cost[st]
        if d % 2 == 0:
            q = goal_on_axis(i, j, d)
            if q is not None:
                return _polyline(g, came, st, start, q), q
            di, dj = DIRS[d]
            nxt = walk(i, j, di, dj, 1)
            if nxt:
                push((nxt[0], nxt[1], d), c0 + RES, st)
            for nd in ((d + 1) % 8, (d - 1) % 8):
                ndi, ndj = DIRS[nd]
                for n in diag_lens:
                    nxt = walk(i, j, ndi, ndj, n)
                    if nxt is None:
                        break
                    push((nxt[0], nxt[1], nd), c0 + n * RES * math.sqrt(2) + TURN_COST_MM, st)
        else:
            for nd in ((d + 1) % 8, (d - 1) % 8):
                ndi, ndj = DIRS[nd]
                q = goal_on_axis(i, j, nd)
                if q is not None:
                    return _polyline(g, came, (i, j, d), start, q), q
                nxt = walk(i, j, ndi, ndj, axis_run)
                if nxt:
                    push((nxt[0], nxt[1], nd), c0 + axis_run * RES + TURN_COST_MM, st)
    return None


def _polyline(g, came, st, start, goal):
    cells = []
    while st is not None:
        cells.append(st)
        st = came[st]
    cells.reverse()
    pts = [(start[0], start[1])]
    prev_d = cells[0][2]
    for k in range(1, len(cells)):
        if cells[k][2] != prev_d:
            pts.append(g.xy(cells[k - 1][0], cells[k - 1][1]))
            prev_d = cells[k][2]
    pts.append(g.xy(cells[-1][0], cells[-1][1]))
    pts.append((goal[0], goal[1]))
    pts = _merge_collinear(pts)
    pts = _fix_end(pts)
    pts = list(reversed(_fix_end(list(reversed(pts)))))
    return [(round(x, 4), round(y, 4)) for x, y in pts]


def _merge_collinear(pts):
    out = [pts[0]]
    for k in range(1, len(pts) - 1):
        a, b, c = out[-1], pts[k], pts[k + 1]
        cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])
        span = math.hypot(c[0] - a[0], c[1] - a[1]) or 1.0
        if abs(cross) / span > RES * 0.75:
            out.append(b)
    out.append(pts[-1])
    return out


def _fix_end(pts):
    """pts[0] is exact; make pts[0]->pts[1] exactly axis-aligned by sliding pts[1] along
    the segment that follows it, keeping that segment's angle."""
    if len(pts) < 3:
        return pts
    (x0, y0), (x1, y1), (x2, y2) = pts[0], pts[1], pts[2]
    horizontal = abs(y1 - y0) < abs(x1 - x0)
    dx2, dy2 = x2 - x1, y2 - y1
    if horizontal:
        delta = y0 - y1
        if abs(dy2) < 1e-9 or abs(delta) < 1e-9:
            pts[1] = (x1, y0)
        else:
            pts[1] = (x1 + delta * dx2 / dy2, y0) if abs(dx2) > 1e-9 else (x1, y0)
            if abs(dx2) < 1e-9:
                pts[1] = (x1, y0)
    else:
        delta = x0 - x1
        if abs(dx2) < 1e-9 or abs(delta) < 1e-9:
            pts[1] = (x0, y1)
        else:
            pts[1] = (x0, y1 + delta * dy2 / dx2) if abs(dy2) > 1e-9 else (x0, y1)
    return pts


def nodes_for(geom, net, layer):
    out = []
    for v in geom["vias"]:
        if v["net"] == net:
            out.append((round(v["x"], 3), round(v["y"], 3), v["d"], "via"))
    for p in geom["pads"]:
        if p["net"] == net and p["layer"] == layer and len(p["poly"]) and _is_th(geom, p):
            xs = [q[0] for ring in p["poly"] for q in ring]
            out.append((round(p["x"], 3), round(p["y"], 3), min(max(xs) - min(xs), 1.2),
                        "%s.%s" % (p["ref"], p["num"])))
    return out


def groups_for(geom, net, nodes):
    """Union nodes already joined by this net's existing copper (tracks and pads on any
    layer). The tree reaches each group once, so it never closes a loop (canon 133)."""
    parent = list(range(len(nodes)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    pts = []                         # (x, y, owner) for every copper point of the net
    for k, n in enumerate(nodes):
        pts.append((n[0], n[1], ("n", k)))
    tracks = [t for t in geom["tracks"] if t["net"] == net]
    pads = [p for p in geom["pads"] if p["net"] == net]
    items = [("t", i) for i in range(len(tracks))] + [("p", i) for i in range(len(pads))]
    idx = {it: len(nodes) + i for i, it in enumerate(items)}
    parent.extend(range(len(nodes), len(nodes) + len(items)))

    def touch(x, y):
        hits = [k for k, n in enumerate(nodes) if math.hypot(n[0] - x, n[1] - y) < 0.05]
        for i, p in enumerate(pads):
            for ring in p["poly"]:
                if _inside(ring, np.array([x]), np.array([y]))[0]:
                    hits.append(idx[("p", i)])
        for i, t in enumerate(tracks):
            for e in (t["a"], t["b"]):
                if math.hypot(e[0] - x, e[1] - y) < 0.02:
                    hits.append(idx[("t", i)])
        return hits

    for i, t in enumerate(tracks):
        for e in (t["a"], t["b"]):
            for h in touch(e[0], e[1]):
                parent[find(h)] = find(idx[("t", i)])
    for i, p in enumerate(pads):
        for k, n in enumerate(nodes):
            if any(_inside(ring, np.array([n[0]]), np.array([n[1]]))[0] for ring in p["poly"]):
                parent[find(k)] = find(idx[("p", i)])
    return [find(k) for k in range(len(nodes))]


def _redundant_vias(geom, net, layer, nodes, plan, group, joined):
    """A via is spare when no trace on the routing layer reaches it and another via of the
    same other-layer island does. Islands ignore routing-layer copper, which is what the
    vias connect to."""
    ends = [tuple(t["a"]) for t in geom["tracks"] if t["net"] == net and t["layer"] == layer]
    ends += [tuple(t["b"]) for t in geom["tracks"] if t["net"] == net and t["layer"] == layer]
    for op in plan:
        ends += [tuple(op["points"][0]), tuple(op["points"][-1])]

    def used(n):
        return any(math.hypot(n[0] - x, n[1] - y) < 0.05 for x, y in ends)

    other = dict(geom, tracks=[t for t in geom["tracks"] if t["layer"] != layer],
                 pads=[p for p in geom["pads"] if p["layer"] != layer])
    island = dict(zip(nodes, groups_for(other, net, nodes)))
    fed = {island[n] for n in nodes if n[3] == "via" and used(n)}
    return [n for n in nodes if n[3] == "via" and group[n] in joined and not used(n)
            and island[n] in fed]


def _is_th(geom, p):
    return any(q["ref"] == p["ref"] and q["num"] == p["num"] and q["layer"] != p["layer"]
               for q in geom["pads"])


def route_net(geom, net, layer, width, sources, only=None, skip=()):
    g = Grid(geom)
    nodes = [n for n in nodes_for(geom, net, layer) if (n[0], n[1]) not in skip]
    if only:
        nodes = [n for n in nodes if any(math.hypot(n[0] - x, n[1] - y) < 0.05 for x, y in only)
                 or any(math.hypot(n[0] - x, n[1] - y) < 0.05 for x, y in sources)]
    keys = {(n[0], n[1]) for n in nodes}
    occ = build_obstacles(g, geom, net, layer, width, keys)
    for n in nodes:
        node_keepclear(g, occ, n, width)
    group = dict(zip(nodes, groups_for(geom, net, nodes)))
    tree = []
    for sx, sy in sources:
        tree.append(min(nodes, key=lambda n: math.hypot(n[0] - sx, n[1] - sy)))
    joined = {group[t] for t in tree}

    # Existing copper already joined to a source is part of the tree, so new legs may end on it.

    tree += [n for n in nodes if n not in tree and group[n] in joined]
    rest = [n for n in nodes if group[n] not in joined]
    same = np.zeros_like(occ)          # this net's new traces
    halo = np.zeros_like(occ)          # around tree nodes, where new traces may meet them
    plan, failed, tried = [], [], {}

    def add_halo(n):
        i, j = g.ij(n[0], n[1])
        rad = int(math.ceil((n[2] / 2 + width + 0.1) / RES))
        yy, xx = np.mgrid[-rad:rad + 1, -rad:rad + 1]
        disk = (xx * xx + yy * yy) <= rad * rad
        sl = (slice(i - rad, i + rad + 1), slice(j - rad, j + rad + 1))
        halo[sl] |= disk

    for t in tree:
        add_halo(t)
    while rest:
        rest.sort(key=lambda n: min(math.hypot(n[0] - t[0], n[1] - t[1]) for t in tree))
        progress = False
        for n in list(rest):
            if tried.get(n) == len(tree):
                continue
            near = sorted(tree, key=lambda t: math.hypot(n[0] - t[0], n[1] - t[1]))[:4]
            ends = set(near) | {n}
            local = np.zeros_like(occ)
            for t in ends:
                i, j = g.ij(t[0], t[1])
                rad = int(math.ceil((t[2] / 2 + width + 0.1) / RES))
                yy, xx = np.mgrid[-rad:rad + 1, -rad:rad + 1]
                local[i - rad:i + rad + 1, j - rad:j + rad + 1] |= (xx * xx + yy * yy) <= rad * rad
            eff = occ | (same & ~local)
            for o in nodes:
                if o != n:
                    mark_capsule(g, eff, (o[0], o[1]), (o[0], o[1]), o[2] / 2 + width / 2 + 0.1 + MARGIN)
            res = astar(g, eff, n, near, base=occ | (same & ~local),
                        goal_pad=width / 2 + 0.1 + MARGIN)
            if res is None:
                tried[n] = len(tree)
                continue
            pts, _goal = res
            plan.append({"op": "track", "net": net, "layer": layer, "width_mm": width,
                         "points": [list(p) for p in pts]})
            for a, b in zip(pts, pts[1:]):
                mark_capsule(g, same, a, b, width + 0.05 + MARGIN)
            tree.append(n)
            add_halo(n)
            joined.add(group[n])
            rest = [r for r in rest if group[r] not in joined]
            progress = True
            break
        if not progress:
            failed = rest
            break
    redundant = _redundant_vias(geom, net, layer, nodes, plan, group, joined)
    prep = []
    for n in redundant:
        for t in geom["tracks"]:
            if t["net"] == net and any(math.hypot(e[0] - n[0], e[1] - n[1]) < 0.02 for e in (t["a"], t["b"])):
                prep.append({"op": "remove_track", "layer": t["layer"],
                             "x": round((t["a"][0] + t["b"][0]) / 2, 4),
                             "y": round((t["a"][1] + t["b"][1]) / 2, 4)})
        prep.append({"op": "remove_via", "x": n[0], "y": n[1]})
    return plan, failed, prep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("geom")
    ap.add_argument("--net", required=True)
    ap.add_argument("--layer", default="B.Cu")
    ap.add_argument("--width", type=float, default=0.4)
    ap.add_argument("--source", action="append", required=True, help="X,Y of a tree root")
    ap.add_argument("--only", action="append", default=[], help="X,Y: route only these nodes")
    ap.add_argument("--skip", action="append", default=[], help="X,Y: leave this node out")
    ap.add_argument("--out", required=True, help="plan; redundant-via removals go to <out>-redundant.json")
    a = ap.parse_args()
    parse = lambda s: tuple(round(float(v), 3) for v in s.split(","))
    plan, failed, prep = route_net(json.load(open(a.geom)), a.net, a.layer, a.width,
                             [parse(s) for s in a.source], [parse(s) for s in a.only],
                             {parse(s) for s in a.skip})
    json.dump(plan, open(a.out, "w"), indent=1)
    json.dump(prep, open(a.out.replace(".json", "-redundant.json"), "w"), indent=1)
    print("redundant vias", sum(1 for o in prep if o["op"] == "remove_via"))
    print("routed", len(plan), "connections;", "failed", len(failed),
          [(n[0], n[1], n[3]) for n in failed])


if __name__ == "__main__":
    main()
