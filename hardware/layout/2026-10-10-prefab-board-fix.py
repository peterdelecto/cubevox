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


if __name__ == '__main__' and not any(a in sys.argv for a in ('--pass2', '--pass3', '--pass4', '--pass5')):
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


def pass3(path, apply):
    """Owner moved R162 east of U101.10. Make pad 1 the corner (S0 vertical in from the north, straight
    west into pin 10), drop the off-pad T, merge the collinear MUX_S0 split left at the old spot."""
    b = pcbnew.LoadBoard(path)
    x = Fix2(b)
    x.say('7. R162 pad 1 as the MUX_S0 corner')
    x.rm('/MUX_S0', F, (153.15, 119.85), (153.65, 120.35))
    x.rm('/MUX_S0', F, (153.65, 120.35), (153.65, 124.72))
    x.rm('/MUX_S0', F, (153.65, 124.72), (153.15, 125.22))
    x.rm('/MUX_S0', F, (153.15, 125.22), (151.56, 125.22))
    x.rm('/MUX_S0', F, (154.03, 125.1), (153.65, 124.72))
    x.rm('/MUX_S0', F, (154.49, 125.1), (154.03, 125.1))
    x.rm('/GND', F, (155.51, 125.1), (156.54, 125.1))
    x.move('R162', 0, 0.12)
    x.via('/GND', (156.54, 125.1), (156.54, 125.22))
    x.add('/GND', F, 0.3, [(155.51, 125.22), (156.54, 125.22)])
    x.end('/MUX_S0', F, (144.25, 119.85), (153.15, 119.85), (153.15, 119.85), (153.99, 119.85))
    x.add('/MUX_S0', F, 0.2, [(153.99, 119.85), (154.49, 120.35), (154.49, 125.22)])
    x.add('/MUX_S0', F, 0.2, [(154.49, 125.22), (151.56, 125.22)])
    x.say('8. merge the MUX_S0 split at the old R162 spot')
    x.rm('/MUX_S0', F, (143.75, 114.0), (143.75, 119.35))
    x.end('/MUX_S0', F, (143.75, 95.6), (143.75, 114.0), (143.75, 114.0), (143.75, 119.35))
    x.commit(apply)


if __name__ == '__main__' and '--pass3' in sys.argv:
    pass3(sys.argv[1], '--apply' in sys.argv)


LIB_H7 = 'hardware/cubevox_h7core.pretty'


def swap(x, ref, fp_name, pad1_at):
    """Replace ref's footprint with the JLC (EasyEDA) one from cubevox_h7core.pretty, same centre, rotated
    so pad 1 lands at pad1_at; fields, path, pad nets and the reference/value text placement carry over."""
    b = x.b
    old = b.FindFootprintByReference(ref)
    new = pcbnew.FootprintLoad(LIB_H7, fp_name)
    new.SetFPID(pcbnew.LIB_ID('cubevox_h7core', fp_name))
    new.SetReference(ref); new.SetValue(old.GetValue()); new.SetPath(old.GetPath())
    for f in old.GetFields():
        if f.GetName() not in ('Reference', 'Value', 'Datasheet', 'Description', 'Footprint'):
            new.SetField(f.GetName(), f.GetText())
    for f in new.GetFields():
        if f.GetName() not in ('Reference', 'Value'):
            f.SetVisible(False)
    new.SetPosition(old.GetPosition())
    for rot in (0, 90, 180, 270):
        new.SetOrientationDegrees(rot)
        p1 = next(p for p in new.Pads() if p.GetNumber() == '1').GetPosition()
        if x.near(p1, V(*pad1_at)):
            break
    else:
        raise SystemExit('swap %s: no rotation puts pad 1 at %s' % (ref, pad1_at))
    for to, tn in ((old.Reference(), new.Reference()), (old.Value(), new.Value())):
        tn.SetAttributes(to.GetAttributes()); tn.SetLayer(to.GetLayer()); tn.SetVisible(to.IsVisible())
        tn.SetPosition(to.GetPosition()); tn.SetTextAngle(to.GetTextAngle())
    nets = {p.GetNumber(): p.GetNetname() for p in old.Pads()}
    for p in new.Pads():
        p.SetNet(x.net(nets[p.GetNumber()]))
    b.Remove(old); b.Add(new)
    x.say('  swap %s -> cubevox_h7core:%s rot %d, pads %s' % (ref, fp_name, rot,
          ' '.join('%s(%.3f,%.3f)' % (p.GetNumber(), M(p.GetPosition().x), M(p.GetPosition().y)) for p in new.Pads())))


