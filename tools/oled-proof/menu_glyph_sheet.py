#!/usr/bin/env python3
"""Composes the menu glyph proof sheet from menu_glyph_proof's PGMs and the font library.

  python3 -I tools/oled-proof/menu_glyph_sheet.py build/menu-glyph-proof tools/oled-proof/menu-glyph-proof.png

The sheet holds, top to bottom:
  1. every inked glyph of Myrtle 7X12 on a 4x grid, the new `>`, `k` and `z` in amber, with guides at
     the cap top, the x-height, the cap centre and the baseline
  2. the new glyphs at 4x beside the font's own relatives (K b d, Z s t, ( ) /)
  3. the set at 1x and 2x, drawn by the firmware's Canvas with its 2 px ink gap
  4. the strings `120 Hz`, `12 kHz` and `UPDATE FIRMWARE >` at 1x, 2x and 4x

The new glyphs are amber everywhere and carry a bar under them in the strings and the set.
"""

import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

LIBRARY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "font-editor", "fonts", "cubevox-fonts.json")
FONT_NAME = "Myrtle 7X12"
NEW_CHARS = ">kz"

BG = (22, 22, 26)
PANEL = (8, 8, 10)
CELL = (34, 34, 40)
LIT = (236, 236, 240)
NEW = (255, 214, 102)
TEXT = (200, 200, 208)
DIM = (130, 130, 142)
RULE = (64, 64, 76)
GUIDE = (74, 120, 170)

SHEET_W = 1180
MARGIN = 24
GRID_LEFT = 76
GRID_SCALE = 4
GRID_GAP = 8
PER_ROW = 28
SECTION_H = 34

FONT = ImageFont.load_default(size=14)
FONT_SMALL = ImageFont.load_default(size=12)
FONT_HEAD = ImageFont.load_default(size=16)


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


def tinted(full, only, scale):
    """The line as lit pixels on the panel colour, the new glyphs' pixels in amber, scaled by whole pixels."""
    img = Image.new("RGB", full.size, PANEL)
    img.paste(Image.new("RGB", full.size, LIT), mask=full.point(lambda v: 255 if v > 127 else 0))
    img.paste(Image.new("RGB", full.size, NEW), mask=only.point(lambda v: 255 if v > 127 else 0))
    return img.resize((full.width * scale, full.height * scale), Image.NEAREST)


def load_font():
    with open(LIBRARY, "r", encoding="utf-8") as f:
        lib = json.load(f)
    return next(x for x in lib["fonts"] if x["name"] == FONT_NAME)


def glyph_rows(font, ch):
    return font["glyphs"][str(ord(ch))]


def has_ink(rows):
    return any("#" in line for line in rows)


def draw_glyph(draw, x0, y0, rows, scale, color=LIT):
    w = len(rows[0]) * scale
    h = len(rows) * scale
    draw.rectangle([x0, y0, x0 + w - 1, y0 + h - 1], fill=CELL)
    for ry, line in enumerate(rows):
        for rx, c in enumerate(line):
            if c == "#":
                px = x0 + rx * scale
                py = y0 + ry * scale
                draw.rectangle([px, py, px + scale - 1, py + scale - 1], fill=color)


def centred_text(draw, cx, y, text, font, fill):
    tw = draw.textlength(text, font=font)
    draw.text((cx - tw / 2, y), text, font=font, fill=fill)


def framed(sheet, draw, x, y, img):
    sheet.paste(img, (x, y))
    draw.rectangle([x - 1, y - 1, x + img.width, y + img.height], outline=RULE)


