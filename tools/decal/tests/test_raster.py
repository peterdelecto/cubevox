import math
import random
import unittest

from decal_core import DecalError, raster

from . import helpers as H

P = raster.PITCH_MM


class FromGrey(unittest.TestCase):
    def test_dark_threshold_and_size(self):
        w, h = 100, 50
        grey = bytearray()
        for _ in range(h):
            grey += bytes([0] * 50 + [255] * 50)
        m = raster.from_grey(w, h, grey, None, 4.0, "dark", 128, False)
        self.assertEqual((m.w, m.h), (40, 20))
        self.assertEqual(m.count(), 20 * 20)
        self.assertTrue(all(m.bits[y * 40 + x] == (x < 20) for y in range(20) for x in range(40)))

    def test_threshold_is_inclusive(self):
        grey = bytearray([100, 128, 129, 255] * 4)
        m = raster.from_grey(4, 4, grey, None, 0.4, "dark", 128, False)
        self.assertEqual(list(m.bits[:4]), [1, 1, 0, 0])

    def test_invert_swaps_art(self):
        grey = bytearray([0] * 8 + [255] * 8)
        a = raster.from_grey(4, 4, grey, None, 0.4, "dark", 128, False)
        b = raster.from_grey(4, 4, grey, None, 0.4, "dark", 128, True)
        self.assertEqual(a.count() + b.count(), 16)
        self.assertEqual(a.bits[:8], b"\x01" * 8)
        self.assertEqual(b.bits[8:], b"\x01" * 8)

    def test_alpha_masks_after_invert(self):
        grey = bytearray([0] * 16)
        alpha = bytearray([255, 255, 0, 0] * 4)
        for invert in (False, True):
            grey_in = bytearray([0, 255, 0, 255] * 4) if invert else grey
            m = raster.from_grey(4, 4, grey_in, alpha, 0.4, "dark", 128, invert)
            self.assertEqual(list(m.bits[:4]), [0, 1, 0, 0] if invert else [1, 1, 0, 0])
            self.assertTrue(all(m.bits[y * 4 + x] == 0 for y in range(4) for x in (2, 3)))

    def test_upscale_averages_by_area(self):
        # A 2x1 image of 0 and 200 scaled to 4 px: the mid pixels sit on the seam.
        m = raster.from_grey(2, 1, bytearray([0, 200]), None, 0.3, "dark", 100, False)
        self.assertEqual((m.w, m.h), (3, 2))

    def test_coarse_source_warns(self):
        grey = bytearray([0, 255] * 50)
        m = raster.from_grey(10, 10, grey, None, 10.0, "dark", 128, False)
        self.assertTrue(any("source pixel" in w for w in m.warnings))
        fine = raster.from_grey(100, 100, bytearray([0, 255] * 5000), None, 10.0, "dark", 128,
                                False)
        self.assertEqual(fine.warnings, [])

    def test_all_one_colour_names_threshold_and_range(self):
        with self.assertRaises(DecalError) as ctx:
            raster.from_grey(8, 8, bytearray([90] * 64), None, 0.8, "dark", 40, False)
        msg = str(ctx.exception)
        self.assertIn("threshold of 40", msg)
        self.assertIn("90 to 90", msg)

    def test_unknown_mode(self):
        with self.assertRaises(DecalError):
            raster.from_grey(2, 2, bytearray(4), None, 0.2, "stipple", 1, False)

    def test_shading_finds_a_groove_under_uneven_light(self):
        w, h = 120, 60
        grey = bytearray()
        for y in range(h):
            for x in range(w):
                light = 100 + x  # lighting ramp across the picture
                groove = 50 <= x < 56
                grey.append(int(light * (0.45 if groove else 1.0)))
        m = raster.from_grey(w, h, grey, None, 12.0, "shading", 8, False)
        self.assertEqual((m.w, m.h), (120, 60))
        groove_cols = sum(m.bits[30 * 120 + x] for x in range(50, 56))
        # the ramp is steep, so the first columns read as shadow against the clamped blur
        flat_cols = sum(m.bits[30 * 120 + x] for x in list(range(12, 35)) + list(range(75, 110)))
        self.assertGreaterEqual(groove_cols, 5)
        self.assertEqual(flat_cols, 0)


