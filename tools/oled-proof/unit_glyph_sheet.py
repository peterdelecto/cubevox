#!/usr/bin/env python3
"""Composes the unit glyph proof sheet from unit_glyph_proof's PGMs and the font library.

  python3 -I tools/oled-proof/unit_glyph_sheet.py build tools/oled-proof/unit-glyph-proof.png

The sheet holds, top to bottom:
  1. per font (Myrtle 7X12, 9X12, 16X24), a row at 4x of the new s, t and d, then m, b and the digits
  2. per letter (s, t, d), the 5x7, 7X12, 9X12 and 16X24 glyphs side by side at 4x, and 16X24 at 2x
  3. the six unit strings in Myrtle 16X24 at 1x, 2x and 4x
  4. the same strings in Myrtle 9X12 and Myrtle 7X12 at 2x and 4x
  5. four readout screens at 3x as the box will draw them with lowercase units
"""

import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

LIBRARY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "font-editor", "fonts", "cubevox-fonts.json")

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
GRID_SCALE = 4
GRID_GAP = 8
LABEL_H = 20
SECTION_H = 34
SCREEN_SCALE = 3
SCREEN_GAP = 24
CAPTION_H = 40

FONT = ImageFont.load_default(size=14)
FONT_SMALL = ImageFont.load_default(size=12)
FONT_HEAD = ImageFont.load_default(size=16)

NEW_CHARS = "std"
ROW_CHARS = "stdmb0123456789"
STRING_LABELS = ["-12 st", "250 ms", "-40 dB", "50 %", "C / Am", "Bb / Gm"]


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P5, then width and height, then maxval, each separated by one whitespace byte.
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


def tinted(gray, scale):
    """A 1-bit gray image as lit and panel colours, scaled by whole pixels."""
    img = Image.new("RGB", gray.size, PANEL)
    img.paste(Image.new("RGB", gray.size, LIT), mask=gray.point(lambda v: 255 if v > 127 else 0))
    return img.resize((gray.width * scale, gray.height * scale), Image.NEAREST)


def load_library():
    with open(LIBRARY, "r", encoding="utf-8") as f:
        lib = json.load(f)
    return {x["name"]: x for x in lib["fonts"]}


def glyph_rows(font, ch):
    return font["glyphs"][str(ord(ch))]


def first_ink_row(rows):
    for y, line in enumerate(rows):
        if "#" in line:
            return y
    return None


def draw_glyph(draw, x0, y0, rows, scale, color=LIT, cell=True):
    """Draws one cell at (x0, y0) with scale px per pixel."""
    w = len(rows[0]) * scale
    h = len(rows) * scale
    if cell:
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


def metric_rows(font):
    """Cell rows the proof marks. The m sets the x-height, the b the ascender, the t its top."""
    return {
        "asc": first_ink_row(glyph_rows(font, "b")),
        "t top": first_ink_row(glyph_rows(font, "t")),
        "x": first_ink_row(glyph_rows(font, "m")),
        "base": font["height"],
    }


def font_row(draw, y, font, title):
    """Section 1 row. Returns the y below it."""
    scale = GRID_SCALE
    cw = font["width"] * scale
    ch = font["height"] * scale
    pitch = cw + GRID_GAP
    draw.text((MARGIN, y), title, font=FONT, fill=TEXT)
    y += 24
    marks = metric_rows(font)
    row_w = len(ROW_CHARS) * pitch - GRID_GAP
    for i, c in enumerate(ROW_CHARS):
        x0 = MARGIN + i * pitch
        color = NEW if c in NEW_CHARS else LIT
        draw_glyph(draw, x0, y, glyph_rows(font, c), scale, color)
        label = c + (" flat" if c == "b" else "")
        centred_text(draw, x0 + cw / 2, y + ch + 2, label, FONT, NEW if c in NEW_CHARS else TEXT)
    # Guides sit on the row edges the marks name, with the name at the right.
    label_y = -100
    for name, row in marks.items():
        gy = y + row * scale
        for gx in range(MARGIN, MARGIN + row_w, 6):
            draw.line([gx, gy, gx + 2, gy], fill=GUIDE)
        label_y = max(label_y + 12, gy - 7)
        draw.text((MARGIN + row_w + 8, label_y), f"{name} {row}", font=FONT_SMALL, fill=GUIDE)
    return y + ch + LABEL_H + 18


