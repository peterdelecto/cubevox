"""Carry a grouped layout region from one KiCad board into another.

Run with KiCad's bundled Python, never with the board open in pcbnew:

  $PY transfer_layout.py SRC.kicad_pcb DST.kicad_pcb --group NAME --out OUT.kicad_pcb
      [--sheet SRC_BLOCK.kicad_sch --src-project P --dst-sheet DST_BLOCK.kicad_sch
       --dst-project Q] [--offset DX,DY]

Footprints are matched through the block sheet's symbol UUIDs. Each symbol
records one reference per project, so a harness ref maps to the parent's
re-annotated ref without any board carrying a symbol path. With no sheet
arguments the references are taken as equal.

A footprint the destination already has is moved onto the source pose. A
footprint it lacks is copied in. Tracks, vias, zones and drawings are copied.

Nets follow the pads. For every matched pad the source net maps to the
destination pad's net, so a port net renamed by the parent still lands on
the right copper. A source net whose pads now land on two destination nets
was rewired by a schematic change; the run reports it and refuses only when
copied copper sits on it. A copied item whose net no pad maps falls back to the same
net name, and the run refuses if that name is absent. A board keeps no net
without copper on it, so a footprint copied into a bare board creates its
pad nets by name.

Exit 0 on success, 2 on any unmatched footprint, footprint library mismatch,
copper on a rewired net, or a missing net.
"""

import argparse
import re
import sys

import pcbnew

SYMBOL_RE = re.compile(r"\n\t\(symbol\n(.*?)\n\t\)", re.S)
UUID_RE = re.compile(r"\n\t\t\(uuid \"([^\"]+)\"\)")
INSTANCE_RE = re.compile(
    r"\(project \"([^\"]+)\"\s*\(path \"([^\"]+)\"\s*\(reference \"([^\"]+)\"")


def fail(msg):
    print("[ERROR] " + msg)
    sys.exit(2)


def refs_by_uuid(sheet, project):
    """Symbol UUID -> reference for one project's instance of the block."""
    text = open(sheet).read()
    out = {}
    for body in SYMBOL_RE.findall(text):
        uuid = UUID_RE.search("\n" + body)
        if not uuid:
            continue
        refs = {r for p, _, r in INSTANCE_RE.findall(body) if p == project}
        if len(refs) > 1:
            fail("symbol %s has %d instances in project %s; one block "
                 "instance per board is supported" % (uuid.group(1), len(refs), project))
        if refs:
            out[uuid.group(1)] = refs.pop()
    if not out:
        fail("no symbol instances for project %s in %s" % (project, sheet))
    return out


def ref_map(args):
    if not args.sheet:
        return None
    src = refs_by_uuid(args.sheet, args.src_project)
    dst = refs_by_uuid(args.dst_sheet or args.sheet, args.dst_project)
    return {src[u]: dst[u] for u in src if u in dst}


def find_group(board, name):
    for g in board.Groups():
        if g.GetName() == name:
            return g
    return None


def ensure_net(dst, name, create=False):
    net = dst.FindNet(name)
    if net is None and create:
        net = pcbnew.NETINFO_ITEM(dst, name)
        dst.Add(net)
    if net is None:
        fail("net %s is absent from the destination" % name)
    return net


