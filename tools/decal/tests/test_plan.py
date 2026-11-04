import math
import unittest

from decal_core import DecalError, plan, raster, trace
from decal_core.plan import Face2D, Source

from . import helpers as H


def flat_face(w=60.0, h=40.0, holes=(), inners=()):
    return Face2D(kind='flat', outer=H.rect_loop(-w / 2, -h / 2, w / 2, h / 2),
                  inners=list(inners), holes=list(holes), centre=(0.0, 0.0), width_mm=w)


def solid(w=596.0, h=396.0):
    """Art that fills a 59.6 by 39.6 mm area, the default size on the 60 by 40 face."""
    return Source(kind='loops', loops=[H.rect_loop(0, 0, w, h)])


def hole_area(piece, index=0):
    return abs(H.shoelace(piece.holes[index]))


class KeepOuts(unittest.TestCase):
    def test_sixteen_mm_keepout_round_a_seven_mm_hole(self):
        r = plan.compute(solid(), flat_face(holes=[(0.0, 0.0, 7.0)]),
                         {'hole_keepouts': {7.0: 16.0}})
        self.assertEqual(r.pieces_count, 1)
        piece = r.pieces[0]
        self.assertEqual(len(piece.holes), 1)
        self.assertAlmostEqual(hole_area(piece), math.pi * 64.0, delta=0.02 * math.pi * 64.0)
        self.assertAlmostEqual(piece.area_mm2, 59.6 * 39.6 - math.pi * 64.0, delta=0.01 * 2360)
        xs = [p[0] for p in piece.holes[0]]
        self.assertAlmostEqual(max(xs) - min(xs), 16.0, delta=0.2)

    def test_default_keepout_is_hole_plus_two_margins(self):
        r = plan.compute(solid(), flat_face(holes=[(0.0, 0.0, 7.0)]), {})
        self.assertAlmostEqual(hole_area(r.pieces[0]), math.pi * 3.7 ** 2, delta=1.5)

    def test_keepout_table_accepts_string_keys(self):
        r = plan.compute(solid(), flat_face(holes=[(0.0, 0.0, 7.0)]),
                         {'hole_keepouts': {'7.0': 16.0}})
        self.assertAlmostEqual(hole_area(r.pieces[0]), math.pi * 64.0, delta=4.0)

    def test_each_hole_size_has_its_own_keepout(self):
        face = flat_face(holes=[(-15.0, 0.0, 3.0), (15.0, 0.0, 7.0)])
        r = plan.compute(solid(), face, {'hole_keepouts': {3.0: 6.0, 7.0: 12.0}})
        areas = sorted(hole_area(r.pieces[0], i) for i in range(2))
        self.assertAlmostEqual(areas[0], math.pi * 9.0, delta=1.0)
        self.assertAlmostEqual(areas[1], math.pi * 36.0, delta=2.5)

    def test_keepouts_off_cuts_only_the_hole_itself(self):
        r = plan.compute(solid(), flat_face(holes=[(0.0, 0.0, 7.0)]), {'keepouts': False})
        self.assertAlmostEqual(hole_area(r.pieces[0]), math.pi * 3.5 ** 2, delta=1.0)

    def test_keepouts_off_runs_art_to_the_face_edge(self):
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 600, 400)])
        face = flat_face()
        on = plan.compute(src, face, {'width_mm': 60.0})
        off = plan.compute(src, face, {'width_mm': 60.0, 'keepouts': False})
        self.assertAlmostEqual(off.area_mm2, 2400.0, delta=5.0)
        self.assertAlmostEqual(on.area_mm2, 59.6 * 39.6, delta=5.0)

    def test_other_cutouts_get_the_margin(self):
        slot = H.rect_loop(-5, -2, 5, 2)
        r = plan.compute(solid(), flat_face(inners=[slot]), {})
        hole = r.pieces[0].holes[0]
        xs = [p[0] for p in hole]
        ys = [p[1] for p in hole]
        self.assertAlmostEqual(max(xs) - min(xs), 10.4, delta=0.15)
        self.assertAlmostEqual(max(ys) - min(ys), 4.4, delta=0.15)

    def test_margin_setting_trims_the_outer_edge(self):
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 600, 400)])
        r = plan.compute(src, flat_face(), {'width_mm': 60.0, 'margin_mm': 1.0})
        self.assertAlmostEqual(r.area_mm2, 58.0 * 38.0, delta=6.0)


