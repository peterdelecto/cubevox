"""Six rail test pads (owner 2026-10-10): TP6 3V3A, TP7 5V, TP8 5VA, TP9 5VA_W, TP10 VREF_BUF, TP11 VDDA.
Copies TP1's footprint (reference on F.Fab, value hidden). Spots from tools/tp_space.py --try.

  <kicad python3> -I hardware/layout/2026-10-10-testpads.py hardware/cubevox.kicad_pcb --apply
"""
import sys
import pcbnew

M = pcbnew.ToMM
V = lambda x, y: pcbnew.VECTOR2I(int(round(x * 1e6)), int(round(y * 1e6)))
# ref, net, pad centre, stub polyline (None = sits on an existing corner), stub width
PADS = [
    ('TP6', '/3V3A', (150.2, 51.8), [(150.2, 49.45), (150.2, 51.8)], 0.3),
    ('TP7', '/5V', (93.05, 56.4), [(93.05, 54.1), (93.05, 56.4)], 0.4),
    ('TP8', '/5VA', (155.3, 61.7875), [(157.6, 61.7875), (155.3, 61.7875)], 0.3),
    ('TP9', '/5VA_W', (68.5, 77.8), [(68.5, 75.5875), (68.5, 77.8)], 0.3),
    ('TP10', '/VREF_BUF', (156.2, 87.4), None, 0.2),
    ('TP11', '/h7core_block/VDDA', (105.1, 94.325), None, 0.3),
]


def main(path, apply):
    b = pcbnew.LoadBoard(path)
    src = b.FindFootprintByReference('TP1')
    for ref, net, (x, y), stub, w in PADS:
        if b.FindFootprintByReference(ref):
            raise SystemExit(ref + ' exists')
        fp = pcbnew.Cast_to_FOOTPRINT(src.Duplicate(False))
        fp.SetReference(ref)
        fp.SetValue(net.split('/')[-1])
        fp.SetPosition(V(x, y))
        n = b.FindNet(net)
        for p in fp.Pads():
            p.SetNet(n)
        b.Add(fp)
        print('%s %s at (%.3f,%.3f) ref text at (%.2f,%.2f) %s' % (ref, net, x, y, M(fp.Reference().GetPosition().x), M(fp.Reference().GetPosition().y), b.GetLayerName(fp.Reference().GetLayer())))
        if stub:
            for a, c in zip(stub, stub[1:]):
                t = pcbnew.PCB_TRACK(b)
                t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(int(round(w * 1e6))); t.SetLayer(pcbnew.F_Cu); t.SetNet(n)
                b.Add(t)
                print('   stub (%.3f,%.3f)-(%.3f,%.3f) w%.2f' % (*a, *c, w))
    if apply:
        pcbnew.ZONE_FILLER(b).Fill(b.Zones())
        pcbnew.SaveBoard(b.GetFileName(), b)
        print('SAVED')
    else:
        print('DRY RUN')


if __name__ == '__main__':
    main(sys.argv[1], '--apply' in sys.argv)
