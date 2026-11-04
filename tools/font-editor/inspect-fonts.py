#!/usr/bin/env python3
"""Measure and render the Stylized Hyperlegible fonts for a design review.

Reads the editor library and writes, under build/fontaudit/, a glyph sheet per font, sample
lines laid out the way the firmware lays them out, a cross-size comparison sheet and a
metrics report per font plus one across the sizes.
"""
import os
import sys
import json
import statistics
from collections import Counter

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIB = os.path.join(ROOT, 'tools', 'font-editor', 'fonts', 'cubevox-fonts.json')
OUT = os.path.join(ROOT, 'build', 'fontaudit')

# name, tag, sheet scale, compare scale, ink gap the firmware uses (0 = fixed pitch)
FONTS = [
    ('Stylized Hyperlegible 5x7', '5x7', 10, 14, 0),
    ('Stylized Hyperlegible 9X12', '9x12', 8, 8, 3),
    ('Stylized Hyperlegible 16X24', '16x24', 5, 4, 3),
]

SAMPLES = {
    '5x7': ['HHOHHOOHH', '0O1I!5S8B2Z6G', '0123456789', 'ABCDEFGHIJKLM', 'NOPQRSTUVWXYZ',
            'UPDATE FIRMWARE', 'INPUT GAIN  -6.5 DB', 'THRESHOLD   -24 DB', 'SPRING REVERB  50 %',
            '[]{}<>|~^`@;?=*$\\_'],
    '9x12': ['HHOHHOOHH', '0O1I!5S8B', '0123456789', 'ABCDEFGHIJKLM', 'NOPQRSTUVWXYZ',
             'INPUT GAIN', 'AUTOTUNE', 'SLAPBACK', 'DISTORTION', 'SPRING REVERB', 'Bb / Gm',
             '-12 ST 250 MS', '(),.:-!"\'#&/'],
    '16x24': ['HHOHHOOHH', '0O1I!5S8B', '0123456789', 'ABCDEFGH', 'IJKLMNOP', 'QRSTUVWX',
              'YZ Bb Gm', 'C / Am', 'F# / D#m', 'Db / Bbm', '-12 ST', '250 MS', '-6.5 DB', '100 %',
              '+,.:\'"#&()/'],
    # The revised 9X12 proof: lowercase a and e pick the second alternates held in those cells.
    'Stylized Hyperlegible 9X12 Revised': [
        'HHOHHOOHH', 'OCGQ 0O DO', '0O1I!5S8B', 'PFEF BR KX', '0123456789', 'ABCDEFGHIJKLM',
        'NOPQRSTUVWXYZ', 'INPUT GAIN', 'AUTOTUNE', 'SLAPBACK', 'SPRING REVERB', 'DECAY RATE',
        'auTOTUNe', 'SLaPBaCK', 'SPRING ReVeRB', 'DeCaY RaTe', 'THRESHOLD', 'Bb / Gm',
        '-12 ST 250 MS', '(),.:-!"\'#&/'],
}

LABELS = {
    'Stylized Hyperlegible 9X12 Revised': {'a': 'A alt 2', 'e': 'E alt 2'},
}

BAR_GLYPHS = 'EFHABPR38S-+=Z25G4'
DIAGONAL_GLYPHS = 'VNZX/7AKMWY'
OPEN_GLYPHS = 'CGS235690'


class Glyph:
    def __init__(self, code, rows):
        self.code = code
        self.ch = chr(code)
        self.px = [[c == '#' for c in r] for r in rows]
        self.h = len(self.px)
        self.w = len(self.px[0])
        self.ink = sum(sum(r) for r in self.px)
        self.box = self._box()

    def _box(self):
        ys = [y for y in range(self.h) if any(self.px[y])]
        if not ys:
            return None
        xs = [x for x in range(self.w) if any(self.px[y][x] for y in range(self.h))]
        return (ys[0], ys[-1], xs[0], xs[-1])

    def at(self, x, y):
        return 0 <= x < self.w and 0 <= y < self.h and self.px[y][x]

    def row(self, y):
        return self.px[y]

    def col(self, x):
        return [self.px[y][x] for y in range(self.h)]


class Font:
    def __init__(self, spec, tag, sheet_scale, compare_scale, gap):
        self.name = spec['name']
        self.tag = tag
        self.w = spec['width']
        self.h = spec['height']
        self.spacing = spec['spacing']
        self.sheet_scale = sheet_scale
        self.compare_scale = compare_scale
        self.gap = gap
        self.glyphs = {chr(int(c)): Glyph(int(c), rows) for c, rows in spec['glyphs'].items()}

    def inked(self):
        return [g for g in self.glyphs.values() if g.ink]

    def resolve(self, ch):
        """The firmware's rule: a lowercase cell with ink is its own glyph, a blank one maps to its capital."""
        g = self.glyphs.get(ch)
        if ch.islower() and (g is None or g.ink == 0):
            g = self.glyphs.get(ch.upper())
        return g