def comparison_rows(draw, y, fonts):
    """Section 2: one row per new letter, one cell per size, baselines on one line."""
    f5, f7, f9, f16 = fonts["Myrtle 5x7"], fonts["Myrtle 7X12"], fonts["Myrtle 9X12"], fonts["Myrtle 16X24"]
    cells = [
        ("5x7", f5, 4, True),
        ("7X12", f7, 4, False),
        ("9X12", f9, 4, False),
        ("16X24", f16, 4, False),
        ("16X24 at 2x", f16, 2, False),
    ]
    column_w = 170
    row_h = f16["height"] * 4 + 44
    for c in NEW_CHARS:
        base = y + f16["height"] * 4
        for k, (name, font, scale, is_5x7) in enumerate(cells):
            ch = c.upper() if is_5x7 else c
            rows = glyph_rows(font, ch)
            w = font["width"] * scale
            mid = MARGIN + k * column_w + column_w // 2
            draw_glyph(draw, mid - w // 2, base - font["height"] * scale, rows, scale, DIM if is_5x7 else NEW)
            centred_text(draw, mid, base + 4, f"{name} {ch}", FONT_SMALL, TEXT)
            if is_5x7:
                centred_text(draw, mid, base + 18, "no lowercase", FONT_SMALL, DIM)
        y += row_h
    return y


def framed(sheet, draw, x, y, img):
    sheet.paste(img, (x, y))
    draw.rectangle([x - 1, y - 1, x + img.width, y + img.height], outline=RULE)


def string_table(sheet, draw, y, out_dir, face, scales, heads):
    """Sections 3 and 4. One row per string; one framed image per (font, scale) column."""
    imgs = [read_pgm(os.path.join(out_dir, f"unit-line-{face_tag}-{i}.pgm"))
            for i in range(len(STRING_LABELS)) for face_tag in face]
    per = len(face)
    label_w = 110
    # Column x positions come from the widest image of each column.
    col_w = []
    for fi in range(per):
        widest = max(imgs[i * per + fi].width for i in range(len(STRING_LABELS)))
        for s in scales:
            col_w.append(widest * s)
    x = MARGIN + label_w
    col_x = []
    for w in col_w:
        col_x.append(x)
        x += w + 28
    # The columns are ordered font by font, then scale.
    col_specs = [(fi, s) for fi in range(per) for s in scales]
    for (fi, s), cx, head in zip(col_specs, col_x, heads):
        draw.text((cx, y), head, font=FONT_SMALL, fill=DIM)
    y += 20
    for i, label in enumerate(STRING_LABELS):
        row_h = max(imgs[i * per + fi].height * s for fi, s in col_specs)
        draw.text((MARGIN, y + row_h // 2 - 8), label, font=FONT, fill=TEXT)
        for (fi, s), cx in zip(col_specs, col_x):
            framed(sheet, draw, cx, y, tinted(imgs[i * per + fi], s))
        y += row_h + 14
    return y


def screens(sheet, draw, y, out_dir):
    with open(os.path.join(out_dir, "unit-screens.txt"), "r", encoding="utf-8") as f:
        rows = [line.rstrip("\n").split("|") for line in f if line.strip()]
    images = [read_pgm(os.path.join(out_dir, f"unit-screen-{r[0]}.pgm")) for r in rows]
    sw = images[0].width * SCREEN_SCALE
    sh = images[0].height * SCREEN_SCALE
    cols = 2
    total_w = cols * sw + (cols - 1) * SCREEN_GAP
    x_start = (SHEET_W - total_w) // 2
    for i, (r, img) in enumerate(zip(rows, images)):
        col = i % cols
        row = i // cols
        x0 = x_start + col * (sw + SCREEN_GAP)
        y0 = y + row * (CAPTION_H + sh + SCREEN_GAP)
        _, title, param, value, left, right = r
        draw.text((x0, y0 + 2), f"{title} / {param} / {value}", font=FONT, fill=LIT)
        draw.text((x0, y0 + 20), f"value ink {left} px from the left edge, {right} px from the right", font=FONT_SMALL, fill=DIM)
        framed(sheet, draw, x0, y0 + CAPTION_H, tinted(img, SCREEN_SCALE))
    n_rows = (len(rows) + cols - 1) // cols
    return y + n_rows * (CAPTION_H + sh + SCREEN_GAP)


def build(out_dir):
    fonts = load_library()
    sheet = Image.new("RGB", (SHEET_W, 7000), BG)
    draw = ImageDraw.Draw(sheet)

    y = MARGIN
    draw.text((MARGIN, y), "Unit glyph proof, lowercase s, t and d in the Myrtle fonts", font=FONT_HEAD, fill=LIT)
    draw.text(
        (MARGIN, y + 24),
        "Glyph rows come from the font library. Strings and screens are drawn by the firmware's Canvas and fonts, "
        "with the units in the case the library gives them.",
        font=FONT_SMALL,
        fill=DIM,
    )
    y += 60

    draw.text((MARGIN, y), "1. New s, t, d with m, b and the digits at 4x (new glyphs in amber, guides at the metric rows)", font=FONT, fill=LIT)
    y += SECTION_H
    for name in ("Myrtle 7X12", "Myrtle 9X12", "Myrtle 16X24"):
        y = font_row(draw, y, fonts[name], name)

    y += 10
    draw.text((MARGIN, y), "2. One letter across the sizes at 4x. Myrtle 5x7 has no lowercase, so the box draws its capital.", font=FONT, fill=LIT)
    y += SECTION_H
    y = comparison_rows(draw, y, fonts)

    y += 10
    draw.text((MARGIN, y), "3. Unit strings in Myrtle 16X24 at 1x, 2x and 4x", font=FONT, fill=LIT)
    y += SECTION_H
    y = string_table(sheet, draw, y, out_dir, ["16"], [1, 2, 4], ["1x", "2x", "4x"])

    y += 10
    draw.text(
        (MARGIN, y),
        "4. The same strings in Myrtle 9X12 and Myrtle 7X12 at 2x and 4x (neither font carries %, so its gap is blank)",
        font=FONT,
        fill=LIT,
    )
    y += SECTION_H
    y = string_table(sheet, draw, y, out_dir, ["9", "7"], [2, 4], ["9X12 at 2x", "9X12 at 4x", "7X12 at 2x", "7X12 at 4x"])

    y += 10
    draw.text(
        (MARGIN, y),
        "5. Readout screens at 3x. Title bar rows 0 to 13 in 7X12, parameter in 9X12 at row 16, value in 16X24 at row 34.",
        font=FONT,
        fill=LIT,
    )
    y += SECTION_H
    y = screens(sheet, draw, y, out_dir)
    return sheet.crop((0, 0, SHEET_W, y + MARGIN))


def main():
    out_dir, out_path = sys.argv[1], sys.argv[2]
    sheet = build(out_dir)
    sheet.save(out_path)
    print(f"wrote {out_path} {sheet.width} x {sheet.height}")


if __name__ == "__main__":
    main()
