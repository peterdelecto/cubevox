#!/usr/bin/env python3
"""Cuts a raw 1-bit OLED proof stream into one animated GIF per scene, plus contact sheets.

  python3 -I tools/oled-proof/motion_gif.py IN.raw OUT_DIR

IN.raw comes from oled_motion; the scene table sits beside it as IN.scenes (number, slug,
first frame, end frame, tab separated). Each scene becomes OUT_DIR/scene-<n>-<slug>.gif.
Contact sheets go to build/ at the repository root, 24 tiles per PNG. Needs Pillow.
"""

import os
import struct
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

MAGIC = b"CVOM"
VERSION = 1
# magic, version, width, height, capWidth, capHeight, fps, frameCount
HEADER = struct.Struct("<4s7I")

SCREEN_W, SCREEN_H = 128, 64
CAP_W, CAP_H = 128, 16
SCREEN_BYTES = SCREEN_W * SCREEN_H // 8
CAP_BYTES = CAP_W * CAP_H // 8
STRIDE = SCREEN_BYTES + CAP_BYTES

SCALE = 5
MARGIN = 16
CAP_GAP = 12
SCREEN_BOX = (SCREEN_W * SCALE, SCREEN_H * SCALE)
CAP_BOX = (CAP_W * SCALE, CAP_H * SCALE)
SCREEN_POS = (MARGIN, MARGIN)
CAP_POS = (MARGIN, MARGIN + SCREEN_BOX[1] + CAP_GAP)
IMG_SIZE = (2 * MARGIN + SCREEN_BOX[0], CAP_POS[1] + CAP_BOX[1] + MARGIN)

BG = (7, 8, 10)
LIT = (236, 240, 244)
CAP_COLOUR = (132, 136, 140)
BLACK = (0, 0, 0)
LABEL_COLOUR = (190, 194, 198)