def place_footprint(fp, dst, dst_fps, refs, delta):
    """Move the destination's matching footprint onto fp's pose, or copy fp in."""
    ref = refs.get(fp.GetReference()) if refs is not None else fp.GetReference()
    if ref is None:
        fail("%s has no reference in the destination project" % fp.GetReference())
    target = dst_fps.get(ref)
    if target is None:
        if refs is not None:
            fail("%s (source %s) is not on the destination board" % (ref, fp.GetReference()))
        target = duplicate(fp)
        dst.Add(target)
        for pad in target.Pads():
            pad.SetNet(ensure_net(dst, pad.GetNetname(), create=True))
    # Compare the footprint name only. The library nickname differs per project
    # (h7core: vs cubevox_h7core:) while the footprint itself is the same.
    if target.GetFPID().GetLibItemName() != fp.GetFPID().GetLibItemName():
        fail("%s is %s here but %s in the source; the sheets have drifted"
             % (ref, target.GetFPIDAsString(), fp.GetFPIDAsString()))
    if target.IsFlipped() != fp.IsFlipped():
        target.Flip(target.GetPosition(), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
    target.SetOrientation(fp.GetOrientation())
    target.SetPosition(fp.GetPosition() + delta)
    target.SetLocked(fp.IsLocked())
    return target


def map_pad_nets(fp, target, nets):
    dst_pads = {p.GetNumber(): p for p in target.Pads()}
    for pad in fp.Pads():
        src_name = pad.GetNetname()
        if not src_name or src_name.startswith("unconnected-"):
            continue
        other = dst_pads.get(pad.GetNumber())
        if other is None:
            fail("%s pad %s missing on %s" % (fp.GetReference(), pad.GetNumber(),
                                             target.GetReference()))
        nets.setdefault(src_name, set()).add(other.GetNetname())


def duplicate(item):
    """A fresh-UUID copy. Footprint and zone bindings take addToParentGroup."""
    if isinstance(item, (pcbnew.FOOTPRINT, pcbnew.ZONE)):
        return item.Duplicate(False).Cast()
    return item.Duplicate().Cast()


def copy_item(item, dst, nets, delta, fallbacks):
    dup = duplicate(item)
    if isinstance(dup, pcbnew.BOARD_CONNECTED_ITEM):
        src_name = item.GetNetname()
        targets = nets.get(src_name)
        if targets is None:
            dst_name = src_name
            if src_name:
                fallbacks.add(src_name)
        elif len(targets) > 1:
            fail("copper on net %s, which the pads map to %s; the schematic rewired it"
                 % (src_name, " and ".join(sorted(targets))))
        else:
            dst_name = next(iter(targets))
        dup.SetNet(ensure_net(dst, dst_name))
    dup.Move(delta)
    dst.Add(dup)
    return dup


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--group", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sheet")
    ap.add_argument("--src-project")
    ap.add_argument("--dst-sheet")
    ap.add_argument("--dst-project")
    ap.add_argument("--offset", default="0,0", help="mm, applied to every item")
    ap.add_argument("--exclude", default="", help="REF,REF,... source footprints to skip")
    args = ap.parse_args()
    if args.sheet and not (args.src_project and args.dst_project):
        fail("--sheet needs --src-project and --dst-project")

    src = pcbnew.LoadBoard(args.src)
    dst = pcbnew.LoadBoard(args.dst)
    group = find_group(src, args.group)
    if group is None:
        fail("group %s not in %s" % (args.group, args.src))
    if find_group(dst, args.group) is not None:
        fail("group %s already in %s; refusing to stack a second copy" % (args.group, args.dst))

    dx, dy = (pcbnew.FromMM(float(v)) for v in args.offset.split(","))
    delta = pcbnew.VECTOR2I(dx, dy)
    refs = ref_map(args)
    dst_fps = {f.GetReference(): f for f in dst.GetFootprints()}
    items = [i.Cast() for i in group.GetItems()]
    excluded = {r for r in args.exclude.split(",") if r}
    footprints = [i for i in items if isinstance(i, pcbnew.FOOTPRINT)
                  and i.GetReference() not in excluded]
    others = [i for i in items if not isinstance(i, pcbnew.FOOTPRINT)]

    new_group = pcbnew.PCB_GROUP(dst)
    new_group.SetName(args.group)
    dst.Add(new_group)
    nets = {}
    for fp in footprints:
        target = place_footprint(fp, dst, dst_fps, refs, delta)
        map_pad_nets(fp, target, nets)
        new_group.AddItem(target)
    fallbacks = set()
    for item in others:
        new_group.AddItem(copy_item(item, dst, nets, delta, fallbacks))

    pcbnew.SaveBoard(args.out, dst)
    print("footprints %d  other items %d  nets mapped by pad %d  by name %d"
          % (len(footprints), len(others), len(nets), len(fallbacks)))
    for s in sorted(nets):
        if len(nets[s]) > 1:
            print("  rewired, no copper carried: %s -> %s" % (s, ", ".join(sorted(nets[s]))))
        elif nets[s] != {s}:
            print("  renamed %s -> %s" % (s, next(iter(nets[s]))))
    for s in sorted(fallbacks):
        print("  by name %s" % s)


if __name__ == "__main__":
    main()
