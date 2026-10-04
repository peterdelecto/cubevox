"""Dump routing geometry from a board to JSON for tools/rail_router.py.

Run with KiCad's python: board_dump.py BOARD OUT.json
Writes the board outline, every track, via and pad (as polygons per copper layer), NPTH
holes, and the courtyards of crystals (canon 35 keeps other nets out from under them).
"""
import json
import sys

import pcbnew

MM = pcbnew.ToMM


def poly_points(sps):
    out = []
    for i in range(sps.OutlineCount()):
        o = sps.Outline(i)
        out.append([(MM(o.CPoint(j).x), MM(o.CPoint(j).y)) for j in range(o.PointCount())])
    return out


def main(board_path, out_path):
    b = pcbnew.LoadBoard(board_path)
    layers = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu}
    outline = pcbnew.SHAPE_POLY_SET()
    b.GetBoardPolygonOutlines(outline, True)
    data = {"outline": poly_points(outline), "tracks": [], "vias": [], "pads": [],
            "npth": [], "keepouts": [], "rule_areas": []}
    for z in b.Zones():
        if z.GetIsRuleArea() and z.GetDoNotAllowTracks():
            for name, lid in layers.items():
                if z.IsOnLayer(lid):
                    data["rule_areas"].append({"ref": "", "layer": name, "poly": poly_points(z.Outline())})
    for t in b.GetTracks():
        if t.GetClass() == "PCB_VIA":
            data["vias"].append({"net": t.GetNetname(), "x": MM(t.GetPosition().x),
                                 "y": MM(t.GetPosition().y), "d": MM(t.GetWidth(pcbnew.F_Cu))})
        elif t.GetLayerName() in layers:
            data["tracks"].append({"net": t.GetNetname(), "layer": t.GetLayerName(),
                                   "a": (MM(t.GetStart().x), MM(t.GetStart().y)),
                                   "b": (MM(t.GetEnd().x), MM(t.GetEnd().y)),
                                   "w": MM(t.GetWidth())})
    for fp in b.GetFootprints():
        ref = fp.GetReference()
        for p in fp.Pads():
            if p.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH:
                data["npth"].append({"x": MM(p.GetPosition().x), "y": MM(p.GetPosition().y),
                                     "d": MM(max(p.GetDrillSize().x, p.GetDrillSize().y))})
                continue
            for name, lid in layers.items():
                if not p.IsOnLayer(lid):
                    continue
                data["pads"].append({"ref": ref, "num": p.GetNumber(), "net": p.GetNetname(),
                                     "layer": name, "x": MM(p.GetPosition().x),
                                     "y": MM(p.GetPosition().y),
                                     "poly": poly_points(p.GetEffectivePolygon(lid))})
        for z in fp.Zones():
            if z.GetIsRuleArea() and z.GetDoNotAllowTracks():
                for name, lid in layers.items():
                    if z.IsOnLayer(lid):
                        data["rule_areas"].append({"ref": ref, "layer": name,
                                                   "poly": poly_points(z.Outline())})
        if fp.GetValue().upper().find("MHZ") >= 0 or ref.startswith("Y"):
            bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
            data["keepouts"].append({"ref": ref, "box": (MM(bb.GetLeft()), MM(bb.GetTop()),
                                                         MM(bb.GetRight()), MM(bb.GetBottom()))})
    json.dump(data, open(out_path, "w"))
    print("dumped", len(data["tracks"]), "tracks", len(data["vias"]), "vias",
          len(data["pads"]), "pad-layers")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