# ---------------------------------------------------------------- measurement

def runs(seq):
    out, n = [], 0
    for v in seq:
        if v:
            n += 1
        elif n:
            out.append(n)
            n = 0
    if n:
        out.append(n)
    return out


def inner_blank_runs(seq):
    """Blank runs between the first and last ink of a row or column."""
    xs = [i for i, v in enumerate(seq) if v]
    if len(xs) < 2:
        return []
    return runs([not v for v in seq[xs[0]:xs[-1] + 1]])


def hruns(g):
    return Counter(n for y in range(g.h) for n in runs(g.row(y)))


def vruns(g):
    return Counter(n for x in range(g.w) for n in runs(g.col(x)))


def gaps(g):
    hs = [n for y in range(g.h) for n in inner_blank_runs(g.row(y))]
    vs = [n for x in range(g.w) for n in inner_blank_runs(g.col(x))]
    return (min(hs) if hs else None, min(vs) if vs else None)


def components(g, diagonal):
    steps = [(1, 0), (-1, 0), (0, 1), (0, -1)]
    if diagonal:
        steps += [(1, 1), (1, -1), (-1, 1), (-1, -1)]
    seen, n = set(), 0
    for y in range(g.h):
        for x in range(g.w):
            if not g.px[y][x] or (x, y) in seen:
                continue
            n += 1
            stack = [(x, y)]
            seen.add((x, y))
            while stack:
                cx, cy = stack.pop()
                for dx, dy in steps:
                    p = (cx + dx, cy + dy)
                    if g.at(*p) and p not in seen:
                        seen.add(p)
                        stack.append(p)
    return n


def lone_pixels(g):
    return sum(1 for y in range(g.h) for x in range(g.w)
               if g.px[y][x] and not (g.at(x - 1, y) or g.at(x + 1, y) or g.at(x, y - 1) or g.at(x, y + 1)))


def checkerboards(g):
    n = 0
    for y in range(g.h - 1):
        for x in range(g.w - 1):
            a, b, c, d = g.px[y][x], g.px[y][x + 1], g.px[y + 1][x], g.px[y + 1][x + 1]
            if (a and d and not b and not c) or (b and c and not a and not d):
                n += 1
    return n


def corner(g, x_edge, y_edge, dx, dy):
    """Blank pixels along the edge row and the edge column before ink starts, as (along, down)."""
    along = 0
    while g.at(x_edge + along * dx, y_edge) is False and along < g.w and 0 <= x_edge + along * dx < g.w:
        along += 1
    down = 0
    while g.at(x_edge, y_edge + down * dy) is False and down < g.h and 0 <= y_edge + down * dy < g.h:
        down += 1
    return (along, down)


def corners(g):
    t, b, l, r = g.box
    return {'TL': corner(g, l, t, 1, 1), 'TR': corner(g, r, t, -1, 1),
            'BL': corner(g, l, b, 1, -1), 'BR': corner(g, r, b, -1, -1)}


def bars(g):
    """Row bands whose ink run spans most of the ink width."""
    t, b, l, r = g.box
    wide = max(3, int(round(0.6 * (r - l + 1))))
    rows = [y for y in range(t, b + 1) if any(n >= wide for n in runs(g.row(y)))]
    bands, start = [], None
    for i, y in enumerate(rows):
        if start is None:
            start = y
        if i + 1 == len(rows) or rows[i + 1] != y + 1:
            bands.append((start, y))
            start = None
    return bands


def mirror_diff(g):
    t, b, l, r = g.box
    return sum(1 for y in range(t, b + 1) for i in range(r - l + 1)
               if g.px[y][l + i] != g.px[y][r - i])


def openings(g):
    """Rows of the ink box left blank on the left edge and on the right edge."""
    t, b, l, r = g.box
    left = sum(1 for y in range(t, b + 1) if not g.px[y][l])
    right = sum(1 for y in range(t, b + 1) if not g.px[y][r])
    return (left, right)


def diagonal_runs(g):
    """Per-row ink run lengths, for strokes that step across the cell."""
    return [runs(g.row(y)) for y in range(g.box[0], g.box[1] + 1)]


def fmt_counter(c):
    return '{' + ' '.join(f'{k}x{v}' for k, v in sorted(c.items())) + '}'


