import math
import unittest

from decal_core import clip, raster

from . import helpers as H

P = raster.PITCH_MM


class Fill(unittest.TestCase):
    def test_triangle_area(self):
        m = clip.fill(120, 120, [(1.0, 1.0), (11.0, 1.0), (1.0, 9.0)])
        self.assertAlmostEqual(m.count() * P * P, 40.0, delta=0.4)

    def test_pixel_is_inside_when_its_centre_is(self):
        # a loop from 0.5 mm to 1.5 mm covers pixel centres 0.55 to 1.45, i.e. 10 columns
        m = clip.fill(30, 30, H.rect_loop(0.5, 0.5, 1.5, 1.5))
        self.assertEqual(m.count(), 100)

    def test_loop_outside_the_frame_is_clipped(self):
        m = clip.fill(20, 20, H.rect_loop(-5, -5, 1.0, 1.0))
        self.assertEqual(m.count(), 100)


class Allowed(unittest.TestCase):
    def setUp(self):
        self.m = clip.allowed(120, 120, H.rect_loop(1, 1, 11, 11), [H.rect_loop(5, 5, 7, 7)], 0.5)

    def at(self, u, v):
        return self.m.bits[int(v / P) * self.m.w + int(u / P)]

    def test_margin_from_the_outer_edge(self):
        self.assertEqual(self.at(1.3, 6.0), 0)
        self.assertEqual(self.at(1.45, 6.0), 0)
        self.assertEqual(self.at(1.7, 6.0), 1)
        self.assertEqual(self.at(10.7, 6.0), 0)
        self.assertEqual(self.at(10.3, 6.0), 1)

    def test_margin_from_the_inner_cutout(self):
        self.assertEqual(self.at(6.0, 6.0), 0)
        self.assertEqual(self.at(4.7, 6.0), 0)
        self.assertEqual(self.at(4.3, 6.0), 1)

    def test_area(self):
        outer = 9.0 * 9.0
        inner = 2 * 2 + 4 * 2 * 0.5 + math.pi * 0.25
        self.assertAlmostEqual(self.m.count() * P * P, outer - inner, delta=0.8)

    def test_zero_margin_keeps_the_cutout_edge(self):
        m = clip.allowed(120, 120, H.rect_loop(1, 1, 11, 11), [H.rect_loop(5, 5, 7, 7)], 0.0)
        self.assertAlmostEqual(m.count() * P * P, 100 - 4, delta=0.2)

    def test_face_edge_just_outside_the_frame_still_trims(self):
        # the face ends 0.2 mm past the frame's left edge, so the first 0.3 mm of the frame goes
        m = clip.allowed(50, 50, H.rect_loop(-0.2, -5, 9, 9), [], 0.5)
        self.assertEqual(m.bits[25 * 50 + 2], 0)
        self.assertEqual(m.bits[25 * 50 + 4], 1)


class ClearCircles(unittest.TestCase):
    def test_removes_a_sixteen_mm_disc(self):
        full = raster.Mask(200, 200, bytearray(b"\x01" * 40000))
        out = clip.clear_circles(full, [(10.0, 10.0, 16.0)])
        removed = full.count() - out.count()
        self.assertAlmostEqual(removed * P * P, math.pi * 64.0, delta=0.5)
        self.assertEqual(out.bits[100 * 200 + 100], 0)
        self.assertEqual(out.bits[100 * 200 + 15], 1)
        self.assertEqual(out.bits[100 * 200 + 185], 1)

    def test_input_is_not_changed(self):
        full = raster.Mask(50, 50, bytearray(b"\x01" * 2500))
        clip.clear_circles(full, [(2.5, 2.5, 3.0)])
        self.assertEqual(full.count(), 2500)

    def test_circle_partly_outside(self):
        full = raster.Mask(50, 50, bytearray(b"\x01" * 2500))
        out = clip.clear_circles(full, [(0.0, 0.0, 4.0)])
        self.assertAlmostEqual((2500 - out.count()) * P * P, math.pi * 4 / 4.0, delta=0.15)

    def test_several_circles(self):
        full = raster.Mask(100, 50, bytearray(b"\x01" * 5000))
        out = clip.clear_circles(full, [(2.0, 2.5, 2.0), (8.0, 2.5, 2.0)])
        self.assertAlmostEqual((5000 - out.count()) * P * P, 2 * math.pi, delta=0.15)


if __name__ == "__main__":
    unittest.main()