class Placement(unittest.TestCase):
    def bbox(self, result):
        pts = [p for piece in result.pieces for p in piece.outer]
        return (min(p[0] for p in pts), min(p[1] for p in pts),
                max(p[0] for p in pts), max(p[1] for p in pts))

    def test_art_is_centred_on_the_click(self):
        face = flat_face()
        face.centre = (4.0, -3.0)
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 20, 10)])
        x0, y0, x1, y1 = self.bbox(plan.compute(src, face, {'width_mm': 20.0}))
        self.assertAlmostEqual((x0 + x1) / 2, 4.0, delta=0.06)
        self.assertAlmostEqual((y0 + y1) / 2, -3.0, delta=0.06)
        self.assertAlmostEqual(x1 - x0, 20.0, delta=0.1)
        self.assertAlmostEqual(y1 - y0, 10.0, delta=0.1)

    def test_offset(self):
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 20, 10)])
        r = plan.compute(src, flat_face(), {'width_mm': 20.0, 'offset_x': 5.0, 'offset_y': -3.0})
        x0, y0, x1, y1 = self.bbox(r)
        self.assertAlmostEqual((x0 + x1) / 2, 5.0, delta=0.06)
        self.assertAlmostEqual((y0 + y1) / 2, -3.0, delta=0.06)

    def test_art_stays_upright_with_y_up(self):
        src = Source(kind='loops', loops=[[(0, 0), (10, 0), (5, 10)]])
        r = plan.compute(src, flat_face(), {'width_mm': 10.0, 'min_width_mm': 0.1})
        outer = r.pieces[0].outer
        apex = max(outer, key=lambda p: p[1])
        self.assertAlmostEqual(apex[0], 0.0, delta=0.3)
        self.assertAlmostEqual(apex[1], 5.0, delta=0.1)

    def test_rotation_swaps_the_extents(self):
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 20, 10)])
        x0, y0, x1, y1 = self.bbox(plan.compute(src, flat_face(), {'width_mm': 20.0,
                                                                   'rotation': 90}))
        self.assertAlmostEqual(x1 - x0, 10.0, delta=0.1)
        self.assertAlmostEqual(y1 - y0, 20.0, delta=0.1)

    def test_rotation_is_clockwise(self):
        # an L with its long arm up and its foot right; clockwise 90 puts the arm to the right
        ell = [(0, 0), (10, 0), (10, 3), (3, 3), (3, 10), (0, 10)]
        src = Source(kind='loops', loops=[ell])
        r = plan.compute(src, flat_face(), {'width_mm': 10.0, 'min_width_mm': 0.1,
                                            'rotation': 90})
        outer = r.pieces[0].outer
        # the stem now runs along the top, so the points at the far right are high
        right = [p for p in outer if p[0] > 4.9]
        self.assertTrue(right)
        self.assertTrue(all(p[1] > 0 for p in right))

    def test_mirror_flips_left_to_right(self):
        ell = [(0, 0), (10, 0), (10, 3), (3, 3), (3, 10), (0, 10)]
        src = Source(kind='loops', loops=[ell])
        plain = plan.compute(src, flat_face(), {'width_mm': 10.0, 'min_width_mm': 0.1})
        flipped = plan.compute(src, flat_face(), {'width_mm': 10.0, 'min_width_mm': 0.1,
                                                  'mirror': True})
        top_plain = max(plain.pieces[0].outer, key=lambda p: p[1])
        top_flipped = max(flipped.pieces[0].outer, key=lambda p: p[1])
        self.assertLess(top_plain[0], 0)
        self.assertGreater(top_flipped[0], 0)

    def test_bad_rotation(self):
        with self.assertRaises(DecalError) as ctx:
            plan.compute(solid(), flat_face(), {'rotation': 45})
        self.assertIn("45", str(ctx.exception))


class Orientation(unittest.TestCase):
    def test_outer_counter_clockwise_and_holes_clockwise_in_face_coordinates(self):
        r = plan.compute(solid(), flat_face(holes=[(0.0, 0.0, 7.0)]), {})
        piece = r.pieces[0]
        self.assertGreater(H.shoelace(piece.outer), 0)
        self.assertLess(H.shoelace(piece.holes[0]), 0)
        self.assertGreater(piece.area_mm2, 0)


