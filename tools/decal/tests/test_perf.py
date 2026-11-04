"""Speed gate. Fusion runs this code in its own thread, so a slow loop freezes the program."""

import math
import random
import time
import unittest

from decal_core import plan
from decal_core.plan import Face2D, Source

from . import helpers as H

LIMIT_SECONDS = 60.0


def synthetic_photo(w=1000, h=600, seed=5):
    """Uneven light, a pattern of ridges and grain, as a stand-in for a relief photograph."""
    rng = random.Random(seed)
    grey = bytearray()
    for y in range(h):
        cy = math.cos(y / 29.0)
        gy = math.sin(y / 11.0)
        row = []
        for x in range(w):
            v = (120 + 0.06 * x + 50 * math.sin(x / 37.0) * cy + 30 * math.sin(x / 11.0 + gy)
                 + rng.uniform(-14, 14))
            row.append(0 if v < 0 else 255 if v > 255 else int(v))
        grey += bytes(row)
    return Source(kind='image', w=w, h=h, grey=grey)


def plate_150x90():
    holes = [(-55.0, 20.0, 3.0), (-30.0, -20.0, 5.0), (0.0, 0.0, 7.0), (25.0, 20.0, 7.0),
             (45.0, -15.0, 12.0), (60.0, 25.0, 3.0)]
    return Face2D(kind='flat', outer=H.rect_loop(-75, -45, 75, 45), holes=holes,
                  centre=(0.0, 0.0), width_mm=150.0)


def dense_blobs(w=1000, h=600, seed=9):
    """Blobs about a millimetre across over the whole picture, the worst case for tracing."""
    rng = random.Random(seed)
    grey = bytearray()
    for y in range(h):
        sy = math.sin(y / 2.6)
        row = [max(0, min(255, int(128 + 110 * math.sin(x / 2.5) * sy + rng.uniform(-6, 6))))
               for x in range(w)]
        grey += bytes(row)
    return Source(kind='image', w=w, h=h, grey=grey)


KEEPOUTS = {3.0: 6.0, 5.0: 8.0, 7.0: 16.0, 12.0: 20.0}


class ComputeSpeed(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.photo = synthetic_photo()

    def run_mode(self, name, settings, photo=None):
        start = time.perf_counter()
        result = plan.compute(photo or self.photo, plate_150x90(),
                              dict(settings, hole_keepouts=KEEPOUTS))
        total = time.perf_counter() - start
        stages = ", ".join(f"{k} {v:.2f}" for k, v in result.timings.items())
        print(f"\ncompute {name}, 1000x600 photo on 150x90 mm, four hole sizes: {total:.2f} s "
              f"({stages}); {result.pieces_count} pieces, {result.points} points, "
              f"dropped {result.dropped_thin} thin and {result.dropped_small} small")
        self.assertLess(total, LIMIT_SECONDS)
        self.assertGreater(result.pieces_count, 0)

    def test_dark_mode(self):
        self.run_mode("dark", {'mode': 'dark', 'threshold': 128})

    def test_shading_mode(self):
        self.run_mode("shading", {'mode': 'shading', 'threshold': 6})

    def test_dense_blobs_worst_case_trace(self):
        self.run_mode("dense dark", {'mode': 'dark', 'threshold': 128}, dense_blobs())
        self.run_mode("dense shading", {'mode': 'shading', 'threshold': 0}, dense_blobs())


if __name__ == "__main__":
    unittest.main()