class Resample(unittest.TestCase):
    def test_exact_area_average(self):
        data = bytearray([0, 100, 200, 40, 60, 80])
        rows = raster._resample(data, 3, 2, 2, 1)
        # output columns cover source x in [0, 1.5) and [1.5, 3), both rows averaged
        self.assertAlmostEqual(rows[0][0], 40.0)
        self.assertAlmostEqual(rows[0][1], 120.0)

    def test_identity(self):
        data = bytearray(range(12))
        rows = raster._resample(data, 4, 3, 4, 3)
        self.assertEqual([list(r) for r in rows],
                         [[float(v) for v in data[i:i + 4]] for i in range(0, 12, 4)])


class FromLoops(unittest.TestCase):
    def test_square_scaled_to_width(self):
        m = raster.from_loops([H.rect_loop(0, 0, 20, 20)], 10.0)
        self.assertEqual((m.w, m.h), (100, 100))
        self.assertEqual(m.count(), 10000)

    def test_y_up_input_stays_upright(self):
        # apex at the top (largest y) means the first mask row is the narrowest.
        m = raster.from_loops([[(0, 0), (10, 0), (5, 10)]], 5.0)
        top = sum(m.bits[:m.w])
        bottom = sum(m.bits[(m.h - 1) * m.w:])
        self.assertLess(top, bottom)

    def test_even_odd_ring(self):
        m = raster.from_loops([H.rect_loop(0, 0, 10, 10), H.rect_loop(3, 3, 7, 7)], 5.0)
        self.assertAlmostEqual(m.count() * P * P, 25 - 4, delta=0.2)

    def test_empty_is_an_error(self):
        with self.assertRaises(DecalError):
            raster.from_loops([], 5.0)


class Orientation(unittest.TestCase):
    PATTERN = ["#..", "##.", "..."]

    def naive_rotate_cw(self, m):
        out = raster.Mask(m.h, m.w)
        for y in range(m.h):
            for x in range(m.w):
                out.bits[x * m.h + (m.h - 1 - y)] = m.bits[y * m.w + x]
        return out

    def test_rotate_quarter_turns(self):
        base = H.mask_from_text(["#..#", "##..", "...#"])
        cur = base
        for turns in range(1, 5):
            cur = self.naive_rotate_cw(cur)
            got = raster.rotate(base, turns)
            self.assertEqual((got.w, got.h, bytes(got.bits)), (cur.w, cur.h, bytes(cur.bits)))
        self.assertEqual(bytes(raster.rotate(base, 4).bits), bytes(base.bits))
        self.assertEqual(bytes(raster.rotate(base, -1).bits), bytes(raster.rotate(base, 3).bits))

    def test_rotate_is_clockwise_on_screen(self):
        # the pixel at the top-left corner goes to the top-right corner
        m = H.mask_from_text(["#..", "...", "..."])
        r = raster.rotate(m, 1)
        self.assertEqual(r.bits[2], 1)

    def test_mirror(self):
        m = H.mask_from_text(["#.", "##", ".#"])
        self.assertEqual(bytes(raster.mirror(m).bits), bytes([0, 1, 1, 1, 1, 0]))
        self.assertEqual(bytes(raster.mirror(raster.mirror(m)).bits), bytes(m.bits))


class Morphology(unittest.TestCase):
    def test_dilate_and_erode_match_the_distance_threshold(self):
        rng = random.Random(3)
        w, h = 40, 33
        m = raster.Mask(w, h)
        for _ in range(5):
            x0, y0 = rng.randint(4, 24), rng.randint(4, 18)
            for y in range(y0, y0 + rng.randint(3, 9)):
                for x in range(x0, min(w - 4, x0 + rng.randint(3, 10))):
                    m.bits[y * w + x] = 1
        r = 0.325
        near_art = H.distance_mm(m, True)
        near_air = H.distance_mm(m, False)
        dil = raster.dilate(m, r)
        ero = raster.erode(m, r)
        for i in range(w * h):
            self.assertEqual(dil.bits[i] == 1, near_art[i] <= r, i)
            self.assertEqual(ero.bits[i] == 1, m.bits[i] == 1 and near_air[i] > r, i)