def read_manifest(out_dir):
    rows = []
    with open(os.path.join(out_dir, "glyph-lines.txt"), "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line:
                rows.append(line.split("|", 4))
    return rows


def spans_of(text):
    return [tuple(int(v) for v in s.split("-")) for s in text.split(",") if s]


def grid(draw, y, font):
    """Section 1. Returns the y below it and the cells without ink."""
    scale = GRID_SCALE
    cw = font["width"] * scale
    ch = font["height"] * scale
    pitch = cw + GRID_GAP
    chars = [chr(c) for c in range(33, 127) if has_ink(glyph_rows(font, chr(c)))]
    blanks = [chr(c) for c in range(33, 127) if not has_ink(glyph_rows(font, chr(c)))]
    marks = [("cap top", 0), ("x-height", 5), ("cap centre", 6), ("base", font["height"])]
    for start in range(0, len(chars), PER_ROW):
        row = chars[start : start + PER_ROW]
        for i, c in enumerate(row):
            x0 = GRID_LEFT + i * pitch
            is_new = c in NEW_CHARS
            draw_glyph(draw, x0, y, glyph_rows(font, c), scale, NEW if is_new else LIT)
            centred_text(draw, x0 + cw / 2, y + ch + 2, c, FONT, NEW if is_new else TEXT)
        row_w = len(row) * pitch - GRID_GAP
        label_y = -100
        for name, r in marks:
            gy = y + r * scale
            for gx in range(GRID_LEFT, GRID_LEFT + row_w, 6):
                draw.line([gx, gy, gx + 2, gy], fill=GUIDE)
            label_y = max(label_y + 12, gy - 7)
            draw.text((MARGIN - 12, label_y), f"{name}", font=FONT_SMALL, fill=GUIDE)
        y += ch + 40
    return y, blanks


def relatives(draw, y, font):
    """Section 2. The new glyphs beside their relatives at 4x."""
    scale = GRID_SCALE
    cw = font["width"] * scale
    pitch = cw + GRID_GAP + 10
    groups = [("K b d", "k"), ("Z s t", "z"), ("( ) /", ">")]
    x = MARGIN
    ch = font["height"] * scale
    for rel, new in groups:
        chars = rel.split() + [new]
        for i, c in enumerate(chars):
            x0 = x + i * pitch
            is_new = c == new
            draw_glyph(draw, x0, y, glyph_rows(font, c), scale, NEW if is_new else LIT)
            centred_text(draw, x0 + cw / 2, y + ch + 2, c, FONT, NEW if is_new else TEXT)
        x += len(chars) * pitch + 40
    return y + ch + 28


def line_image(out_dir, row, scale):
    kind, ident, _, spans, _ = row
    full = read_pgm(os.path.join(out_dir, f"glyph-line-{ident}.pgm"))
    only = read_pgm(os.path.join(out_dir, f"glyph-line-{ident}-new.pgm"))
    return tinted(full, only, scale), spans_of(spans)


def bars(draw, x, y, img, spans, scale):
    """Amber bar under each new glyph's ink, 3 px below the line."""
    for x0, x1 in spans:
        draw.rectangle([x + x0 * scale, y + img.height + 3, x + (x1 + 1) * scale - 1, y + img.height + 4], fill=NEW)


def set_rows(sheet, draw, y, out_dir, rows):
    """Section 3. Each set line at 1x and 2x side by side."""
    x1 = MARGIN
    x2 = MARGIN + 128 + 40
    draw.text((x1, y), "1x", font=FONT_SMALL, fill=DIM)
    draw.text((x2, y), "2x", font=FONT_SMALL, fill=DIM)
    y += 20
    for row in rows:
        img1, spans = line_image(out_dir, row, 1)
        img2, _ = line_image(out_dir, row, 2)
        framed(sheet, draw, x1, y, img1)
        bars(draw, x1, y, img1, spans, 1)
        framed(sheet, draw, x2, y, img2)
        bars(draw, x2, y, img2, spans, 2)
        y += max(img1.height, img2.height) + 18
    return y


def string_rows(sheet, draw, y, out_dir, rows):
    """Section 4. Each string at 1x, 2x and 4x."""
    scales = [1, 2, 4]
    xs = [MARGIN + 110, MARGIN + 110 + 128 + 40, MARGIN + 110 + 128 + 40 + 256 + 40]
    for s, x in zip(scales, xs):
        draw.text((x, y), f"{s}x", font=FONT_SMALL, fill=DIM)
    y += 20
    for row in rows:
        text = row[4]
        imgs = [line_image(out_dir, row, s) for s in scales]
        row_h = max(img.height for img, _ in imgs)
        draw.text((MARGIN, y + row_h // 2 - 8), text, font=FONT, fill=TEXT)
        for (img, spans), s, x in zip(imgs, scales, xs):
            framed(sheet, draw, x, y, img)
            bars(draw, x, y, img, spans, s)
        y += row_h + 22
    return y


def build(out_dir):
    font = load_font()
    manifest = read_manifest(out_dir)
    set_lines = [r for r in manifest if r[0] == "set"]
    strings = [r for r in manifest if r[0] == "str"]
    sheet = Image.new("RGB", (SHEET_W, 3400), BG)
    draw = ImageDraw.Draw(sheet)

    y = MARGIN
    draw.text((MARGIN, y), "Menu glyph proof, Myrtle 7X12 with the draft > k and z", font=FONT_HEAD, fill=LIT)
    draw.text(
        (MARGIN, y + 24),
        "Glyph rows come from the font library. The set and the strings are drawn by the firmware's Canvas and fonts "
        "(2 px ink gap). The three new glyphs are amber. They are drafts for the owner to refine.",
        font=FONT_SMALL,
        fill=DIM,
    )
    y += 60

    draw.text((MARGIN, y), "1. Every inked glyph at 4x, guides at the cap top, x-height (row 5), cap centre (rows 5 and 6) and base", font=FONT, fill=LIT)
    y += SECTION_H
    y, blanks = grid(draw, y, font)
    draw.text(
        (MARGIN, y),
        "Cells without ink, which the box draws as blank or, for a lowercase letter, as its capital: " + " ".join(blanks),
        font=FONT_SMALL,
        fill=DIM,
    )
    y += 34

    draw.text((MARGIN, y), "2. The new glyphs beside the font's own K b d, Z s t and ( ) /, at 4x", font=FONT, fill=LIT)
    y += SECTION_H
    y = relatives(draw, y, font)

    y += 16
    draw.text((MARGIN, y), "3. The set at 1x and 2x, one canvas line per row, a bar under each new glyph", font=FONT, fill=LIT)
    y += SECTION_H
    y = set_rows(sheet, draw, y, out_dir, set_lines)

    y += 16
    draw.text((MARGIN, y), "4. The menu's value strings at 1x, 2x and 4x, a bar under each new glyph", font=FONT, fill=LIT)
    y += SECTION_H
    y = string_rows(sheet, draw, y, out_dir, strings)
    return sheet.crop((0, 0, SHEET_W, y + MARGIN))


def main():
    out_dir, out_path = sys.argv[1], sys.argv[2]
    sheet = build(out_dir)
    sheet.save(out_path)
    print(f"wrote {out_path} {sheet.width} x {sheet.height}")


if __name__ == "__main__":
    main()
