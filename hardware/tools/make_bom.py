#!/usr/bin/env python3
"""JLC BOM (and CPL) from the KiCad netlist and position export.

Usage (repo root):
  python3 hardware/tools/make_bom.py hardware/exports/cubevox-netlist.net \
      hardware/production/cubevox-bom.csv \
      [--cpl RAW_POS.csv hardware/production/cubevox-cpl.csv]

BOM rows group by LCSC number: Comment, Designator, Footprint, LCSC Part #.
Skipped: symbols marked exclude-from-BOM or DNP, and the owner-soldered jacks.
CPL: the kicad-cli `pcb export pos --format csv --units mm --side front --exclude-dnp`
file with JLC's column names, restricted to the BOM's designators.
Exit 1 when a BOM part has no LCSC number or the BOM and CPL designators differ.
"""
import csv
import re
import sys
from collections import OrderedDict

OWNER_SOLDERED = {"J101", "J106", "J108"}


def parse_sexpr(text):
    tokens = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack, cur = [], []
    for tok in tokens:
        if tok == "(":
            stack.append(cur)
            cur = []
        elif tok == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(tok[1:-1].replace('\\"', '"') if tok.startswith('"') else tok)
    return cur[0]


def children(node, key):
    return [c for c in node[1:] if isinstance(c, list) and c and c[0] == key]


def value_of(node, key):
    found = children(node, key)
    return found[0][1] if found and len(found[0]) > 1 else None


def components(netlist_path):
    root = parse_sexpr(open(netlist_path, encoding="utf-8").read())
    comps = children(root, "components")[0]
    for comp in children(comps, "comp"):
        props = {}
        for p in children(comp, "property"):
            name = value_of(p, "name")
            props[name] = value_of(p, "value") or ""
        fields = {}
        for fl in children(comp, "fields"):
            for f in children(fl, "field"):
                name = value_of(f, "name")
                fields[name] = f[2] if len(f) > 2 and not isinstance(f[2], list) else ""
        yield {
            "ref": value_of(comp, "ref"),
            "value": value_of(comp, "value") or "",
            "footprint": (value_of(comp, "footprint") or "").split(":")[-1],
            "lcsc": fields.get("LCSC") or props.get("LCSC") or "",
            "skip": "exclude_from_bom" in props or "dnp" in props,
        }


def ref_key(ref):
    m = re.match(r"([A-Za-z_]+)(\d+)", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def write_bom(netlist_path, out_path):
    groups = OrderedDict()
    missing = []
    for c in components(netlist_path):
        if c["skip"] or c["ref"] in OWNER_SOLDERED:
            continue
        if not c["lcsc"]:
            missing.append(c["ref"])
            continue
        g = groups.setdefault(c["lcsc"], {"values": [], "refs": [], "fps": []})
        for k, v in (("values", c["value"]), ("refs", c["ref"]), ("fps", c["footprint"])):
            if k == "refs" or v not in g[k]:
                g[k].append(v)
    rows = []
    for lcsc, g in groups.items():
        refs = sorted(g["refs"], key=ref_key)
        rows.append([" / ".join(g["values"]), ",".join(refs), " / ".join(g["fps"]), lcsc])
    rows.sort(key=lambda r: ref_key(r[1].split(",")[0]))
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        w.writerows(rows)
    placed = {r for row in rows for r in row[1].split(",")}
    print("BOM: %d lines, %d parts, %d distinct LCSC -> %s" % (len(rows), len(placed), len(groups), out_path))
    if missing:
        print("ERROR: no LCSC number: " + ",".join(sorted(missing, key=ref_key)))
    return placed, not missing


def write_cpl(raw_path, out_path, bom_refs):
    with open(raw_path, newline="", encoding="utf-8") as f:
        raw = list(csv.DictReader(f))
    out = [r for r in raw if r["Ref"] in bom_refs]
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for r in sorted(out, key=lambda r: ref_key(r["Ref"])):
            w.writerow([r["Ref"], r["PosX"], r["PosY"], "Top" if r["Side"] == "top" else "Bottom", r["Rot"]])
    cpl_refs = {r["Ref"] for r in out}
    dropped = sorted({r["Ref"] for r in raw} - bom_refs, key=ref_key)
    print("CPL: %d rows -> %s (dropped from the raw export: %s)" % (len(out), out_path, ",".join(dropped) or "none"))
    unplaced = sorted(bom_refs - cpl_refs, key=ref_key)
    if unplaced:
        print("ERROR: in BOM but not in CPL: " + ",".join(unplaced))
    return not unplaced


def main(argv):
    if len(argv) not in (3, 6) or (len(argv) == 6 and argv[3] != "--cpl"):
        print(__doc__)
        return 2
    bom_refs, ok = write_bom(argv[1], argv[2])
    if len(argv) == 6:
        ok = write_cpl(argv[4], argv[5], bom_refs) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