class OpenClose(unittest.TestCase):
    def bars(self):
        # horizontal bars of 6, 8 and 10 px (0.6, 0.8, 1.0 mm), each its own piece
        return H.mask_rect(120, 70, [(10, 5, 100, 11), (10, 20, 100, 28), (10, 40, 100, 50)])

    def test_thin_bar_goes_and_wide_bars_stay(self):
        out, lost = raster.open_close(self.bars(), 0.8)
        rows = [sum(out.bits[y * 120:(y + 1) * 120]) for y in range(70)]
        self.assertEqual(sum(rows[0:15]), 0)
        self.assertGreater(sum(rows[20:28]), 8 * 80)
        self.assertGreater(sum(rows[40:50]), 10 * 80)
        self.assertEqual(lost, 1)

    def test_bar_exactly_min_width_is_kept(self):
        m = H.mask_rect(60, 30, [(5, 10, 55, 18)])
        out, lost = raster.open_close(m, 0.8)
        self.assertEqual(lost, 0)
        # the round element trims the four corners and nothing else
        self.assertGreaterEqual(out.count(), 50 * 8 - 20)
        self.assertEqual(sum(out.bits[14 * 60:15 * 60]), 50)

    def test_bar_just_under_min_width_goes(self):
        m = H.mask_rect(60, 30, [(5, 10, 55, 17)])
        out, lost = raster.open_close(m, 0.8)
        self.assertEqual(out.count(), 0)
        self.assertEqual(lost, 1)

    def test_art_ends_at_the_frame_edge(self):
        # edges stay straight, only the four corners round off
        m = H.mask_rect(60, 30, [(0, 0, 60, 30)])
        out, lost = raster.open_close(m, 0.8)
        self.assertEqual(lost, 0)
        self.assertEqual(sum(out.bits[15 * 60:16 * 60]), 60)
        self.assertLess(sum(out.bits[0:60]), 60)
        self.assertGreaterEqual(out.count(), 60 * 30 - 4 * 8)

    def test_sliver_in_the_frame_corner_goes(self):
        m = H.mask_rect(60, 30, [(0, 0, 40, 30), (54, 24, 60, 30)])
        out, lost = raster.open_close(m, 0.8)
        self.assertEqual(sum(out.bits[27 * 60 + 54:27 * 60 + 60]), 0)
        self.assertEqual(lost, 1)

    def test_narrow_gap_fills_and_wide_gap_stays(self):
        # two bars 6 px apart and two bars 12 px apart
        m = H.mask_rect(100, 90, [(10, 5, 90, 20), (10, 26, 90, 40),
                                  (10, 50, 90, 62), (10, 74, 90, 86)])
        out, lost = raster.open_close(m, 0.8)
        self.assertEqual(sum(out.bits[22 * 100 + 20:22 * 100 + 80]), 60)  # filled
        self.assertEqual(sum(out.bits[68 * 100:69 * 100]), 0)   # kept open
        self.assertEqual(lost, 1)  # the two bars merged

    def test_small_minimum_changes_nothing(self):
        m = self.bars()
        out, lost = raster.open_close(m, 0.05)
        self.assertEqual(bytes(out.bits), bytes(m.bits))
        self.assertEqual(lost, 0)

    def test_opening_is_a_subset_and_idempotent(self):
        rng = random.Random(11)
        m = raster.Mask(50, 40, bytearray(1 if rng.random() < 0.55 else 0 for _ in range(2000)))
        once, _ = raster.open_close(m, 0.6)
        twice, _ = raster.open_close(once, 0.6)
        self.assertEqual(bytes(once.bits), bytes(twice.bits))


class Pieces(unittest.TestCase):
    def test_diagonal_pixels_join_under_eight_connectivity(self):
        m = H.mask_from_text(["#..", ".#.", "..#"])
        self.assertEqual(raster.count_pieces(m), 1)

    def test_separate_squares(self):
        m = H.mask_rect(30, 10, [(1, 1, 5, 5), (10, 1, 14, 5), (20, 2, 24, 8)])
        self.assertEqual(raster.count_pieces(m), 3)

    def test_u_shape_is_one_piece(self):
        m = H.mask_from_text(["#.#", "#.#", "###"])
        self.assertEqual(raster.count_pieces(m), 1)

    def test_empty(self):
        self.assertEqual(raster.count_pieces(raster.Mask(5, 5)), 0)

    def test_four_connected_labels_split_a_diagonal(self):
        m = H.mask_from_text(["#.", ".#"])
        self.assertEqual(raster.label(m, False).count, 2)


class Intersect(unittest.TestCase):
    def test_and(self):
        a = H.mask_from_text(["##..", "##.."])
        b = H.mask_from_text(["#.#.", ".###"])
        self.assertEqual(bytes(raster.intersect(a, b).bits), bytes([1, 0, 0, 0, 0, 1, 0, 0]))

    def test_size_mismatch(self):
        with self.assertRaises(ValueError):
            raster.intersect(raster.Mask(2, 2), raster.Mask(3, 2))


if __name__ == "__main__":
    unittest.main()
