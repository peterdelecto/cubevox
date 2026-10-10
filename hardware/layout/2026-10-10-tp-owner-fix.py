"""Tidy the owner-moved test pads TP6, TP10, TP11 (2026-10-10).

  <kicad python3> -I hardware/layout/2026-10-10-tp-owner-fix.py hardware/cubevox.kicad_pcb [--apply]

TP10 and TP11 land on the x of the pad they stub from, each stub becomes one straight
segment. TP6's stub ends at the pad centre with 0.5 mm chamfer legs at the C25.1 lane.
"""
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
Fix = import_module('2026-10-10-prefab-fix').Fix
M = pcbnew.ToMM
F = pcbnew.F_Cu


def pad_xy(b, ref, num):
    p = next(p for p in b.FindFootprintByReference(ref).Pads() if p.GetNumber() == num)
    return M(p.GetPosition().x), M(p.GetPosition().y)


def main(path, apply):
    b = pcbnew.LoadBoard(path)
    x = Fix(b)

    x.say('TP10 VREF_BUF onto U108.1 x')
    ux, uy = pad_xy(b, 'U108', '1')
    tx, ty = pad_xy(b, 'TP10', '1')
    x.rm('/VREF_BUF', F, (159.6, 85.7), (159.6, 87.84))
    x.rm('/VREF_BUF', F, (159.6, 87.84), (ux, 87.8525))
    x.rm('/VREF_BUF', F, (ux, 87.8525), (ux, uy))
    x.move('TP10', ux - tx, 0)
    x.add('/VREF_BUF', F, 0.2, [(ux, ty), (ux, uy)])

    x.say('TP11 VDDA onto C10.1 x')
    cx, cy = pad_xy(b, 'C10', '1')
    tx, ty = pad_xy(b, 'TP11', '1')
    x.rm('/h7core_block/VDDA', F, (cx, cy), (cx, 94.6))
    x.rm('/h7core_block/VDDA', F, (cx, 94.6), (tx, ty))
    x.move('TP11', cx - tx, 0)
    x.add('/h7core_block/VDDA', F, 0.3, [(cx, ty), (cx, cy)])

    x.say('TP6 3V3A stub to centre, 0.5 mm legs')
    tx, ty = pad_xy(b, 'TP6', '1')
    x.rm('/3V3A', F, (143.95, 49.45), (143.7, 49.2))
    x.rm('/3V3A', F, (143.7, 49.2), (143.7, 47.4))
    x.end('/3V3A', F, (148.0, 49.45), (143.95, 49.45), (143.95, 49.45), (144.2, 49.45))
    x.add('/3V3A', F, 0.3, [(144.2, 49.45), (tx, 48.95), (tx, ty)])

    x.commit(apply)


if __name__ == '__main__':
    main(sys.argv[1], '--apply' in sys.argv)
