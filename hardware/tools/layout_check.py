"""Overlap check over a foreman schematic spec (grid units).

Estimates the boxes of symbol bodies, stub labels, reference/value texts, notes and
sheet symbols, then lists every pair that overlaps. Used before rendering.
Run: PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=R python3 layout_check.py spec.json boarddir
"""
import json
import os
import sys

from foreman import schematic as S

G = 1.27
CW = 1.27     # grid units per character (labels, mixed case)
TH = 1.6      # text height box


def box(x0, y0, x1, y1):
    return (min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1))


def hit(a, b, tol=0.05):
    return a[0] < b[2] - tol and b[0] < a[2] - tol and a[1] < b[3] - tol and b[1] < a[3] - tol


def check(spec_path, board_dir, own_parts=None):
    spec = S.load_spec(spec_path)
    texts = S._library_texts(spec, board_dir)
    pins, bodies = {}, {}
    for lib, name in spec["lib_symbols"]:
        blk = S.find_sym_block(texts[lib], name)
        pins["%s:%s" % (lib, name)] = S.parse_pins(blk)
        bodies["%s:%s" % (lib, name)] = S.parse_body(blk)
    items = []   # (owner, kind, box)
    for part in spec["parts"]:
        lp, body = pins[part["lib_id"]], bodies[part["lib_id"]]
        x, y = part["at"]
        ang = int(part.get("angle", 0))
        if body:
            (lx0, ly0, lx1, ly1), _ = body
            a = S.place((lx0 / G, ly0 / G), x, y, ang)
            b = S.place((lx1 / G, ly1 / G), x, y, ang)
            items.append((part["ref"], "body", box(a[0], a[1], b[0], b[1])))
        rows = {r["pin"]: r for r in part.get("pins", [])}
        sdirs = set()
        for n, r in rows.items():
            if r.get("net"):
                px, py = S.place((lp[n][0] / G, lp[n][1] / G), x, y, ang)
                d = S._pin_direction(r.get("dir"), part.get("dir"), lp[n])
                sdirs.add(tuple(d))
                ln = r.get("len") or S.DEFAULT_STUB_LEN
                ex, ey = px + d[0] * ln, py + d[1] * ln
                w = len(r["net"]) * CW + 1.5
                if d[1] == 0:
                    items.append((part["ref"], "label:" + r["net"],
                                  box(ex, ey - 1.0, ex + d[0] * w, ey + 0.2)))
                else:
                    items.append((part["ref"], "label:" + r["net"],
                                  box(ex - 1.0, ey, ex + 0.2, ey + d[1] * w)))
        tp = S.text_positions(body, x * G, y * G, ang, G, sdirs)
        if tp:
            (rx, ry), (vx, vy), just = tp
            for txt, (tx, ty) in ((part["ref"], (rx, ry)), (part.get("value", ""), (vx, vy))):
                if part["ref"].startswith("H"):
                    continue
                w = len(txt) * CW
                tx, ty = tx / G, ty / G
                if just == "left":
                    bb = box(tx, ty - 0.8, tx + w, ty + 0.8)
                elif just == "right":
                    bb = box(tx - w, ty - 0.8, tx, ty + 0.8)
                else:
                    bb = box(tx - w / 2, ty - 0.8, tx + w / 2, ty + 0.8)
                items.append((part["ref"], "text:" + txt, bb))
    for row in spec.get("power_symbols", []):
        x, y = row["at"]
        st = row.get("stub")
        if st:
            d = st.get("dir", (1, 0))
            ln = st.get("len") or S.DEFAULT_STUB_LEN
            ex, ey = x + d[0] * ln, y + d[1] * ln
            w = len(st["net"]) * CW + 1.5
            if d[1] == 0:
                items.append(("PWR" + st["net"], "label", box(ex, ey - 1, ex + d[0] * w, ey + 0.2)))
            else:
                items.append(("PWR" + st["net"], "label", box(ex - 1, ey, ex + 0.2, ey + d[1] * w)))
            items.append(("PWR" + st["net"], "body", box(x - 1, y - 1, x + 1, y + 1)))
    for i, note in enumerate(spec.get("texts", [])):
        x, y = note["at"]
        items.append(("NOTE%d" % i, "note:" + note["text"][:20],
                      box(x, y - 1.2, x + len(note["text"]) * CW, y + 0.2)))
    for row in spec.get("sheets", []):
        x, y = row["at"]
        w, h = row["size"]
        items.append((row["name"], "sheet", box(x, y, x + w, y + h)))
        for p in row["pins"]:
            d = (1, 0) if p["side"] == "right" else (-1, 0)
            px, py = x + (w if p["side"] == "right" else 0), y + p["offset"]
            if p.get("no_connect"):
                continue
            ln = p.get("len") or S.DEFAULT_STUB_LEN
            net = p.get("net", p["name"])
            ex = px + d[0] * ln
            wid = len(net) * CW + 1.5
            items.append((row["name"], "label:" + net,
                          box(ex, py - 1.0, ex + d[0] * wid, py + 0.2)))
            nm = len(p["name"]) * CW
            items.append((row["name"], "pinname", box(px - nm, py - 0.8, px, py + 0.8)))
    out = []
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            a, b = items[i], items[j]
            if a[0] == b[0] and not (a[1].startswith(("text", "label")) and b[1].startswith(("text", "label"))):
                continue
            if hit(a[2], b[2]):
                out.append("%s %s  X  %s %s  at (%.0f,%.0f)" % (a[0], a[1], b[0], b[1], a[2][0], a[2][1]))
    ext = [min(b[2][0] for b in items), min(b[2][1] for b in items),
           max(b[2][2] for b in items), max(b[2][3] for b in items)]
    return out, ext


if __name__ == "__main__":
    res, ext = check(sys.argv[1], sys.argv[2])
    print("\n".join(res))
    print("%d overlaps; extent grid x %.0f..%.0f y %.0f..%.0f" % (len(res), ext[0], ext[2], ext[1], ext[3]))