def glyph_report(g):
    t, b, l, r = g.box
    cs = corners(g)
    gh, gv = gaps(g)
    line = (f"{g.ch!r:5} box t{t} b{b} l{l} r{r} w{r - l + 1} h{b - t + 1} ink {g.ink:3} | "
            f"hruns {fmt_counter(hruns(g))} vruns {fmt_counter(vruns(g))} | gaps h{gh} v{gv} | "
            f"comp {components(g, False)}/{components(g, True)} lone {lone_pixels(g)} chk {checkerboards(g)} | "
            f"corners " + ' '.join(f"{k}{v}" for k, v in cs.items()) +
            f" | mirror {mirror_diff(g)} | open L{openings(g)[0]} R{openings(g)[1]}")
    if g.ch in BAR_GLYPHS:
        line += f" | bars {bars(g)}"
    if g.ch in DIAGONAL_GLYPHS:
        line += f" | rowruns {diagonal_runs(g)}"
    return line


def font_report(font):
    out = [f"== {font.name}: cell {font.w} x {font.h}, editor spacing {font.spacing}, "
           f"firmware {'fixed pitch ' + str(font.w + font.spacing) if font.gap == 0 else 'ink + ' + str(font.gap) + ' gap'}, "
           f"{len(font.inked())} inked glyphs ==", '']
    out.append('-- per glyph --')
    for g in sorted(font.inked(), key=lambda g: g.code):
        out.append(glyph_report(g))
    caps = [font.glyphs[c] for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' if font.glyphs[c].ink]
    digits = [font.glyphs[c] for c in '0123456789' if font.glyphs[c].ink]
    out += ['', '-- vertical metrics --']
    for label, gs in (('caps', caps), ('digits', digits)):
        out.append(f"{label} top rows {fmt_counter(Counter(g.box[0] for g in gs))} "
                   f"bottom rows {fmt_counter(Counter(g.box[1] for g in gs))}")
    punct = [g for g in font.inked() if not g.ch.isalnum()]
    out.append('punctuation box (top,bottom,left,right): ' +
               ' '.join(f"{g.ch}{g.box}" for g in sorted(punct, key=lambda g: g.code)))
    out += ['', '-- widths --']
    out.append(f"caps ink widths {fmt_counter(Counter(g.box[3] - g.box[2] + 1 for g in caps))}")
    out.append('caps by width: ' + ' '.join(f"{g.ch}{g.box[3] - g.box[2] + 1}" for g in sorted(caps, key=lambda g: g.box[3] - g.box[2])))
    out.append('digit ink widths: ' + ' '.join(f"{g.ch}{g.box[3] - g.box[2] + 1}" for g in digits))
    out.append('digit ink left/right: ' + ' '.join(f"{g.ch}:{g.box[2]}-{g.box[3]}" for g in digits))
    out += ['', '-- colour (ink count, caps and digits, median marked) --']
    allg = caps + digits
    med = statistics.median(g.ink for g in allg)
    out.append(f"median {med}; " + ' '.join(f"{g.ch}{g.ink}{'+' if g.ink > 1.25 * med else '-' if g.ink < 0.75 * med else ''}"
                                             for g in sorted(allg, key=lambda g: g.ink)))
    out += ['', '-- stroke widths (all caps and digits) --']
    out.append(f"horizontal runs {fmt_counter(sum((hruns(g) for g in allg), Counter()))}")
    out.append(f"vertical runs {fmt_counter(sum((vruns(g) for g in allg), Counter()))}")
    o = font.glyphs.get('O')
    if o and o.ink:
        mid = (o.box[0] + o.box[1]) // 2
        out.append(f"O wall runs at mid row {mid}: {runs(o.row(mid))}; O top bar runs at col mid: {runs(o.col((o.box[2] + o.box[3]) // 2))}")
    out += ['', '-- bars (row bands) and cell centre --']
    out.append(f"cell rows 0..{font.h - 1}, centre {(font.h - 1) / 2}")
    for c in BAR_GLYPHS:
        g = font.glyphs.get(c)
        if g and g.ink:
            out.append(f"{c} {bars(g)}")
    out += ['', '-- gaps (smallest blank run inside the ink, horizontal/vertical) --']
    out.append(' '.join(f"{g.ch}{gaps(g)[0]}/{gaps(g)[1]}" for g in sorted(font.inked(), key=lambda g: g.code)))
    out += ['', '-- flags --']
    for g in sorted(font.inked(), key=lambda g: g.code):
        flags = []
        if lone_pixels(g):
            flags.append('lone')
        if checkerboards(g):
            flags.append('checkerboard')
        if components(g, False) != components(g, True):
            flags.append('diagonal-only link')
        if components(g, True) > 1 and g.ch not in '!":;%=i?j':
            flags.append(f'{components(g, True)} pieces')
        if flags:
            out.append(f"{g.ch!r} " + ', '.join(flags))
    return '\n'.join(out) + '\n'


def compare_report(fonts):
    common = sorted(set.intersection(*[set(g.ch for g in f.inked()) for f in fonts]), key=ord)
    out = ['== glyphs present in all sizes: ' + ''.join(common), '']
    for ch in common:
        out.append(f"{ch!r}")
        for f in fonts:
            g = f.glyphs[ch]
            t, b, l, r = g.box
            w, h = r - l + 1, b - t + 1
            cs = corners(g)
            bar_pos = [round(((a + z) / 2 - t) / max(h - 1, 1), 2) for a, z in bars(g)] if ch in BAR_GLYPHS else ''
            lo, ro = openings(g)
            out.append(f"   {f.tag:6} w{w:2} h{h:2} w/h {w / h:.2f} ink {g.ink / (w * h):.2f} "
                       f"corners " + ' '.join(f"{k}{v}" for k, v in cs.items()) +
                       f" open L{lo / h:.2f} R{ro / h:.2f} bars {bar_pos} mirror {mirror_diff(g)} stroke {hruns(g).most_common(1)[0][0]}")
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------- rendering

def paint(d, g, x0, y0, s, grid):
    for y in range(g.h):
        for x in range(g.w):
            if g.px[y][x]:
                d.rectangle([x0 + x * s, y0 + y * s, x0 + (x + 1) * s - 1, y0 + (y + 1) * s - 1], fill=255)
    if grid:
        for x in range(g.w + 1):
            d.line([(x0 + x * s, y0), (x0 + x * s, y0 + g.h * s)], fill=60)
        for y in range(g.h + 1):
            d.line([(x0, y0 + y * s), (x0 + g.w * s, y0 + y * s)], fill=60)


def sheet(font, path):
    gs = sorted(font.inked(), key=lambda g: g.code)
    s, cols = font.sheet_scale, 10
    cw, chh = font.w * s + 14, font.h * s + 26
    rows = (len(gs) + cols - 1) // cols
    img = Image.new('L', (cols * cw + 10, rows * chh + 10), 0)
    d = ImageDraw.Draw(img)
    labels = LABELS.get(font.name, {})
    for i, g in enumerate(gs):
        x0, y0 = 10 + (i % cols) * cw, 10 + (i // cols) * chh
        d.text((x0, y0), labels.get(g.ch, f"{g.ch} {g.code}"), fill=170)
        paint(d, g, x0, y0 + 14, s, True)
    img.save(path)


def pair_sheet(a, b, path):
    """Every glyph of two same-size fonts side by side, a on the left."""
    chars = sorted(set(g.ch for g in a.inked()) | set(g.ch for g in b.inked()), key=ord)
    s, per_row = 8, 8
    gw = (a.w + b.w) * s + 30
    gh = max(a.h, b.h) * s + 26
    rows = (len(chars) + per_row - 1) // per_row
    img = Image.new('L', (per_row * gw + 10, rows * gh + 10), 0)
    d = ImageDraw.Draw(img)
    labels = LABELS.get(b.name, {})
    for i, ch in enumerate(chars):
        x0, y0 = 10 + (i % per_row) * gw, 10 + (i // per_row) * gh
        d.text((x0, y0), str(labels.get(ch, ch)), fill=170)
        paint(d, a.glyphs[ch], x0, y0 + 14, s, True)
        paint(d, b.glyphs[ch], x0 + a.w * s + 12, y0 + 14, s, True)
    img.save(path)


def layout(font, text):
    """Pen positions the firmware would use. Returns [(cell x, glyph)] and the text width."""
    placed, pen, trail = [], 0, 0
    for ch in text:
        g = None if ch == ' ' else font.resolve(ch)
        if font.gap == 0:
            if g and g.ink:
                placed.append((pen, g))
            pen += font.w + font.spacing
            trail = font.spacing
        else:
            if g is None or g.ink == 0:
                pen += font.w // 2
                trail = 0
                continue
            l, r = g.box[2], g.box[3]
            placed.append((pen - l, g))
            pen += (r - l + 1) + font.gap
            trail = font.gap
    return placed, max(pen - trail, 1)


def render_line(font, text):
    placed, width = layout(font, text)
    img = Image.new('L', (width, font.h), 0)
    for x0, g in placed:
        for y in range(g.h):
            for x in range(g.w):
                if g.px[y][x] and 0 <= x0 + x < width:
                    img.putpixel((x0 + x, y), 255)
    return img


def samples(font, path_x1, path_x3):
    by_size = '5x7' if font.h <= 7 else '9x12' if font.h <= 12 else '16x24'
    lines = [render_line(font, t) for t in SAMPLES.get(font.name) or SAMPLES.get(font.tag) or SAMPLES[by_size]]
    lead = max(2, font.h // 4)
    width = max(l.width for l in lines) + 8
    img = Image.new('L', (width, sum(l.height + lead for l in lines) + 4), 0)
    y = 4
    for l in lines:
        img.paste(l, (4, y))
        y += l.height + lead
    img.save(path_x1)
    img.resize((img.width * 3, img.height * 3), Image.Resampling.NEAREST).save(path_x3)


def compare_sheet(fonts, chars, path):
    per_row = 6
    scales = [f.compare_scale for f in fonts]
    group_w = sum(f.w * s for f, s in zip(fonts, scales)) + 8 * len(fonts) + 10
    group_h = max(f.h * s for f, s in zip(fonts, scales)) + 24
    rows = (len(chars) + per_row - 1) // per_row
    img = Image.new('L', (per_row * group_w + 10, rows * group_h + 10), 0)
    d = ImageDraw.Draw(img)
    for i, ch in enumerate(chars):
        gx, gy = 10 + (i % per_row) * group_w, 10 + (i // per_row) * group_h
        d.text((gx, gy), ch, fill=170)
        x = gx
        bottom = gy + group_h - 4
        for f, s in zip(fonts, scales):
            g = f.resolve(ch) or f.glyphs[' ']
            paint(d, g, x, bottom - g.h * s, s, True)
            x += g.w * s + 8
    img.save(path)


def run_family():
    """The three project fonts from the editor library: sheets, samples, metrics, cross-size sheets."""
    with open(LIB) as fh:
        lib = json.load(fh)
    by_name = {f['name']: f for f in lib['fonts']}
    fonts = [Font(by_name[name], tag, ss, cs, gap) for name, tag, ss, cs, gap in FONTS]
    for f in fonts:
        sheet(f, os.path.join(OUT, f'{f.tag}-sheet.png'))
        samples(f, os.path.join(OUT, f'{f.tag}-samples-x1.png'), os.path.join(OUT, f'{f.tag}-samples-x3.png'))
        with open(os.path.join(OUT, f'{f.tag}-metrics.txt'), 'w') as fh:
            fh.write(font_report(f))
    compare_sheet(fonts, list('ABCDEFGHIJKLMNOPQRSTUVWXYZ'), os.path.join(OUT, 'compare-letters.png'))
    compare_sheet(fonts, list('0123456789b') + list('!"#&\'(),-./:'), os.path.join(OUT, 'compare-digits-punct.png'))
    with open(os.path.join(OUT, 'compare-metrics.txt'), 'w') as fh:
        fh.write(compare_report(fonts))


def run_one(lib_path, name, tag, gap, pair_with):
    """One font from any library: sheet, samples, metrics, and a before/after sheet against pair_with."""
    with open(lib_path) as fh:
        lib = json.load(fh)
    by_name = {f['name']: f for f in lib['fonts']}
    h = by_name[name]['height']
    scales = (10, 14) if h <= 7 else (8, 8) if h <= 12 else (5, 4)
    f = Font(by_name[name], tag, scales[0], scales[1], gap)
    sheet(f, os.path.join(OUT, f'{tag}-sheet.png'))
    samples(f, os.path.join(OUT, f'{tag}-samples-x1.png'), os.path.join(OUT, f'{tag}-samples-x3.png'))
    with open(os.path.join(OUT, f'{tag}-metrics.txt'), 'w') as fh:
        fh.write(font_report(f))
    if pair_with:
        before = Font(by_name[pair_with], 'before', scales[0], scales[1], gap)
        pair_sheet(before, f, os.path.join(OUT, f'{tag}-before-after.png'))


def main(argv):
    """inspect-fonts.py                                  the three project fonts
       inspect-fonts.py LIB NAME TAG GAP [BEFORE_NAME]   one font, spaced by ink with GAP (0 = fixed pitch)"""
    os.makedirs(OUT, exist_ok=True)
    if len(argv) >= 5:
        run_one(argv[1], argv[2], argv[3], int(argv[4]), argv[5] if len(argv) > 5 else None)
    else:
        run_family()
    print('wrote', OUT, sorted(os.listdir(OUT)))


if __name__ == '__main__':
    main(sys.argv)