class Counts(unittest.TestCase):
    def test_fields_agree(self):
        r = plan.compute(solid(), flat_face(holes=[(0.0, 0.0, 7.0)]), {})
        self.assertEqual(r.points, trace.points(r.pieces))
        self.assertEqual(r.pieces_count, len(r.pieces))
        self.assertAlmostEqual(r.area_mm2, sum(p.area_mm2 for p in r.pieces))
        self.assertAlmostEqual(r.est_minutes, r.points / plan.POINTS_PER_MINUTE)
        self.assertIsNone(r.over)
        cyl = flat_face(holes=[(0.0, 0.0, 7.0)])
        cyl.kind = 'cylinder'
        c = plan.compute(solid(), cyl, {})
        self.assertAlmostEqual(c.est_minutes, c.points / 5000 + c.pieces_count * 0.55 / 60)
        self.assertGreater(c.est_minutes, r.est_minutes)
        self.assertEqual(set(r.timings), {'mask', 'orient', 'keepouts', 'width', 'trace', 'map'})

    def test_thin_and_small_drops_are_counted(self):
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 200, 200),
                                          H.rect_loop(220, 0, 225, 200),   # 0.5 mm wide bar
                                          H.rect_loop(300, 0, 306, 6)])    # 0.36 mm2 square
        r = plan.compute(src, flat_face(), {'width_mm': 30.6, 'min_width_mm': 0.3,
                                            'min_area_mm2': 0.5})
        self.assertEqual(r.dropped_small, 1)
        self.assertEqual(r.pieces_count, 2)
        r = plan.compute(src, flat_face(), {'width_mm': 30.6, 'min_width_mm': 0.8,
                                            'min_area_mm2': 0.0})
        self.assertGreaterEqual(r.dropped_thin, 1)
        self.assertEqual(r.pieces_count, 1)

    def test_invert_on_loops_keeps_the_corners(self):
        src = Source(kind='loops', loops=[H.circle_loop(0, 0, 5)])
        r = plan.compute(src, flat_face(), {'width_mm': 10.0, 'invert': True,
                                            'min_width_mm': 0.3})
        self.assertEqual(r.pieces_count, 4)
        self.assertAlmostEqual(r.area_mm2, 100 - math.pi * 25, delta=1.5)


class Ceilings(unittest.TestCase):
    def test_constants(self):
        self.assertEqual((plan.CEILING_POINTS, plan.CEILING_PIECES, plan.POINTS_PER_MINUTE),
                         (10_000, 1_000, 5_000))
        self.assertEqual(plan.CYLINDER_SECONDS_PER_PIECE, 0.55)

    def test_check_trips_just_above_the_values(self):
        self.assertIsNone(plan.check_ceilings(10_000, 1_000))
        self.assertIsNone(plan.check_ceilings(0, 0))
        msg = plan.check_ceilings(10_001, 1)
        self.assertIn("10,001", msg)
        self.assertIn("10,000", msg)
        self.assertNotIn("pieces", msg)
        msg = plan.check_ceilings(1, 1_001)
        self.assertIn("1,001", msg)
        self.assertIn("1,000", msg)
        both = plan.check_ceilings(20_000, 2_000)
        self.assertIn("points", both)
        self.assertIn("pieces", both)

    def test_piece_ceiling_trips_in_compute(self):
        n, side, pitch = 32, 1.2, 2.0
        squares = [H.rect_loop(i * pitch, j * pitch, i * pitch + side, j * pitch + side)
                   for i in range(n) for j in range(n)]
        width = (n - 1) * pitch + side
        face = flat_face(100.0, 100.0)
        r = plan.compute(Source(kind='loops', loops=squares), face, {'width_mm': width})
        self.assertEqual(r.pieces_count, 1024)
        self.assertGreaterEqual(r.points, 4096)  # the minimum width rounds the corners
        self.assertIn("pieces", r.over)
        self.assertNotIn("points", r.over)

    def test_point_ceiling_trips_in_compute(self):
        loops = []
        for col in range(12):
            for row in range(30):
                x0, y0 = col * 14.0, row * 4.0
                pts = [(x0, y0), (x0 + 12.0, y0)]
                for t in range(11, -1, -1):
                    tx = x0 + t * 1.0
                    pts += [(tx + 0.75, y0 + 2.0), (tx + 0.75, y0 + 2.5),
                            (tx + 0.25, y0 + 2.5), (tx + 0.25, y0 + 2.0)]
                loops.append(pts)
        face = flat_face(200.0, 150.0)
        r = plan.compute(Source(kind='loops', loops=loops), face,
                         {'width_mm': 166.0, 'min_width_mm': 0.3})
        self.assertEqual(r.pieces_count, 360)
        self.assertGreater(r.points, plan.CEILING_POINTS)
        self.assertIn("points", r.over)
        self.assertNotIn("pieces", r.over)


