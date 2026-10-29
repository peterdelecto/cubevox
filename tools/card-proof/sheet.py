"""Composes the card proof sheet from card_proof's raw frames.

  python3 -I tools/card-proof/sheet.py build/card-proof.raw build/card-proof.tsv
"""
import shutil
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, SCALE = 128, 64, 4
COLS, MARGIN, LABEL_H, BORDER = 2, 24, 40, 2
PAGE = (255, 255, 255)
GREY = (190, 190, 190)
OFF, ON = (0, 0, 0), (255, 255, 255)

raw = open(sys.argv[1], "rb").read()
rows = [line.rstrip("\n").split("\t") for line in open(sys.argv[2])]
assert len(raw) == W * H * len(rows), (len(raw), len(rows))

font = ImageFont.load_default(size=14)
fw, fh = W * SCALE, H * SCALE
cell_w = fw + 2 * BORDER
cell_h = LABEL_H + fh + 2 * BORDER
n_rows = (len(rows) + COLS - 1) // COLS
sheet = Image.new("RGB", (MARGIN + COLS * (cell_w + MARGIN), MARGIN + n_rows * (cell_h + MARGIN)), PAGE)
draw = ImageDraw.Draw(sheet)

for i, (name, caption) in enumerate(rows):
    ox = MARGIN + (i % COLS) * (cell_w + MARGIN)
    oy = MARGIN + (i // COLS) * (cell_h + MARGIN)
    draw.text((ox, oy), name, fill=(0, 0, 0), font=font)
    draw.text((ox, oy + 18), caption, fill=(60, 60, 60), font=font)
    draw.rectangle((ox, oy + LABEL_H, ox + cell_w - 1, oy + LABEL_H + fh + 2 * BORDER - 1), fill=GREY)
    img = Image.new("RGB", (W, H), OFF)
    img.putdata([ON if b else OFF for b in raw[i * W * H:(i + 1) * W * H]])
    sheet.paste(img.resize((fw, fh), Image.NEAREST), (ox + BORDER, oy + LABEL_H + BORDER))

stem = sys.argv[3] if len(sys.argv) > 3 else "card-proof"
sheet.save(f"build/{stem}.png")
shutil.copyfile(f"build/{stem}.png", f"tools/card-proof/{stem}.png")
print(sheet.size)
