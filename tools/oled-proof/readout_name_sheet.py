#!/usr/bin/env python3
"""Composes a readout proof sheet from readout_name_proof's PGMs.

  python3 -I tools/oled-proof/readout_name_sheet.py build tools/oled-proof/readout-name-proof.png name
  python3 -I tools/oled-proof/readout_name_sheet.py build tools/oled-proof/readout-title-proof.png title

The name sheet holds the Myrtle 7X12 glyph grid at 4x, nine specimen lines at 1x and 2x, and the
eight name-row candidates in 2 columns at 3x. The title sheet holds the eight inverted-bar
candidates only. Each screen sits under a caption.
"""

import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_NAME = "Myrtle 7X12"
LIBRARY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "font-editor", "fonts", "cubevox-fonts.json")

BG = (22, 22, 26)
PANEL = (8, 8, 10)
CELL = (34, 34, 40)
LIT = (236, 236, 240)
TEXT = (200, 200, 208)
DIM = (130, 130, 142)
RULE = (64, 64, 76)

SHEET_W = 960
MARGIN = 24
GRID_SCALE = 4
GRID_PER_ROW = 25
GRID_GAP = 8
SCREEN_SCALE = 3
SCREEN_COLS = 2
SCREEN_GAP = 24
CAPTION_H = 36

FONT = ImageFont.load_default(size=14)
FONT_SMALL = ImageFont.load_default(size=12)
FONT_HEAD = ImageFont.load_default(size=16)


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


def load_glyphs():
    with open(LIBRARY, "r", encoding="utf-8") as f:
        lib = json.load(f)
    font = next(x for x in lib["fonts"] if x["name"] == FONT_NAME)
    glyphs = []
    for key in sorted(font["glyphs"], key=int):
        rows = font["glyphs"][key]
        if any("#" in r for r in rows):
            glyphs.append((chr(int(key)), rows))
    return font["width"], font["height"], glyphs


def glyph_grid(draw, y, width, height, glyphs):
    """Draws the grid at (MARGIN, y); returns the y below it."""
    cell_w = width * GRID_SCALE
    cell_h = height * GRID_SCALE
    label_h = 20
    pitch = cell_w + GRID_GAP
    for i, (ch, rows) in enumerate(glyphs):
        col = i % GRID_PER_ROW
        row = i // GRID_PER_ROW
        x0 = MARGIN + col * pitch
        y0 = y + row * (cell_h + label_h + GRID_GAP)
        draw.rectangle([x0, y0, x0 + cell_w - 1, y0 + cell_h - 1], fill=CELL)
        for ry, line in enumerate(rows):
            for rx, c in enumerate(line):
                if c == "#":
                    px = x0 + rx * GRID_SCALE
                    py = y0 + ry * GRID_SCALE
                    draw.rectangle([px, py, px + GRID_SCALE - 1, py + GRID_SCALE - 1], fill=LIT)
        label = ch
        tw = draw.textlength(label, font=FONT)
        draw.text((x0 + (cell_w - tw) / 2, y0 + cell_h + 2), label, font=FONT, fill=TEXT)
    rows_used = (len(glyphs) + GRID_PER_ROW - 1) // GRID_PER_ROW
    return y + rows_used * (cell_h + label_h + GRID_GAP)


def read_manifest(out_dir, set_name):
    with open(os.path.join(out_dir, f"readout-{set_name}-screens.txt"), "r", encoding="utf-8") as f:
        screens = [line.rstrip("\n").split("|") for line in f if line.strip()]
    images = [read_pgm(os.path.join(out_dir, f"readout-{set_name}-{s[0]}.pgm")) for s in screens]
    return screens, images


def detail_text(name_w, status, bar_w):
    if int(bar_w) > 0:
        return f"bar {bar_w} px, name {name_w} px, {status}"
    return f"name row {name_w} px, {status}"


def screen_grid(sheet, draw, y, screens, images):
    """Pastes the screens in SCREEN_COLS columns at 3x, each under its caption; returns the y below."""
    sw = images[0].width * SCREEN_SCALE
    sh = images[0].height * SCREEN_SCALE
    total_w = SCREEN_COLS * sw + (SCREEN_COLS - 1) * SCREEN_GAP
    x_start = (SHEET_W - total_w) // 2
    for i, (s, img) in enumerate(zip(screens, images)):
        col = i % SCREEN_COLS
        row = i // SCREEN_COLS
        x0 = x_start + col * (sw + SCREEN_GAP)
        y0 = y + row * (CAPTION_H + sh + SCREEN_GAP)
        _, caption, name_w, status, bar_w = s
        draw.text((x0, y0 + 2), caption, font=FONT, fill=LIT)
        draw.text((x0, y0 + 2 + 16), detail_text(name_w, status, bar_w), font=FONT_SMALL, fill=DIM)
        sheet.paste(tinted(img, SCREEN_SCALE), (x0, y0 + CAPTION_H + 6))
        draw.rectangle([x0 - 1, y0 + CAPTION_H + 5, x0 + sw, y0 + CAPTION_H + 6 + sh], outline=RULE)
    rows = (len(screens) + SCREEN_COLS - 1) // SCREEN_COLS
    return y + rows * (CAPTION_H + sh + SCREEN_GAP)


