"""Pre-fab audit fixes (docs/audits/2026-10-09-prefab-mega-audit.md, owner go 2026-10-10).

  <kicad python3> -I hardware/layout/2026-10-10-prefab-fix.py hardware/cubevox.kicad_pcb [--apply]

Without --apply it only reports what it would touch. Every removal must match exactly one
track; every moved part lists the tracks that end on its pads and how they were shifted.
"""
import sys

import pcbnew

M = pcbnew.ToMM
F, B = pcbnew.F_Cu, pcbnew.B_Cu
TOL = 10000  # nm


def V(x, y):
    return pcbnew.VECTOR2I(int(round(x * 1e6)), int(round(y * 1e6)))


class Fix:
    def __init__(self, board):
        self.b = board
        self.log = []
        self.pending_remove = []

    def say(self, s):
        self.log.append(s)
        print(s)

    def tracks(self):
        return [t for t in self.b.GetTracks() if t.GetClass() == 'PCB_TRACK']

    def vias(self):
        return [t for t in self.b.GetTracks() if t.GetClass() == 'PCB_VIA']

    def near(self, a, pt):
        return abs(a.x - pt.x) <= TOL and abs(a.y - pt.y) <= TOL

    def seg(self, net, layer, p1, p2):
        a, c = V(*p1), V(*p2)
        hits = [t for t in self.tracks() if t.GetNetname() == net and t.GetLayer() == layer
                and (self.near(t.GetStart(), a) and self.near(t.GetEnd(), c) or self.near(t.GetStart(), c) and self.near(t.GetEnd(), a))
                and t not in self.pending_remove]
        if len(hits) != 1:
            raise SystemExit('seg %s %s %s-%s: %d matches' % (net, 'F' if layer == F else 'B', p1, p2, len(hits)))
        return hits[0]

    def rm(self, net, layer, p1, p2):
        t = self.seg(net, layer, p1, p2)
        self.pending_remove.append(t)
        self.say('  rm  %-22s %s (%.3f,%.3f)-(%.3f,%.3f)' % (net, 'F' if layer == F else 'B', *p1, *p2))

    def add(self, net, layer, w, pts):
        n = self.b.FindNet(net)
        if n is None:
            raise SystemExit('net %s missing' % net)
        for a, c in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(self.b)
            t.SetStart(V(*a)); t.SetEnd(V(*c)); t.SetWidth(int(round(w * 1e6))); t.SetLayer(layer); t.SetNet(n)
            self.b.Add(t)
            self.say('  add %-22s %s (%.3f,%.3f)-(%.3f,%.3f) w%.2f' % (net, 'F' if layer == F else 'B', *a, *c, w))

    def end(self, net, layer, p1, p2, old, new):
        """Move one endpoint of the track p1-p2 from old to new."""
        t = self.seg(net, layer, p1, p2)
        o = V(*old)
        if self.near(t.GetStart(), o):
            t.SetStart(V(*new))
        elif self.near(t.GetEnd(), o):
            t.SetEnd(V(*new))
        else:
            raise SystemExit('end %s: %s not an endpoint' % (net, old))
        self.say('  end %-22s %s (%.3f,%.3f) -> (%.3f,%.3f)' % (net, 'F' if layer == F else 'B', *old, *new))

    def width(self, net, layer, p1, p2, w):
        t = self.seg(net, layer, p1, p2)
        t.SetWidth(int(round(w * 1e6)))
        self.say('  w   %-22s %s (%.3f,%.3f)-(%.3f,%.3f) -> %.2f' % (net, 'F' if layer == F else 'B', *p1, *p2, w))

    def via(self, net, old, new):
        o = V(*old)
        hits = [v for v in self.vias() if v.GetNetname() == net and self.near(v.GetPosition(), o)]
        if len(hits) != 1:
            raise SystemExit('via %s %s: %d matches' % (net, old, len(hits)))
        hits[0].SetPosition(V(*new))
        self.say('  via %-22s (%.3f,%.3f) -> (%.3f,%.3f)' % (net, *old, *new))

    def move(self, ref, dx, dy):
        """Move a footprint; shift track ends that sit on its pad centres when the track runs along the move axis."""
        fp = self.b.FindFootprintByReference(ref)
        if fp is None:
            raise SystemExit('no %s' % ref)
        olds = [(p.GetNumber(), p.GetNetname(), pcbnew.VECTOR2I(p.GetPosition())) for p in fp.Pads()]
        fp.SetPosition(V(M(fp.GetPosition().x) + dx, M(fp.GetPosition().y) + dy))
        self.say('  mv  %s by (%.3f,%.3f) -> (%.3f,%.3f)' % (ref, dx, dy, M(fp.GetPosition().x), M(fp.GetPosition().y)))
        d = V(dx, dy)
        for num, net, o in olds:
            for t in self.tracks():
                if t.GetNetname() != net or t in self.pending_remove:
                    continue
                for getter, setter in ((t.GetStart, t.SetStart), (t.GetEnd, t.SetEnd)):
                    pt = getter()
                    if not self.near(pt, o):
                        continue
                    other = t.GetEnd() if getter is t.GetStart else t.GetStart()
                    horiz, vert = other.y == pt.y, other.x == pt.x
                    along = (dy == 0 and horiz) or (dx == 0 and vert) or (dx == 0 and dy == 0)
                    if along or (dx != 0 and dy != 0 and False):
                        setter(pcbnew.VECTOR2I(pt.x + d.x, pt.y + d.y))
                        self.say('      pad %s.%s %s track end shifted' % (ref, num, net))
                    else:
                        self.say('      WARN pad %s.%s %s track (%.3f,%.3f)-(%.3f,%.3f) not along the move' % (ref, num, net, M(t.GetStart().x), M(t.GetStart().y), M(t.GetEnd().x), M(t.GetEnd().y)))

    def commit(self, apply):
        for t in self.pending_remove:
            self.b.Remove(t)
        self.say('removed %d tracks' % len(self.pending_remove))
        if apply:
            filler = pcbnew.ZONE_FILLER(self.b)
            filler.Fill(self.b.Zones())
            pcbnew.SaveBoard(self.b.GetFileName(), self.b)
            self.say('SAVED ' + self.b.GetFileName())
        else:
            self.say('DRY RUN, nothing saved')


