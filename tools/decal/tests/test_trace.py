import unittest

from decal_core import raster, trace

from . import helpers as H

P = raster.PITCH_MM


class Square(unittest.TestCase):
    def test_ten_mm_square_is_one_loop_of_100_mm2(self):
        m = H.mask_rect(120, 120, [(10, 10, 110, 110)])
        pieces, dropped = trace.loops(m, 0.08, 0.5)
        self.assertEqual((len(pieces), dropped), (1, 0))
        self.assertAlmostEqual(pieces[0].area_mm2, 100.0, delta=0.5)
        self.assertAlmostEqual(abs(H.shoelace(pieces[0].outer)), 100.0, delta=0.5)
        self.assertEqual(len(pieces[0].outer), 4)
        self.assertEqual(pieces[0].holes, [])

    def test_orientation_in_the_mask_frame(self):
        # v runs down, so an outer loop that is counter-clockwise on screen has a negative sum
        m = H.mask_rect(40, 40, [(5, 5, 35, 35)])
        outer = trace.loops(m, 0.0, 0.0)[0][0].outer
        self.assertLess(H.shoelace(outer), 0)

    def test_coordinates_are_pixel_corners_in_mm(self):
        m = H.mask_rect(40, 40, [(10, 20, 30, 30)])
        outer = trace.loops(m, 0.0, 0.0)[0][0].outer
        self.assertEqual(sorted(outer), sorted([(1.0, 2.0), (3.0, 2.0), (3.0, 3.0), (1.0, 3.0)]))


class Rings(unittest.TestCase):
    def ring(self):
        m = H.mask_rect(140, 140, [(10, 10, 110, 110)])
        for y in range(40, 80):
            m.bits[y * 140 + 40:y * 140 + 80] = bytes(40)
        return m

    def test_ring_gives_an_outer_and_a_hole(self):
        pieces, dropped = trace.loops(self.ring(), 0.08, 0.5)
        self.assertEqual((len(pieces), dropped), (1, 0))
        piece = pieces[0]
        self.assertEqual(len(piece.holes), 1)
        self.assertAlmostEqual(piece.area_mm2, 100.0 - 16.0, delta=0.4)
        self.assertLess(H.shoelace(piece.outer), 0)
        self.assertGreater(H.shoelace(piece.holes[0]), 0)
        self.assertAlmostEqual(abs(H.shoelace(piece.holes[0])), 16.0, delta=0.2)

    def test_island_inside_a_hole_is_its_own_piece(self):
        m = self.ring()
        for y in range(50, 70):
            m.bits[y * 140 + 50:y * 140 + 70] = b"\x01" * 20
        pieces, _ = trace.loops(m, 0.08, 0.5)
        self.assertEqual(len(pieces), 2)
        holes = sorted(len(p.holes) for p in pieces)
        self.assertEqual(holes, [0, 1])
        island = next(p for p in pieces if not p.holes)
        self.assertAlmostEqual(island.area_mm2, 4.0, delta=0.1)

    def test_each_hole_goes_to_its_own_piece(self):
        m = H.mask_rect(200, 80, [(5, 5, 85, 75), (110, 5, 190, 75)])
        for x0 in (20, 45, 125, 150):
            for y in range(30, 50):
                m.bits[y * 200 + x0:y * 200 + x0 + 12] = bytes(12)
        pieces, _ = trace.loops(m, 0.08, 0.5)
        self.assertEqual([len(p.holes) for p in pieces], [2, 2])
        for p in pieces:
            xs = [q[0] for q in p.outer]
            for hole in p.holes:
                for x, _y in hole:
                    self.assertTrue(min(xs) < x < max(xs))

    def test_hole_nested_two_deep(self):
        m = H.mask_rect(120, 120, [(5, 5, 115, 115)])
        for y in range(20, 100):
            m.bits[y * 120 + 20:y * 120 + 100] = bytes(80)
        for y in range(40, 80):
            m.bits[y * 120 + 40:y * 120 + 80] = b"\x01" * 40
        pieces, _ = trace.loops(m, 0.0, 0.0)
        self.assertEqual(sorted(len(p.holes) for p in pieces), [0, 1])


