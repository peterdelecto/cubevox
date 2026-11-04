import os
import random
import struct
import unittest
import zlib

from decal_core import DecalError, png

from . import helpers as H

W, HT = 7, 6  # width 7 leaves partial bytes at every sub-byte depth


def expected(colour_type, depth, rows, palette, trns):
    """Independent decode of raw sample rows to (grey, alpha-or-None)."""
    grey, alpha = [], []
    has_alpha = colour_type in (4, 6) or trns is not None
    for row in rows:
        if colour_type == 0:
            for v in row:
                g = v * (255 // ((1 << depth) - 1)) if depth < 8 else (v >> 8 if depth == 16 else v)
                grey.append(g)
                if trns is not None:
                    alpha.append(0 if v == struct.unpack(">H", trns)[0] else 255)
        elif colour_type == 2:
            for i in range(0, len(row), 3):
                r, g, b = (c >> 8 if depth == 16 else c for c in row[i:i + 3])
                grey.append(H.luma(r, g, b))
                if trns is not None:
                    alpha.append(0 if tuple(row[i:i + 3]) == struct.unpack(">3H", trns) else 255)
        elif colour_type == 3:
            for v in row:
                r, g, b = palette[v]
                grey.append(H.luma(r, g, b))
                alpha.append(trns[v] if trns is not None and v < len(trns) else 255)
        elif colour_type == 4:
            for i in range(0, len(row), 2):
                grey.append(row[i] >> 8 if depth == 16 else row[i])
                alpha.append(row[i + 1] >> 8 if depth == 16 else row[i + 1])
        else:
            for i in range(0, len(row), 4):
                r, g, b, a = (c >> 8 if depth == 16 else c for c in row[i:i + 4])
                grey.append(H.luma(r, g, b))
                alpha.append(a)
    return bytearray(grey), (bytearray(alpha) if has_alpha else None)


class DecodeEveryMode(unittest.TestCase):
    def check(self, colour_type, depth, palette=None, trns=None, seed=1):
        per_pixel = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour_type]
        rng = random.Random(seed)
        rows = H.random_rows(rng, W, HT, per_pixel, depth)
        path = H.write_temp(H.png_bytes(W, HT, colour_type, depth, rows, palette, trns))
        self.addCleanup(os.remove, path)
        w, h, grey, alpha = png.read(path)
        want_grey, want_alpha = expected(colour_type, depth, rows, palette, trns)
        self.assertEqual((w, h), (W, HT))
        self.assertEqual(bytes(grey), bytes(want_grey))
        if want_alpha is None:
            self.assertIsNone(alpha)
        else:
            self.assertEqual(bytes(alpha), bytes(want_alpha))

    def test_grey_every_depth(self):
        for depth in (1, 2, 4, 8, 16):
            with self.subTest(depth=depth):
                self.check(0, depth)

    def test_rgb_every_depth(self):
        for depth in (8, 16):
            with self.subTest(depth=depth):
                self.check(2, depth)

    def test_palette_every_depth(self):
        for depth in (1, 2, 4, 8):
            with self.subTest(depth=depth):
                rng = random.Random(depth)
                palette = [(rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255))
                           for _ in range(1 << depth)]
                self.check(3, depth, palette=palette)

    def test_palette_with_transparency(self):
        palette = [(0, 0, 0), (255, 255, 255), (200, 30, 30), (10, 200, 10)]
        self.check(3, 2, palette=palette, trns=bytes([0, 128, 255]))

    def test_grey_alpha(self):
        for depth in (8, 16):
            with self.subTest(depth=depth):
                self.check(4, depth)

    def test_rgba(self):
        for depth in (8, 16):
            with self.subTest(depth=depth):
                self.check(6, depth)

    def test_grey_colour_key_makes_alpha(self):
        rows = [[0, 1, 2, 3, 2, 1, 0]] * HT
        path = H.write_temp(H.png_bytes(W, HT, 0, 2, rows, trns=struct.pack(">H", 2)))
        self.addCleanup(os.remove, path)
        _, _, _, alpha = png.read(path)
        self.assertEqual(list(alpha[:W]), [255, 255, 0, 255, 0, 255, 255])

    def test_rgb_colour_key_makes_alpha(self):
        rows = [[1, 2, 3, 9, 9, 9] * 3 + [1, 2, 3]] * HT
        key = struct.pack(">3H", 1, 2, 3)
        path = H.write_temp(H.png_bytes(W, HT, 2, 8, [r[:W * 3] for r in rows], trns=key))
        self.addCleanup(os.remove, path)
        _, _, _, alpha = png.read(path)
        self.assertEqual(list(alpha[:3]), [0, 255, 0])

    def test_opaque_modes_have_no_alpha(self):
        rows = [[10, 20, 30] * W] * HT
        path = H.write_temp(H.png_bytes(W, HT, 2, 8, rows))
        self.addCleanup(os.remove, path)
        self.assertIsNone(png.read(path)[3])

    def test_ancillary_chunks_are_skipped(self):
        rows = [[5] * W] * HT
        path = H.write_temp(H.png_bytes(W, HT, 0, 8, rows, extra=[(b"tEXt", b"k\x00v"),
                                                                   (b"zzZz", b"xx")]))
        self.addCleanup(os.remove, path)
        self.assertEqual(png.read(path)[2], bytearray([5] * W * HT))


