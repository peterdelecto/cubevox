#!/usr/bin/env python3
"""Composes the menu font proof sheet from menu_font_proof's PGMs and manifests.

  python3 -I tools/oled-proof/menu_font_sheet.py build/menu-font-proof tools/oled-proof/menu-font-proof.png

Three candidates, three frames each at 4x with a 1 px pixel grid, then frame 1 of each candidate at
1x, the panel's true size.
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

TITLE = "Menu font proof, 2026-10-30"

BG = (22, 22, 26)
PANEL = (8, 8, 10)
GRID = (30, 30, 38)
LIT = (236, 236, 240)
LIT_GRID = (150, 150, 160)
TEXT = (200, 200, 208)
DIM = (130, 130, 142)
RULE = (64, 64, 76)

SCALE = 4
FRAME_W = 128 * SCALE
FRAME_H = 64 * SCALE
MARGIN = 32
FRAME_GAP = 24
FRAMES_PER_CANDIDATE = 3
SHEET_W = 2 * MARGIN + FRAMES_PER_CANDIDATE * FRAME_W + (FRAMES_PER_CANDIDATE - 1) * FRAME_GAP

F_TITLE = ImageFont.load_default(size=24)
F_NAME = ImageFont.load_default(size=20)
F_BODY = ImageFont.load_default(size=14)
F_SMALL = ImageFont.load_default(size=13)


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields = []
    pos = 2
    while len(fields) < 3:
        while data[pos : pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
        pos += 1
    w, h, _ = fields
    return Image.frombytes("L", (w, h), data[pos : pos + w * h])


def gridded(gray):
    """The 1-bit image at SCALE with a 1 px grid line after every source pixel."""
    w, h = gray.size
    img = Image.new("RGB", (w * SCALE, h * SCALE), PANEL)
    out = img.load()
    src = gray.load()
    for y in range(h * SCALE):
        row_gutter = y % SCALE == SCALE - 1
        for x in range(w * SCALE):
            on = src[x // SCALE, y // SCALE] > 127
            if row_gutter or x % SCALE == SCALE - 1:
                out[x, y] = LIT_GRID if on else GRID
            else:
                out[x, y] = LIT if on else PANEL
    return img


def plain(gray):
    """The 1-bit image at 1x in the sheet's lit and panel colours."""
    img = Image.new("RGB", gray.size, PANEL)
    img.paste(Image.new("RGB", gray.size, LIT), mask=gray.point(lambda v: 255 if v > 127 else 0))
    return img


def read_lines(path):
    with open(path, "r", encoding="utf-8") as f:
        return [line.rstrip("\n") for line in f if line.strip()]


def main():
    in_dir, out_path = sys.argv[1], sys.argv[2]
    candidates = [line.split("|") for line in read_lines(os.path.join(in_dir, "menu-candidates.txt"))]
    frames = [line.split("|") for line in read_lines(os.path.join(in_dir, "menu-frames.txt"))]
    notes = read_lines(os.path.join(in_dir, "menu-notes.txt"))

    head_h = 40 + 22 * (1 + len(notes)) + 16
    cand_h = 28 + 24 + FRAME_H + 8 + 40 + 36
    strip_h = 36 + 24 + 64 + 28 + 24
    total_h = MARGIN + head_h + len(candidates) * cand_h + strip_h + MARGIN

    sheet = Image.new("RGB", (SHEET_W, total_h), BG)
    draw = ImageDraw.Draw(sheet)

    y = MARGIN
    draw.text((MARGIN, y), TITLE, font=F_TITLE, fill=LIT)
    y += 40
    draw.text(
        (MARGIN, y),
        "The menu list in Myrtle 7X12 (font7x12, 2 px ink gap), drawn by the firmware's Canvas and fonts. "
        "Each frame is 128 x 64 px at 4x with a 1 px pixel grid.",
        font=F_SMALL,
        fill=DIM,
    )
    y += 22
    for line in notes:
        draw.text((MARGIN, y), line, font=F_SMALL, fill=DIM)
        y += 22
    y += 16

    for cid, key in candidates:
        draw.text((MARGIN, y), cid, font=F_NAME, fill=LIT)
        draw.text((MARGIN, y + 28), key, font=F_BODY, fill=TEXT)
        top = y + 28 + 24
        for n in range(1, FRAMES_PER_CANDIDATE + 1):
            _, _, caption, detail = next(f for f in frames if f[0] == cid and int(f[1]) == n)
            x0 = MARGIN + (n - 1) * (FRAME_W + FRAME_GAP)
            sheet.paste(gridded(read_pgm(os.path.join(in_dir, f"menu-{cid}-{n}.pgm"))), (x0, top))
            draw.rectangle([x0 - 1, top - 1, x0 + FRAME_W, top + FRAME_H], outline=RULE)
            draw.text((x0, top + FRAME_H + 8), caption, font=F_BODY, fill=TEXT)
            draw.text((x0, top + FRAME_H + 8 + 20), detail, font=F_SMALL, fill=DIM)
        y += cand_h

    draw.text((MARGIN, y), "Frame 1 of each candidate at 1x, the panel's true size (128 x 64 px)", font=F_BODY, fill=TEXT)
    y += 36
    strip_gap = 64
    for i, (cid, _) in enumerate(candidates):
        x0 = MARGIN + i * (128 + strip_gap)
        draw.text((x0, y), cid, font=F_NAME, fill=LIT)
        sheet.paste(plain(read_pgm(os.path.join(in_dir, f"menu-{cid}-1.pgm"))), (x0, y + 28))
        draw.rectangle([x0 - 1, y + 27, x0 + 128, y + 28 + 64], outline=RULE)

    sheet.save(out_path)
    print(f"wrote {out_path} {sheet.width} x {sheet.height}")


if __name__ == "__main__":
    main()