class Errors(unittest.TestCase):
    def test_art_wider_than_a_flat_face(self):
        with self.assertRaises(DecalError) as ctx:
            plan.compute(solid(), flat_face(), {'width_mm': 80.0})
        msg = str(ctx.exception)
        self.assertIn("80.0 mm", msg)
        self.assertIn("60.0 mm", msg)

    def test_art_wider_than_a_cylinder_arc(self):
        face = flat_face()
        face.kind = 'cylinder'
        with self.assertRaises(DecalError) as ctx:
            plan.compute(solid(), face, {'width_mm': 59.9})
        self.assertIn("arc length", str(ctx.exception))
        plan.compute(solid(), face, {})

    def test_nothing_left_names_the_counts(self):
        src = Source(kind='loops', loops=[H.rect_loop(0, 0, 5, 5)])
        with self.assertRaises(DecalError) as ctx:
            plan.compute(src, flat_face(), {'width_mm': 0.5})
        msg = str(ctx.exception)
        self.assertIn("Nothing is left", msg)
        self.assertIn("1 pieces before", msg)

    def test_nothing_left_when_every_piece_is_inside_a_keepout(self):
        src = Source(kind='loops', loops=[H.circle_loop(0, 0, 3)])
        face = flat_face(holes=[(0.0, 0.0, 6.0)])
        with self.assertRaises(DecalError):
            plan.compute(src, face, {'width_mm': 6.0})

    def test_flat_image_names_threshold_and_range(self):
        src = Source(kind='image', w=20, h=10, grey=bytearray([200] * 200))
        with self.assertRaises(DecalError) as ctx:
            plan.compute(src, flat_face(), {'threshold': 100})
        self.assertIn("threshold of 100", str(ctx.exception))

    def test_unknown_source_kind(self):
        with self.assertRaises(DecalError):
            plan.compute(Source(kind='text'), flat_face(), {})

    def test_face_too_narrow_for_the_margin(self):
        with self.assertRaises(DecalError):
            plan.compute(solid(), flat_face(0.3, 5.0), {})


class Image(unittest.TestCase):
    def test_black_disc_on_white(self):
        w, h = 200, 100
        grey = bytearray()
        for y in range(h):
            grey += bytes(0 if (x + 0.5 - 100) ** 2 + (y + 0.5 - 50) ** 2 <= 900 else 255
                          for x in range(w))
        src = Source(kind='image', w=w, h=h, grey=grey)
        r = plan.compute(src, flat_face(40.0, 30.0), {})
        self.assertEqual(r.pieces_count, 1)
        radius = 30 * 39.6 / 200
        self.assertAlmostEqual(r.area_mm2, math.pi * radius ** 2, delta=0.02 * math.pi * radius ** 2)
        xs = [p[0] for p in r.pieces[0].outer]
        self.assertAlmostEqual((min(xs) + max(xs)) / 2, 0.0, delta=0.1)
        self.assertEqual(r.warnings, [])

    def test_transparent_pixels_are_not_art(self):
        w, h = 100, 50
        grey = bytearray(bytes([0]) * (w * h))
        alpha = bytearray()
        for y in range(h):
            alpha += bytes(255 if x < 50 else 0 for x in range(w))
        r = plan.compute(Source(kind='image', w=w, h=h, grey=grey, alpha=alpha),
                         flat_face(40.0, 30.0), {})
        xs = [p[0] for p in r.pieces[0].outer]
        self.assertLess(max(xs), 0.2)


class Cancelled(Exception):
    pass


def groove_image():
    """A ramp of light with a dark groove, traced in shading mode so every stage runs."""
    w, h = 120, 60
    grey = bytearray()
    for _y in range(h):
        grey += bytes(int((100 + x) * (0.45 if 50 <= x < 56 else 1.0)) for x in range(w))
    return Source(kind='image', w=w, h=h, grey=grey)


