"""Board side of the 2026-10-10 pre-fab fixes (docs/audits/2026-10-10-prefab-*.md).

  <kicad python3> -I hardware/layout/2026-10-10-prefab-board-fix.py hardware/cubevox.kicad_pcb [--apply]

1. K103 pad contacts on the NC pins: IN_P to pad 2, LINE_N to pad 7, pads 4/5 open. IN_N stays
   on COM pad 6. The relay pads are 1.0 mm wide and 2.1 mm tall.
2. C127 feeds U105.8 on F.Cu; the B.Cu jog between the two 5VA vias goes.
3. The GND via inside D2.2 goes (the pad keeps its via 0.94 mm south).
4. R162 100k 0402 on MUX_S0 as a pass-through at (143.75, 114.0), GND via east of it.
5. RESET and BOOT silk text at 0.15 mm stroke.
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location('fix1', __file__.replace('prefab-board-fix', 'prefab-fix'))
fix1 = importlib.util.module_from_spec(spec); spec.loader.exec_module(fix1)
import pcbnew

F, B, V, M = pcbnew.F_Cu, pcbnew.B_Cu, fix1.V, pcbnew.ToMM


class Fix2(fix1.Fix):
    def net(self, name):
        n = self.b.FindNet(name)
        if n is None:
            n = pcbnew.NETINFO_ITEM(self.b, name)
            self.b.Add(n)
            self.say('  net %s created' % name)
        return n

    def add_via(self, net, pt, d=0.6, drill=0.3):
        v = pcbnew.PCB_VIA(self.b)
        v.SetPosition(V(*pt)); v.SetWidth(int(d * 1e6)); v.SetDrill(int(drill * 1e6))
        v.SetLayerPair(F, B); v.SetNet(self.net(net))
        self.b.Add(v)
        self.say('  via+ %-22s (%.3f,%.3f) d%.2f' % (net, pt[0], pt[1], d))

    def rm_via(self, net, pt):
        o = V(*pt)
        hits = [v for v in self.vias() if v.GetNetname() == net and self.near(v.GetPosition(), o)]
        if len(hits) != 1:
            raise SystemExit('via %s %s: %d matches' % (net, pt, len(hits)))
        self.pending_remove.append(hits[0])
        self.say('  via- %-22s (%.3f,%.3f)' % (net, *pt))

    def pad_net(self, ref, num, net):
        fp = self.b.FindFootprintByReference(ref)
        p = next(p for p in fp.Pads() if p.GetNumber() == num)
        self.say('  pad %s.%s %s -> %s' % (ref, num, p.GetNetname(), net))
        p.SetNet(self.net(net))

    def clone(self, src_ref, ref, value, centre, pad_at):
        """Duplicate src_ref as ref at centre, rotated so pad 1 lands at pad_at. Empty path: parity by reference."""
        src = self.b.FindFootprintByReference(src_ref)
        fp = pcbnew.Cast_to_FOOTPRINT(src.Duplicate(False))
        fp.SetReference(ref); fp.SetValue(value); fp.SetPath(pcbnew.KIID_PATH())
        fp.SetPosition(V(*centre))
        for rot in (0, 90, 180, 270):
            fp.SetOrientationDegrees(rot)
            p1 = next(p for p in fp.Pads() if p.GetNumber() == '1').GetPosition()
            if self.near(p1, V(*pad_at)):
                break
        else:
            raise SystemExit('clone %s: no rotation puts pad 1 at %s' % (ref, pad_at))
        self.b.Add(fp)
        self.say('  fp+ %s (%s) at (%.3f,%.3f) rot %d, pad 1 at (%.3f,%.3f)' % (ref, value, *centre, rot, *pad_at))
        return fp

    def text_thickness(self, text, layer, w):
        hits = [t for t in self.b.GetDrawings() if t.GetClass() == 'PCB_TEXT' and t.GetLayer() == layer and t.GetText() == text]
        if len(hits) != 1:
            raise SystemExit('text %s: %d matches' % (text, len(hits)))
        hits[0].SetTextThickness(int(w * 1e6))
        self.say('  text %-8s stroke -> %.2f' % (text, w))


def main(path, apply):
    b = pcbnew.LoadBoard(path)
    x = Fix2(b)

    x.say('1a. K103 pole A: IN_P to pad 2')
    x.rm('/IN_P', F, (195.7, 92.0), (195.1, 91.4))
    x.rm('/IN_P', F, (195.1, 91.4), (195.1, 86.51))
    x.end('/IN_P', F, (204.9, 92.0), (195.7, 92.0), (195.7, 92.0), (191.3, 92.0))
    x.add('/IN_P', F, 0.2, [(191.3, 92.0), (190.7, 91.4), (190.7, 86.51)])
    x.pad_net('K103', '2', '/IN_P')
    x.pad_net('K103', '4', 'unconnected-(K103-Pad4)')

    x.say('1b. K103 pole B: LINE_N to pad 7 (IN_N stays on COM pad 6)')
    # chain: C125.1 - hop vias - R105.1 - via - B.Cu lane between the pad rows - via - pad 7
    x.rm('/LINE_N', F, (195.1, 79.49), (195.1, 74.6))
    x.rm('/LINE_N', F, (197.75, 79.49), (195.1, 79.49))
    x.add('/LINE_N', F, 0.2, [(195.1, 74.6), (195.1, 75.1), (196.4, 76.4), (196.4, 78.99), (196.9, 79.49), (197.75, 79.49)])
    x.add('/LINE_N', F, 0.2, [(197.75, 79.49), (197.75, 80.6)])
    x.add_via('/LINE_N', (197.75, 80.6))
    x.add('/LINE_N', B, 0.2, [(197.75, 80.6), (191.2, 80.6), (190.7, 81.1), (190.7, 81.6)])
    x.add_via('/LINE_N', (190.7, 81.6))
    x.add('/LINE_N', F, 0.2, [(190.7, 81.6), (190.7, 79.49)])
    x.pad_net('K103', '7', '/LINE_N')
    x.pad_net('K103', '5', 'unconnected-(K103-Pad5)')

    x.say('2. C127 to U105.8 on F.Cu')
    x.rm('/5VA', B, (229.1, 80.6), (227.3, 80.6))
    x.rm('/5VA', B, (227.3, 80.6), (226.0, 79.3))
    x.rm('/5VA', B, (226.0, 79.3), (226.0, 78.5))
    x.add('/5VA', F, 0.3, [(226.8, 78.5), (226.8, 80.4), (228.3, 81.9), (229.1, 81.9)])

    x.say('3. D2.2 in-pad via')
    x.rm_via('/GND', (114.956, 40.285))

    x.say('4. R162 100k 0402 on MUX_S0 (pass-through at pad 1; the four MUX lines sit at 0.65 mm pitch)')
    fp = x.clone('R154', 'R162', '100k', (144.26, 114.0), (143.75, 114.0))
    p2 = next(p for p in fp.Pads() if p.GetNumber() == '2').GetPosition()
    x.say('  pad 2 at (%.3f,%.3f)' % (M(p2.x), M(p2.y)))
    x.pad_net('R162', '1', '/MUX_S0')
    x.pad_net('R162', '2', '/GND')
    x.end('/MUX_S0', F, (143.75, 95.6), (143.75, 119.35), (143.75, 119.35), (143.75, 114.0))
    x.add('/MUX_S0', F, 0.2, [(143.75, 114.0), (143.75, 119.35)])
    x.add_via('/GND', (145.8, 114.0))
    x.add('/GND', F, 0.3, [(M(p2.x), 114.0), (145.8, 114.0)])

    x.say('5. RESET / BOOT silk stroke')
    x.text_thickness('RESET', pcbnew.F_SilkS, 0.15)
    x.text_thickness('BOOT', pcbnew.F_SilkS, 0.15)

    x.commit(apply)


if __name__ == '__main__' and '--pass2' not in sys.argv:
    main(sys.argv[1], '--apply' in sys.argv)


def pass2(path, apply):
    """LINE_N hop via to R105.1 as one straight and one 45 (the Z shape read as a jog)."""
    b = pcbnew.LoadBoard(path)
    x = Fix2(b)
    x.say('6. LINE_N hop via to R105.1: straight south, one 45, west entry')
    x.rm('/LINE_N', F, (195.1, 74.6), (195.1, 75.1))
    x.rm('/LINE_N', F, (195.1, 75.1), (196.4, 76.4))
    x.rm('/LINE_N', F, (196.4, 76.4), (196.4, 78.99))
    x.rm('/LINE_N', F, (196.4, 78.99), (196.9, 79.49))
    x.rm('/LINE_N', F, (196.9, 79.49), (197.75, 79.49))
    x.add('/LINE_N', F, 0.2, [(195.1, 74.6), (195.1, 77.49), (197.1, 79.49), (197.75, 79.49)])
    x.commit(apply)


if __name__ == '__main__' and '--pass2' in sys.argv:
    pass2(sys.argv[1], '--apply' in sys.argv)