def screens_height(images, count):
    sh = images[0].height * SCREEN_SCALE
    rows = (count + SCREEN_COLS - 1) // SCREEN_COLS
    return rows * (CAPTION_H + sh + SCREEN_GAP)


def name_sheet(out_dir):
    width, height, glyphs = load_glyphs()
    screens, screen_imgs = read_manifest(out_dir, "name")
    with open(os.path.join(out_dir, "readout-name-lines.txt"), "r", encoding="utf-8") as f:
        lines = [line.rstrip("\n") for line in f]
    line_imgs = [read_pgm(os.path.join(out_dir, f"readout-name-line{i}.pgm")) for i in range(len(lines))]

    grid_rows = (len(glyphs) + GRID_PER_ROW - 1) // GRID_PER_ROW
    grid_h = grid_rows * (height * GRID_SCALE + 20 + GRID_GAP)
    line_h = max(img.height for img in line_imgs) * 2 + 8
    head_h = 56
    section_h = 34
    total_h = (MARGIN + head_h + section_h + grid_h + section_h + len(line_imgs) * line_h + section_h
               + screens_height(screen_imgs, len(screens)) + MARGIN)
    sheet = Image.new("RGB", (SHEET_W, total_h), BG)
    draw = ImageDraw.Draw(sheet)

    y = MARGIN
    draw.text((MARGIN, y), "Readout name row proof, Myrtle 7X12 and the candidate screens", font=FONT_HEAD, fill=LIT)
    draw.text(
        (MARGIN, y + 24),
        "Every screen is drawn by the firmware's Canvas and fonts. 128 x 64 px, 1 px pad on every edge. "
        "The font draws the same set as Myrtle 9X12, so % stays blank.",
        font=FONT_SMALL,
        fill=DIM,
    )
    y += head_h

    draw.text((MARGIN, y), f"{FONT_NAME} glyph grid at 4x, {len(glyphs)} glyphs", font=FONT, fill=TEXT)
    y += section_h
    y = glyph_grid(draw, y, width, height, glyphs)

    draw.text((MARGIN, y), "Specimen lines, ink gap 3 px, 1x on the left and 2x on the right", font=FONT, fill=TEXT)
    y += section_h
    left_w = max(img.width for img in line_imgs) + 16
    for img in line_imgs:
        sheet.paste(tinted(img, 1), (MARGIN, y))
        sheet.paste(tinted(img, 2), (MARGIN + left_w, y))
        y += line_h

    draw.text((MARGIN, y), "Candidate screens at 3x. The width is the name row's ink width from Canvas::textWidth.", font=FONT, fill=TEXT)
    y += section_h
    screen_grid(sheet, draw, y, screens, screen_imgs)
    return sheet


def title_sheet(out_dir):
    screens, screen_imgs = read_manifest(out_dir, "title")
    head_h = 56
    total_h = MARGIN + head_h + screens_height(screen_imgs, len(screens)) + MARGIN
    sheet = Image.new("RGB", (SHEET_W, total_h), BG)
    draw = ImageDraw.Draw(sheet)

    y = MARGIN
    draw.text((MARGIN, y), "Readout title bar proof, Myrtle 7X12 on an inverted name row", font=FONT_HEAD, fill=LIT)
    draw.text(
        (MARGIN, y + 24),
        "Drawn by the firmware's Canvas and fonts at 3x. The bar is 14 px tall, rows 0 to 13, text top at y 1. "
        "Bar width and name width are in px.",
        font=FONT_SMALL,
        fill=DIM,
    )
    y += head_h
    screen_grid(sheet, draw, y, screens, screen_imgs)
    return sheet


def main():
    out_dir, out_path = sys.argv[1], sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) > 3 else "name"
    sheet = title_sheet(out_dir) if mode == "title" else name_sheet(out_dir)
    sheet.save(out_path)
    print(f"wrote {out_path} {sheet.width} x {sheet.height}")


if __name__ == "__main__":
    main()