GROOVE = {'mode': 'shading', 'threshold': 8, 'width_mm': 12.0, 'min_width_mm': 0.3}


class Progress(unittest.TestCase):
    def record(self, source, settings):
        calls = []
        result = plan.compute(source, flat_face(20.0, 10.0), settings,
                              progress=lambda stage, fraction: calls.append((stage, fraction)))
        return calls, result

    def test_calls_run_from_zero_to_one_without_going_back(self):
        calls, _ = self.record(groove_image(), GROOVE)
        fractions = [f for _, f in calls]
        self.assertEqual(calls[0], ('mask', 0.0))
        self.assertEqual(calls[-1], ('map', 1.0))
        self.assertEqual(fractions, sorted(fractions))
        self.assertTrue(all(0.0 <= f <= 1.0 for f in fractions))
        order = []
        for stage, _ in calls:
            if stage not in order:
                order.append(stage)
        self.assertEqual(order, [name for name, _ in plan.STAGE_WEIGHTS])

    def test_every_stage_boundary_is_reported(self):
        calls, _ = self.record(groove_image(), GROOVE)
        done = 0.0
        for name, weight in plan.STAGE_WEIGHTS:
            done += weight
            last = [f for stage, f in calls if stage == name][-1]
            self.assertAlmostEqual(last, min(done, 1.0))

    def test_slow_loops_report_inside_their_stage(self):
        calls, _ = self.record(groove_image(), GROOVE)
        per_stage = {name: [f for stage, f in calls if stage == name]
                     for name, _ in plan.STAGE_WEIGHTS}
        self.assertGreater(len(per_stage['mask']), 20)
        self.assertGreater(len(per_stage['width']), 3)
        self.assertGreater(len(per_stage['trace']), 3)
        self.assertLess(len(calls), 600)
        gaps = [b - a for (_, a), (_, b) in zip(calls, calls[1:])]
        self.assertLess(max(gaps), 0.1)

    def test_progress_does_not_change_the_result(self):
        _, with_progress = self.record(groove_image(), GROOVE)
        without = plan.compute(groove_image(), flat_face(20.0, 10.0), GROOVE)
        self.assertEqual([(p.outer, p.holes, p.area_mm2) for p in with_progress.pieces],
                         [(p.outer, p.holes, p.area_mm2) for p in without.pieces])
        self.assertEqual((with_progress.points, with_progress.dropped_thin,
                          with_progress.dropped_small),
                         (without.points, without.dropped_thin, without.dropped_small))

    def test_loops_source_and_dark_image_report_too(self):
        calls = []
        plan.compute(solid(), flat_face(), {}, progress=lambda s, f: calls.append(f))
        self.assertEqual((calls[0], calls[-1]), (0.0, 1.0))
        self.assertEqual(calls, sorted(calls))

    def test_image_with_alpha_reports_to_the_end_of_the_mask_stage(self):
        w, h = 100, 50
        alpha = bytearray(bytes(255 if x < 50 else 0 for x in range(w)) * h)
        calls, _ = self.record(Source(kind='image', w=w, h=h, grey=bytearray(w * h), alpha=alpha),
                               {'width_mm': 10.0})
        fractions = [f for _, f in calls]
        self.assertEqual(fractions, sorted(fractions))

    def test_callback_raising_during_the_mask_cancels(self):
        def stop(stage, fraction):
            if stage == 'mask' and fraction > 0.1:
                raise Cancelled()

        with self.assertRaises(Cancelled):
            plan.compute(groove_image(), flat_face(20.0, 10.0), GROOVE, progress=stop)

    def test_callback_raising_during_the_trace_cancels(self):
        seen = []

        def stop(stage, fraction):
            seen.append((stage, fraction))
            if stage == 'trace' and fraction > 0.8:
                raise Cancelled()

        with self.assertRaises(Cancelled):
            plan.compute(groove_image(), flat_face(20.0, 10.0), GROOVE, progress=stop)
        self.assertEqual(seen[-1][0], 'trace')
        self.assertNotIn('map', [stage for stage, _ in seen])

    def test_old_call_without_progress_still_works(self):
        result = plan.compute(solid(), flat_face(), {})
        self.assertEqual(result.pieces_count, 1)


if __name__ == "__main__":
    unittest.main()