class Dropping(unittest.TestCase):
    def test_small_pieces_drop_and_count(self):
        m = H.mask_rect(80, 40, [(5, 5, 35, 35), (50, 10, 54, 14)])  # 9 mm2 and 0.16 mm2
        pieces, dropped = trace.loops(m, 0.08, 0.5)
        self.assertEqual(len(pieces), 1)
        self.assertEqual(dropped, 1)

    def test_small_hole_is_filled_and_counted(self):
        m = H.mask_rect(60, 60, [(5, 5, 55, 55)])
        for y in range(28, 31):
            m.bits[y * 60 + 28:y * 60 + 31] = bytes(3)
        pieces, dropped = trace.loops(m, 0.08, 0.5)
        self.assertEqual((len(pieces), len(pieces[0].holes), dropped), (1, 0, 1))
        self.assertAlmostEqual(pieces[0].area_mm2, 25.0, delta=0.01)

    def test_dropped_outer_takes_its_holes(self):
        m = H.mask_rect(40, 40, [(5, 5, 15, 15)])
        for y in range(8, 12):
            m.bits[y * 40 + 8:y * 40 + 12] = bytes(4)
        pieces, dropped = trace.loops(m, 0.0, 2.0)
        self.assertEqual((pieces, dropped), ([], 1))

    def test_empty_mask(self):
        self.assertEqual(trace.loops(raster.Mask(10, 10), 0.08, 0.5), ([], 0))


class Corners(unittest.TestCase):
    def test_diagonal_pixels_trace_as_separate_simple_loops(self):
        m = H.mask_from_text(["#..", ".#.", "..#"])
        pieces, _ = trace.loops(m, 0.0, 0.0)
        self.assertEqual(len(pieces), 3)
        for p in pieces:
            self.assertEqual(len(p.outer), 4)

    def test_l_shape_has_six_corners(self):
        m = H.mask_rect(30, 30, [(5, 5, 10, 25), (5, 20, 25, 25)])
        pieces, _ = trace.loops(m, 0.08, 0.0)
        self.assertEqual(len(pieces[0].outer), 6)


class Simplify(unittest.TestCase):
    def circle_mask(self, r_px=100):
        size = 2 * r_px + 20
        m = raster.Mask(size, size)
        c = size / 2.0
        for y in range(size):
            for x in range(size):
                if (x + 0.5 - c) ** 2 + (y + 0.5 - c) ** 2 <= r_px * r_px:
                    m.bits[y * size + x] = 1
        return m

    def test_simplifying_holds_the_tolerance(self):
        m = self.circle_mask()
        full = trace.loops(m, 0.0, 0.0)[0][0].outer
        thin = trace.loops(m, 0.08, 0.0)[0][0].outer
        self.assertLess(len(thin), len(full) / 2)
        worst = max(H.poly_distance(p, thin) for p in full)
        self.assertLessEqual(worst, 0.08 + 1e-9)

    def test_tolerance_is_used_not_exceeded_by_much(self):
        m = self.circle_mask()
        coarse = trace.loops(m, 0.3, 0.0)[0][0].outer
        fine = trace.loops(m, 0.08, 0.0)[0][0].outer
        self.assertLess(len(coarse), len(fine))
        full = trace.loops(m, 0.0, 0.0)[0][0].outer
        self.assertLessEqual(max(H.poly_distance(p, coarse) for p in full), 0.3 + 1e-9)

    def test_staircase_diagonal_collapses(self):
        m = raster.Mask(80, 80)
        for i in range(60):
            m.bits[(10 + i) * 80 + 10 + i:(10 + i) * 80 + 10 + i + 12] = b"\x01" * 12
        full = trace.loops(m, 0.0, 0.0)[0][0].outer
        thin = trace.loops(m, 0.08, 0.0)[0][0].outer
        self.assertGreater(len(full), 100)
        self.assertLess(len(thin), 12)

    def test_points_counts_every_loop(self):
        m = H.mask_rect(60, 60, [(5, 5, 55, 55)])
        for y in range(20, 40):
            m.bits[y * 60 + 20:y * 60 + 40] = bytes(20)
        pieces = trace.loops(m, 0.08, 0.0)[0]
        self.assertEqual(trace.points(pieces), 8)


if __name__ == "__main__":
    unittest.main()
