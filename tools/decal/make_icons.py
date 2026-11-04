"""Draw the Decal command icons into tools/decal/Decal/resources/ with plain Python.

Run: python3 -I tools/decal/make_icons.py

Fusion reads 16x16.png and 16x16@2x.png for the small icon and 32x32.png and 32x32@2x.png for
the large one, and writes a question-mark placeholder for any it cannot find. The art is a
square plate seen from above, outlined in Fusion's dark grey, with a filled rounded square
inlaid in it. The line is one unit of the 16 or 32 grid, so two pixels in an @2x image.
"""

import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'Decal', 'resources')

OUTLINE_RGB = (0x4D, 0x4D, 0x4D)
INLAY_RGB = (0x99, 0x99, 0x99)
LINE = 1.0          # outline width in grid units
SUPER = 8           # samples per pixel along each side, for smooth edges

# Per grid: the plate outline's centre-line box and the inlay's filled box, each as
# (x0, y0, x1, y1, corner radius) in grid units. The small grid keeps fewer, larger details.

DESIGNS = {
    16: {'plate': (1.5, 1.5, 14.5, 14.5, 2.0), 'inlay': (5.0, 5.0, 11.0, 11.0, 1.5)},
    32: {'plate': (2.5, 2.5, 29.5, 29.5, 4.0), 'inlay': (10.0, 10.0, 22.0, 22.0, 3.0)},
}

FILES = (('16x16.png', 16, 1), ('16x16@2x.png', 16, 2), ('32x32.png', 32, 1), ('32x32@2x.png', 32, 2))


def box_distance(x: float, y: float, box: tuple) -> float:
    """Signed distance from (x, y) to a rounded box; negative inside."""
    x0, y0, x1, y1, r = box
    qx = abs(x - (x0 + x1) / 2) - ((x1 - x0) / 2 - r)
    qy = abs(y - (y0 + y1) / 2) - ((y1 - y0) / 2 - r)
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def render(grid: int, scale: int) -> bytes:
    """RGBA rows, each led by PNG filter byte 0. The outline and the inlay never overlap, so
    their sample counts add into one coverage."""
    design = DESIGNS[grid]
    size = grid * scale
    steps = [(k + 0.5) / SUPER for k in range(SUPER)]
    total = SUPER * SUPER
    out = bytearray()
    for py in range(size):
        out.append(0)
        for px in range(size):
            line = fill = 0
            for sy in steps:
                y = (py + sy) / scale
                for sx in steps:
                    x = (px + sx) / scale
                    if abs(box_distance(x, y, design['plate'])) <= LINE / 2:
                        line += 1
                    elif box_distance(x, y, design['inlay']) <= 0.0:
                        fill += 1
            covered = line + fill
            if covered == 0:
                out += b'\x00\x00\x00\x00'
                continue
            rgb = [round((OUTLINE_RGB[c] * line + INLAY_RGB[c] * fill) / covered) for c in range(3)]
            out += bytes(rgb + [round(255 * covered / total)])
    return bytes(out)


def png(width: int, height: int, rows: bytes) -> bytes:
    """An 8-bit RGBA PNG."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(rows, 9))
            + chunk(b'IEND', b''))


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for name, grid, scale in FILES:
        size = grid * scale
        path = os.path.join(OUT, name)
        with open(path, 'wb') as fh:
            fh.write(png(size, size, render(grid, scale)))
        print(f'{path}  {size} x {size}')


if __name__ == '__main__':
    main()