class Rejections(unittest.TestCase):
    def read_error(self, data, suffix=".png"):
        path = H.write_temp(data, suffix)
        self.addCleanup(os.remove, path)
        with self.assertRaises(DecalError) as ctx:
            png.read(path)
        self.assertIn(path, str(ctx.exception))
        return str(ctx.exception)

    def good(self):
        return H.png_bytes(W, HT, 0, 8, [[7] * W] * HT)

    def test_interlaced_is_rejected_by_name(self):
        msg = self.read_error(H.png_bytes(W, HT, 0, 8, [[7] * W] * HT, interlace=1))
        self.assertIn("interlaced", msg)
        self.assertIn("Adam7", msg)
        self.assertIn("Preview", msg)

    def test_not_a_png_names_the_header(self):
        msg = self.read_error(b"GIF89a-not-a-png")
        self.assertIn("not a PNG", msg)
        self.assertIn("47 49 46", msg)

    def test_unknown_critical_chunk(self):
        msg = self.read_error(H.png_bytes(W, HT, 0, 8, [[7] * W] * HT, extra=[(b"ABCD", b"x")]))
        self.assertIn("ABCD", msg)

    def test_crc_error(self):
        data = bytearray(self.good())
        data[20] ^= 0xFF  # inside the IHDR body
        self.assertIn("CRC", self.read_error(bytes(data)))

    def test_truncated(self):
        data = self.good()
        self.assertIn("truncated", self.read_error(data[:len(data) - 20]))

    def test_missing_iend(self):
        data = self.good()
        self.assertIn("IEND", self.read_error(data[:len(data) - 12]))

    def test_zlib_error(self):
        data = (b"\x89PNG\r\n\x1a\n"
                + H.chunk(b"IHDR", struct.pack(">IIBBBBB", W, HT, 8, 0, 0, 0, 0))
                + H.chunk(b"IDAT", b"this is not zlib data")
                + H.chunk(b"IEND", b""))
        self.assertIn("decompress", self.read_error(data))

    def test_short_image_data_is_truncated(self):
        data = (b"\x89PNG\r\n\x1a\n"
                + H.chunk(b"IHDR", struct.pack(">IIBBBBB", W, HT, 8, 0, 0, 0, 0))
                + H.chunk(b"IDAT", zlib.compress(b"\x00" * 10))
                + H.chunk(b"IEND", b""))
        self.assertIn("truncated", self.read_error(data))

    def test_invalid_mode(self):
        data = (b"\x89PNG\r\n\x1a\n"
                + H.chunk(b"IHDR", struct.pack(">IIBBBBB", W, HT, 16, 3, 0, 0, 0))
                + H.chunk(b"IEND", b""))
        self.assertIn("valid PNG mode", self.read_error(data))

    def test_missing_file_names_the_path(self):
        with self.assertRaises(DecalError) as ctx:
            png.read("/nonexistent/decal-test.png")
        self.assertIn("/nonexistent/decal-test.png", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