def main(path, apply):
    b = pcbnew.LoadBoard(path)
    x = Fix(b)
    N3 = '/3V3'

    x.say('F1 3V3 loop + acute T at (159.97,58.90)')
    x.rm(N3, F, (159.97, 58.9), (160.72, 59.65))
    x.rm(N3, F, (160.72, 59.65), (169.942, 59.65))
    x.rm(N3, F, (159.97, 49.43), (159.97, 58.9))
    x.rm(N3, F, (159.97, 58.9), (159.97, 78.87))
    x.add(N3, F, 0.3, [(159.97, 49.43), (159.97, 78.87)])

    x.say('F2 LINE_P daisy chain through C121.1')
    x.rm('/LINE_P', F, (213.4, 70.8), (214.1, 71.5))
    x.rm('/LINE_P', F, (214.1, 71.5), (214.1, 73.6))
    x.end('/LINE_P', F, (194.6, 70.8), (213.4, 70.8), (213.4, 70.8), (218.1, 70.8))
    x.add('/LINE_P', F, 0.2, [(218.1, 70.8), (218.5, 71.2), (218.5, 72.16)])

    x.say('F3 G2_INV one line, C132 0.07 west')
    for p1, p2 in (((232.5, 68.5), (231.7, 69.3)), ((231.7, 69.3), (231.7, 78.43)), ((231.7, 78.43), (231.77, 78.5)),
                   ((231.77, 78.5), (231.77, 79.3)), ((231.77, 79.3), (231.63, 79.44)), ((231.63, 79.44), (231.63, 81.9))):
        x.rm('/G2_INV', F, p1, p2)
    x.rm('/PRE_OUT', F, (230.37, 70.37), (230.37, 78.5))
    x.rm('/PRE_OUT', F, (230.37, 78.5), (230.37, 81.9))
    x.move('C132', -0.07, 0)
    x.add('/PRE_OUT', F, 0.2, [(230.37, 70.37), (230.37, 81.9)])
    x.add('/G2_INV', F, 0.2, [(232.5, 68.5), (231.63, 69.37), (231.63, 81.9)])

    x.say('F4 3V3 at C57, C13 0.30 north, VCAP2 centred')
    for p1, p2 in (((90.0, 76.45), (90.0, 75.56)), ((90.0, 75.56), (89.6, 75.16)), ((89.6, 75.16), (88.51, 75.16)),
                   ((88.51, 75.16), (88.25, 74.9)), ((88.25, 74.9), (88.25, 74.48))):
        x.rm(N3, F, p1, p2)
    for p1, p2 in (((90.95, 74.45), (90.95, 75.7)), ((90.775, 74.275), (90.95, 74.45)), ((90.0, 74.275), (90.775, 74.275))):
        x.rm('/h7core_block/VCAP2', F, p1, p2)
    x.move('C13', 0, -0.3)
    x.via('/GND', (90.0, 71.875), (90.0, 71.5))
    x.end('/GND', F, (90.0, 72.425), (90.0, 71.875), (90.0, 71.875), (90.0, 71.5))
    x.add(N3, F, 0.3, [(90.0, 76.45), (90.0, 75.45), (89.03, 74.48), (88.25, 74.48)])
    x.add('/h7core_block/VCAP2', F, 0.2, [(91.0, 76.45), (91.0, 74.225), (90.75, 73.975), (90.0, 73.975)])

    x.say('F5 VREF_BUF one diagonal across U108')
    for p1, p2 in (((159.61, 88.55), (160.55, 88.55)), ((160.55, 88.55), (160.75, 88.75)), ((160.75, 88.75), (160.75, 90.25)),
                   ((160.75, 90.25), (160.95, 90.45)), ((160.95, 90.45), (161.89, 90.45))):
        x.rm('/VREF_BUF', F, p1, p2)
    x.add('/VREF_BUF', F, 0.2, [(159.613, 88.55), (159.987, 88.55), (161.887, 90.45)])

    x.say('F6 VBUS at J1 one diagonal')
    VB = '/h7core_block/VBUS'
    for p1, p2 in (((117.4, 36.165), (117.4, 37.664)), ((117.4, 37.664), (118.56, 38.824)), ((118.56, 38.824), (118.56, 38.86)),
                   ((118.56, 38.86), (119.375, 39.675)), ((119.375, 39.675), (120.11, 39.675))):
        x.rm(VB, F, p1, p2)
    x.add(VB, F, 0.3, [(117.4, 36.165), (117.4, 37.6), (119.475, 39.675), (120.11, 39.675)])

    x.say('F7 VBUS at C18: C18 0.20 north, via exits straight')
    x.rm(VB, F, (111.11, 40.0), (111.11, 40.3))
    x.rm(VB, F, (111.11, 40.3), (110.91, 40.5))
    x.rm(VB, F, (111.11, 40.0), (111.91, 40.0))
    x.rm(VB, B, (112.11, 40.0), (111.11, 40.0))
    x.rm(VB, B, (112.61, 39.5), (112.11, 40.0))
    x.rm('/GND', F, (111.91, 41.55), (112.785, 41.55))
    x.move('C18', 0, -0.2)
    x.via('/GND', (112.785, 41.55), (112.785, 41.35))
    x.add('/GND', F, 0.3, [(111.91, 41.35), (112.785, 41.35)])
    x.via(VB, (111.11, 40.0), (111.11, 39.8))
    x.end(VB, F, (110.91, 40.5), (109.2, 40.5), (110.91, 40.5), (110.81, 40.5))
    x.add(VB, F, 0.3, [(111.11, 39.8), (111.91, 39.8)])
    x.add(VB, B, 0.3, [(112.61, 39.5), (112.31, 39.8), (111.11, 39.8)])
    x.add(VB, F, 0.2, [(111.11, 39.8), (111.11, 40.2), (110.81, 40.5)])

    x.say('F8 TOGGLE1 F.Cu chamfers 0.3 legs')
    x.rm('/TOGGLE1', F, (64.5, 116.0), (64.7, 116.2))
    x.rm('/TOGGLE1', F, (64.7, 116.2), (66.3, 116.2))
    x.rm('/TOGGLE1', F, (66.3, 116.2), (66.6, 116.5))
    x.end('/TOGGLE1', F, (66.6, 116.5), (66.6, 122.05), (66.6, 116.5), (66.6, 116.6))
    x.add('/TOGGLE1', F, 0.2, [(64.5, 116.0), (64.8, 116.3), (66.3, 116.3), (66.6, 116.6)])

    x.say('F9 TOGGLE1/TOGGLE2 vias: straight exit 0.4, chamfer 0.3')
    x.via('/TOGGLE2', (112.2, 107.48), (112.4, 107.48))
    x.rm('/TOGGLE2', B, (112.2, 110.0), (112.2, 107.48))
    x.end('/TOGGLE2', B, (108.5, 113.7), (112.2, 110.0), (112.2, 110.0), (112.4, 109.8))
    x.add('/TOGGLE2', B, 0.2, [(112.4, 109.8), (112.4, 107.48)])
    x.rm('/TOGGLE2', F, (112.2, 107.48), (111.88, 107.48))
    x.rm('/TOGGLE2', F, (111.88, 107.48), (111.7, 107.3))
    x.end('/TOGGLE2', F, (111.7, 107.3), (111.7, 87.5), (111.7, 107.3), (111.7, 107.18))
    x.add('/TOGGLE2', F, 0.2, [(112.4, 107.48), (112.0, 107.48), (111.7, 107.18)])
    x.via('/TOGGLE1', (110.8, 107.48), (110.6, 107.48))
    x.end('/TOGGLE1', B, (110.8, 107.48), (95.52, 107.48), (110.8, 107.48), (110.6, 107.48))
    x.rm('/TOGGLE1', F, (110.8, 107.48), (111.12, 107.48))
    x.rm('/TOGGLE1', F, (111.12, 107.48), (111.3, 107.3))
    x.end('/TOGGLE1', F, (111.3, 107.3), (111.3, 88.0), (111.3, 107.3), (111.3, 107.18))
    x.add('/TOGGLE1', F, 0.2, [(110.6, 107.48), (111.0, 107.48), (111.3, 107.18)])

    x.say('F10 ADC_SCKI via onto the U8.6 lane')
    SK = '/h7core_block/ADC_SCKI'
    x.rm(SK, F, (128.25, 100.5), (128.25, 100.85))
    x.rm(SK, F, (128.25, 100.85), (127.9, 101.2))
    x.via(SK, (128.25, 100.5), (128.25, 101.2))
    x.end(SK, F, (127.9, 101.2), (126.8, 101.2), (127.9, 101.2), (128.25, 101.2))
    x.end(SK, B, (128.25, 101.9), (128.25, 100.5), (128.25, 100.5), (128.25, 101.2))

    x.say('F11 GND vias at U8 0.35 north, straight 0.40 into each')
    x.rm('/GND', F, (124.4, 101.575), (124.4, 101.3))
    x.rm('/GND', F, (124.675, 101.85), (124.4, 101.575))
    x.via('/GND', (124.4, 101.3), (124.4, 100.95))
    x.end('/GND', F, (126.8, 101.85), (124.675, 101.85), (124.675, 101.85), (124.9, 101.85))
    x.add('/GND', F, 0.3, [(124.9, 101.85), (124.4, 101.35), (124.4, 100.95)])
    x.rm('/GND', F, (123.0, 101.575), (123.0, 101.3))
    x.rm('/GND', F, (122.725, 101.85), (123.0, 101.575))
    x.via('/GND', (123.0, 101.3), (123.0, 100.95))
    x.end('/GND', F, (121.2, 101.85), (122.725, 101.85), (122.725, 101.85), (122.5, 101.85))
    x.add('/GND', F, 0.3, [(122.5, 101.85), (123.0, 101.35), (123.0, 100.95)])

    x.say('F12 GND via (122.90,103.80) 0.20 south, straight 0.40')
    x.rm('/GND', F, (122.575, 103.15), (122.9, 103.475))
    x.rm('/GND', F, (122.9, 103.475), (122.9, 103.8))
    x.via('/GND', (122.9, 103.8), (122.9, 104.0))
    x.end('/GND', F, (121.2, 103.15), (122.575, 103.15), (122.575, 103.15), (122.45, 103.15))
    x.add('/GND', F, 0.3, [(122.45, 103.15), (122.9, 103.6), (122.9, 104.0)])

    x.say('F13 MUTE_COIL leaves Q101.3 from its centre')
    x.rm('/MUTE_COIL', F, (70.5, 77.5), (70.0, 77.5))
    x.end('/MUTE_COIL', F, (70.0, 77.5), (70.0, 69.27), (70.0, 77.5), (70.0, 76.5))
    x.add('/MUTE_COIL', F, 0.3, [(70.5, 77.5), (70.5, 77.0), (70.0, 76.5)])

    x.say('F14 R105 to the K103 pad row y 79.49')
    x.end('/LINE_N', F, (195.1, 79.5), (195.1, 74.6), (195.1, 79.5), (195.1, 79.49))
    x.end('/LINE_N', F, (197.75, 79.5), (195.1, 79.5), (195.1, 79.5), (195.1, 79.49))
    x.move('R105', 0, -0.01)

    x.say('F16 buck cluster centre to centre')
    x.move('L101', -0.15, 0)
    x.move('C116', -0.2, 0)
    x.move('C113', 0, 0.4)
    x.via('/GND', (97.0, 51.5), (97.0, 51.9))
    x.end('/GND', F, (97.0, 52.7), (97.0, 51.5), (97.0, 51.5), (97.0, 51.9))
    x.move('C112', 0, -0.49)
    x.rm('/BUCK5_SW', F, (94.1, 51.6), (94.1, 47.8))
    x.add('/BUCK5_SW', F, 0.5, [(94.0, 51.9), (94.0, 46.0)])
    x.rm('/BUCK5_BST', F, (93.05, 51.6), (93.05, 50.25))
    x.add('/BUCK5_BST', F, 0.2, [(93.05, 51.9), (93.05, 51.05), (92.6, 50.6), (92.6, 50.0)])
    x.rm('/VIN_9V', F, (94.3, 54.1), (94.65, 54.1))
    x.rm('/VIN_9V', F, (95.25, 53.7), (96.6, 53.7))
    x.rm('/VIN_9V', F, (97.4, 54.05), (98.7, 54.05))
    x.add('/VIN_9V', F, 0.4, [(94.0, 54.1), (99.5, 54.1)])

    x.say('F17 D106 0.25 south, VIN_9V centre to centre')
    x.move('D106', 0, 0.25)
    x.rm('/VIN_9V', F, (233.5, 51.45), (230.65, 51.45))
    x.add('/VIN_9V', F, 0.4, [(229.59, 51.45), (234.35, 51.45)])

    x.say('F18 TP2 onto the VIN_9V line')
    x.move('TP2', 0, -0.05)
    x.end('/VIN_9V', F, (229.59, 51.45), (221.55, 51.45), (221.55, 51.45), (221.3, 51.45))

    x.say('F19 TP3 trace into J106.1 centre')
    for p1, p2 in (((222.6, 42.7), (222.6, 40.05)), ((222.6, 40.05), (222.65, 40.0)), ((221.3, 44.0), (222.6, 42.7)), ((219.0, 44.0), (221.3, 44.0))):
        x.rm('/GND', F, p1, p2)
    x.add('/GND', F, 0.3, [(219.0, 44.0), (221.35, 44.0), (222.65, 42.7), (222.65, 40.0)])

    x.say('F20 edge bends')
    x.rm(N3, F, (84.365, 79.73), (83.83, 79.73)); x.rm(N3, F, (83.83, 79.73), (83.6, 79.5))
    x.end(N3, F, (83.6, 79.5), (83.6, 74.7), (83.6, 79.5), (83.6, 79.565))
    x.add(N3, F, 0.3, [(84.365, 79.73), (83.765, 79.73), (83.6, 79.565)])
    x.rm(N3, F, (93.35, 93.725), (92.8, 93.725)); x.rm(N3, F, (92.8, 93.725), (92.5, 93.425))
    x.end(N3, F, (92.5, 91.55), (92.5, 93.425), (92.5, 93.425), (92.5, 93.5))
    x.add(N3, F, 0.3, [(93.35, 93.725), (92.725, 93.725), (92.5, 93.5)])
    x.rm(N3, F, (88.5, 124.15), (88.0, 124.15)); x.rm(N3, F, (88.0, 124.15), (86.55, 125.6))
    x.rm(N3, F, (85.0, 125.6), (86.55, 125.6)); x.rm(N3, F, (82.6, 125.6), (85.0, 125.6))
    x.end(N3, F, (67.2, 125.6), (82.6, 125.6), (82.6, 125.6), (86.42, 125.6))
    x.add(N3, F, 0.3, [(88.5, 124.15), (87.87, 124.15), (86.42, 125.6)])
    x.rm(N3, F, (101.0, 98.8), (101.5, 98.8)); x.rm(N3, F, (101.5, 98.8), (102.7, 100.0))
    x.end(N3, F, (102.7, 100.0), (109.1, 100.0), (102.7, 100.0), (102.875, 100.0))
    x.add(N3, F, 0.3, [(101.0, 98.8), (101.675, 98.8), (102.875, 100.0)])
    x.move('C8', 0.1, 0)
    x.via('/GND', (103.925, 98.775), (104.025, 98.775))
    HO = '/h7core_block/HSE_OUT'
    x.rm(HO, F, (96.0, 91.55), (96.0, 92.4)); x.rm(HO, F, (96.0, 92.4), (97.775, 94.175))
    x.end(HO, F, (97.775, 94.175), (99.0, 94.175), (97.775, 94.175), (97.625, 94.175))
    x.add(HO, F, 0.2, [(96.0, 91.55), (96.0, 92.55), (97.625, 94.175)])
    HI = '/h7core_block/HSE_IN'
    x.rm(HI, F, (95.5, 91.55), (95.5, 92.6)); x.rm(HI, F, (95.5, 92.6), (95.9, 93.0))
    x.end(HI, F, (95.9, 93.0), (95.9, 96.15), (95.9, 93.0), (95.9, 93.15))
    x.add(HI, F, 0.2, [(95.5, 91.55), (95.5, 92.75), (95.9, 93.15)])
    x.rm('/GND', F, (130.775, 101.5), (131.625, 101.5))
    x.move('C47', 0, -0.2)
    x.via('/GND', (131.625, 101.5), (131.625, 101.3))
    x.add('/GND', F, 0.3, [(130.775, 101.3), (131.625, 101.3)])
    x.rm('/IN_P', F, (207.6, 78.87), (206.63, 78.87)); x.rm('/IN_P', F, (206.63, 78.87), (205.3, 80.2))
    x.end('/IN_P', F, (205.3, 80.2), (205.3, 91.6), (205.3, 80.2), (205.3, 80.12))
    x.add('/IN_P', F, 0.2, [(207.6, 78.87), (206.55, 78.87), (205.3, 80.12)])
    SS = '/h7core_block/ADC_SCKI_SRC'
    x.rm(SS, F, (90.0, 91.55), (89.75, 91.55)); x.rm(SS, F, (89.75, 91.55), (88.15, 93.15))
    x.end(SS, F, (88.15, 93.15), (88.15, 93.49), (88.15, 93.15), (88.15, 93.07))
    x.add(SS, F, 0.2, [(90.0, 91.55), (89.67, 91.55), (88.15, 93.07)])

    x.say('F21 merge collinear 3V3 at C165.1')
    x.rm(N3, F, (83.06, 103.37), (83.06, 102.665)); x.rm(N3, F, (83.06, 102.665), (83.06, 91.35))
    x.add(N3, F, 0.3, [(83.06, 103.37), (83.06, 91.35)])

    x.say('F22 VREF 0.20 -> 0.30 at R146')
    for p1, p2 in (((157.25, 93.0), (157.25, 94.4)), ((157.25, 94.4), (155.84, 95.81)), ((155.84, 95.81), (155.84, 97.0))):
        x.width('/VREF', F, p1, p2, 0.3)

    x.commit(apply)


if __name__ == '__main__':
    main(sys.argv[1], '--apply' in sys.argv)