def pass4(path, apply):
    """U107, U108, C19 from KiCad stock footprints to the JLC ones (CPL rotation convention), with the
    track ends, TP10 and two vias following the 0.13 to 0.30 mm pad shifts."""
    b = pcbnew.LoadBoard(path)
    x = Fix2(b)
    x.say('9. U107 SP809 SOT-23')
    swap(x, 'U107', 'SOT-23-3_L2.9-W1.6-P1.90-LS2.8-BR', (158.7, 47.25))
    x.end('/GND', F, (158.7, 47.55), (158.7, 46.7), (158.7, 47.55), (158.7, 47.25))
    x.end('/GND', F, (158.7, 47.25), (158.7, 46.7), (158.7, 46.7), (158.7, 46.2))
    x.via('/GND', (158.7, 46.7), (158.7, 46.2))
    LDO = '/h7core_block/LDO_EN'
    x.rm(LDO, F, (156.3, 47.55), (156.8, 47.55))
    x.rm(LDO, F, (155.37, 46.62), (156.3, 47.55))
    x.add(LDO, F, 0.2, [(155.37, 46.62), (156.0, 47.25), (156.8, 47.25)])
    x.rm('/3V3', F, (157.75, 49.43), (159.97, 49.43))
    x.add('/3V3', F, 0.3, [(159.97, 49.43), (158.5, 49.43), (158.2, 49.73), (157.75, 49.73)])

    x.say('10. U108 OPA197 SOT-23-5')
    swap(x, 'U108', 'SOT-23-5_L3.0-W1.7-P0.95-LS2.8-BR', (159.45, 88.55))
    VB, VD = '/VREF_BUF', '/VREF_DIV'
    x.end(VB, F, (156.05, 88.55), (159.613, 88.55), (159.613, 88.55), (159.45, 88.55))
    x.rm(VB, F, (159.613, 85.7), (159.613, 88.55))
    x.move('TP10', 159.45 - 159.6125, 0)
    x.add(VB, F, 0.2, [(159.45, 85.7), (159.45, 88.55)])
    x.end(VB, F, (159.613, 88.55), (160.5, 88.55), (159.613, 88.55), (159.45, 88.55))
    x.end('/GND', F, (158.15, 89.5), (159.61, 89.5), (159.61, 89.5), (159.45, 89.5))
    x.rm(VD, F, (159.61, 90.45), (159.61, 92.25))
    x.rm(VD, F, (159.61, 92.25), (160.61, 93.25))
    x.add(VD, F, 0.2, [(159.45, 90.45), (159.45, 92.09), (160.61, 93.25)])
    x.end(VB, F, (161.1, 90.45), (161.887, 90.45), (161.887, 90.45), (162.05, 90.45))
    x.end('/5VA', F, (161.89, 88.55), (163.37, 88.55), (161.89, 88.55), (162.05, 88.55))
    x.rm('/5VA', F, (161.89, 84.1), (161.89, 88.55))
    x.rm('/5VA', F, (160.89, 83.1), (161.89, 84.1))
    x.end('/5VA', F, (158.0, 83.1), (160.89, 83.1), (160.89, 83.1), (161.05, 83.1))
    x.add('/5VA', F, 0.3, [(161.05, 83.1), (162.05, 84.1), (162.05, 88.55)])

    x.say('11. C19 tantalum 6032')
    swap(x, 'C19', 'CAP-SMD_L6.0-W3.2-R-RD', (106.4, 46.0))
    x.end('/5V', F, (103.5, 46.0), (106.53, 46.0), (106.53, 46.0), (106.4, 46.0))
    x.rm('/5V', F, (106.53, 46.0), (106.53, 56.8))
    x.add('/5V', F, 0.5, [(106.4, 46.0), (106.4, 56.8)])
    x.via('/5V', (106.53, 56.8), (106.4, 56.8))
    x.end('/5V', B, (98.1, 56.8), (106.53, 56.8), (106.53, 56.8), (106.4, 56.8))
    x.rm('/5V', B, (106.53, 56.8), (106.53, 59.8))
    x.rm('/5V', B, (106.53, 59.8), (107.53, 60.8))
    x.add('/5V', B, 0.5, [(106.4, 56.8), (106.4, 59.67), (107.53, 60.8)])
    x.end('/GND', F, (111.47, 46.0), (112.905, 46.0), (111.47, 46.0), (111.6, 46.0))
    x.commit(apply)


if __name__ == '__main__' and '--pass4' in sys.argv:
    pass4(sys.argv[1], '--apply' in sys.argv)


def pass5(path, apply):
    """U107.1 GND via 0.1 mm south: 0.25 mm to the 3V3 lane at y 45.6 (netclass clearance 0.2)."""
    b = pcbnew.LoadBoard(path)
    x = Fix2(b)
    x.end('/GND', F, (158.7, 47.25), (158.7, 46.2), (158.7, 46.2), (158.7, 46.3))
    x.via('/GND', (158.7, 46.2), (158.7, 46.3))
    x.commit(apply)


if __name__ == '__main__' and '--pass5' in sys.argv:
    pass5(sys.argv[1], '--apply' in sys.argv)