SHEET_STEP = 12
SHEET_COLS = 4
SHEET_ROWS = 6
SHEET_TILE = (IMG_SIZE[0] // 2, IMG_SIZE[1] // 2)
GUTTER = 8
LABEL_FONT_SIZE = 14
LABEL_SAMPLE = "t=00.00s"
LABEL_PAD = 4

REPO_BUILD_DIR = Path(__file__).resolve().parents[2] / "build"

# Table b maps a page byte to 255 when bit b is set, else 0.
BIT_TABLES = [bytes(255 if (value >> bit) & 1 else 0 for value in range(256)) for bit in range(8)]


def fail(message, code=2):
    print(f"motion_gif: error: {message}", file=sys.stderr)
    sys.exit(code)


def box_at(pos, size):
    return (pos[0], pos[1], pos[0] + size[0], pos[1] + size[1])


def size_kb(path):
    return os.path.getsize(path) / 1000


def decode_bitmap(buf, width, height):
    """Expand an SH1106 page-layout bitmap to an 'L' image, 255 where lit.

    Byte p * width + x holds column x of page p. Bit b (0 = top) is row 8 * p + b.
    """
    rows = [b""] * height
    for page in range(height // 8):
        page_bytes = buf[page * width:(page + 1) * width]
        for bit in range(8):
            rows[page * 8 + bit] = page_bytes.translate(BIT_TABLES[bit])
    return Image.frombytes("L", (width, height), b"".join(rows))


def build_grid_mask():
    """Return 255 on each cell's 4 x 4 lit block and 0 on its gap row and column."""
    last = SCALE - 1
    lit_row = bytes(0 if x % SCALE == last else 255 for x in range(SCREEN_BOX[0]))
    gap_row = bytes(SCREEN_BOX[0])
    rows = [gap_row if y % SCALE == last else lit_row for y in range(SCREEN_BOX[1])]
    return Image.frombytes("L", SCREEN_BOX, b"".join(rows))


def build_palette():
    palette = Image.new("P", (1, 1))
    palette.putpalette([channel for colour in (BG, LIT, CAP_COLOUR, BLACK) for channel in colour])
    return palette


def render_frame(screen, caption, grid):
    frame = Image.new("RGB", IMG_SIZE, BG)
    lit = decode_bitmap(screen, SCREEN_W, SCREEN_H).resize(SCREEN_BOX, Image.Resampling.NEAREST)
    frame.paste(LIT, box_at(SCREEN_POS, SCREEN_BOX), ImageChops.multiply(lit, grid))
    text = decode_bitmap(caption, CAP_W, CAP_H).resize(CAP_BOX, Image.Resampling.NEAREST)
    frame.paste(CAP_COLOUR, box_at(CAP_POS, CAP_BOX), text)
    return frame


def read_stream(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        fail(f"{path} is shorter than the {HEADER.size}-byte header")
    magic, version, width, height, cap_w, cap_h, fps, count = HEADER.unpack_from(data)
    if magic != MAGIC:
        fail(f"bad magic {magic!r}, expected {MAGIC!r}")
    if version != VERSION:
        fail(f"unsupported version {version}, expected {VERSION}")
    if (width, height, cap_w, cap_h) != (SCREEN_W, SCREEN_H, CAP_W, CAP_H):
        fail(f"layout {width}x{height} with {cap_w}x{cap_h} caption, "
             f"expected {SCREEN_W}x{SCREEN_H} with {CAP_W}x{CAP_H}")
    if fps == 0:
        fail("fps is 0")
    if count == 0:
        fail("stream has no frames")
    needed = HEADER.size + count * STRIDE
    if len(data) < needed:
        fail(f"truncated: {count} frames need {needed} bytes, file has {len(data)}")
    return data, fps, count


def read_scenes(raw_path, count):
    path = Path(raw_path).with_suffix(".scenes")
    if not path.exists():
        fail(f"scene table {path} not found; oled_motion writes it beside the raw stream")
    scenes = []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        number, slug, start, end = line.split("\t")
        start, end = int(start), int(end)
        if not 0 <= start < end <= count:
            fail(f"scene {number} spans frames {start}..{end}, stream has {count}")
        scenes.append((number, slug, start, end))
    if not scenes:
        fail(f"scene table {path} is empty")
    return scenes


def record(data, index):
    base = HEADER.size + index * STRIDE
    return data[base:base + SCREEN_BYTES], data[base + SCREEN_BYTES:base + STRIDE]


def write_sheet(path, group, fps, font, label_h):
    """Lay out (frame index, half-size frame) pairs four across and save a PNG."""
    rows = (len(group) + SHEET_COLS - 1) // SHEET_COLS
    pitch_x = SHEET_TILE[0] + GUTTER
    pitch_y = label_h + SHEET_TILE[1] + GUTTER
    sheet = Image.new("RGB", (GUTTER + SHEET_COLS * pitch_x, GUTTER + rows * pitch_y), BG)
    draw = ImageDraw.Draw(sheet)
    for n, (index, tile) in enumerate(group):
        x = GUTTER + (n % SHEET_COLS) * pitch_x
        y = GUTTER + (n // SHEET_COLS) * pitch_y
        draw.text((x, y), f"t={index / fps:.2f}s", font=font, fill=LABEL_COLOUR)
        sheet.paste(tile, (x, y + label_h))
    sheet.save(path)


def gif_seconds(path):
    with Image.open(path) as gif:
        total = 0
        for i in range(gif.n_frames):
            gif.seek(i)
            total += gif.info.get("duration", 0)
        return gif.n_frames, total / 1000


def main(argv):
    if len(argv) != 3:
        fail("usage: python3 -I motion_gif.py IN.raw OUT_DIR")
    out_dir = Path(argv[2]).resolve()
    if not out_dir.is_dir():
        fail(f"output folder {out_dir} does not exist")
    data, fps, count = read_stream(argv[1])
    scenes = read_scenes(argv[1], count)

    grid = build_grid_mask()
    palette = build_palette()
    font = ImageFont.load_default(size=LABEL_FONT_SIZE)
    label_h = font.getbbox(LABEL_SAMPLE)[3] + LABEL_PAD
    REPO_BUILD_DIR.mkdir(exist_ok=True)

    sheet_tiles = []
    written = []
    for number, slug, start, end in scenes:
        frames = []
        for index in range(start, end):
            screen, caption = record(data, index)
            frame = render_frame(screen, caption, grid)
            frames.append(frame.quantize(palette=palette, dither=Image.Dither.NONE))
            if index % SHEET_STEP == 0:
                sheet_tiles.append((index, frame.resize(SHEET_TILE, Image.Resampling.NEAREST)))
        path = out_dir / f"scene-{number}-{slug}.gif"
        frames[0].save(str(path), save_all=True, append_images=frames[1:],
                       duration=round(1000 / fps), loop=0, disposal=1, optimize=False)
        written.append((path, end - start))

    capacity = SHEET_COLS * SHEET_ROWS
    sheets = []
    for first in range(0, len(sheet_tiles), capacity):
        group = sheet_tiles[first:first + capacity]
        path = REPO_BUILD_DIR / f"{Path(argv[1]).stem}-sheet-{len(sheets) + 1}.png"
        write_sheet(path, group, fps, font, label_h)
        sheets.append((path, [index for index, _ in group]))

    for path, rendered in written:
        stored, seconds = gif_seconds(path)
        print(f"{path.name}  {seconds:.2f} s  {rendered} frames rendered, {stored} stored  {size_kb(path):.0f} kB")
    print(f"sheets: {len(sheets)} in {REPO_BUILD_DIR}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except OSError as err:
        fail(f"{err.filename}: {err.strerror}" if err.filename else str(err), 1)
