"""Read a flat EPS into filled shapes, and write them as SVG for viewing.

Handles the Affinity export (m/l/c/closepath, RGB setcolor, eofill) and the
Illustrator export (mo/li/cv/cp, cmyk, f, scale/translate/ct). Only the page body
is read when the file has one, so procset definitions are skipped.

Usage: python3 -I eps_art.py in.eps out.svg
"""
import re
import sys

MOVE, LINE, CURVE = {'m', 'mo'}, {'l', 'li'}, {'c', 'cv'}
NONZERO, EVENODD = {'fill', 'f'}, {'eofill', 'ef'}
DROP = {'newpath', 'np', 'eoclip', 'clip', 'clp'}


def size(path):
  """Page width and height in points from the bounding box comment."""
  with open(path, encoding='latin-1') as f:
    text = f.read(200000)
  m = (re.search(r'%%HiResBoundingBox: *(\S+) +(\S+) +(\S+) +(\S+)', text)
       or re.search(r'%%BoundingBox: *(\S+) +(\S+) +(\S+) +(\S+)', text))
  x0, y0, x1, y1 = map(float, m.groups())
  return x1 - x0, y1 - y0


def body(path):
  with open(path, encoding='latin-1') as f:
    lines = f.read().splitlines()
  start = next((i for i, s in enumerate(lines) if s.startswith('%%Page:')), None)
  if start is None:
    return lines
  end = next(i for i in range(start, len(lines)) if lines[i].startswith('%%PageTrailer'))
  return lines[start:end]


def parse(path):
  """Return [(rgb, subpaths, evenodd)], one per fill; a subpath is [(x, y), seg, ...]
  where seg is ('l', x, y) or ('c', x1, y1, x2, y2, x, y). Page points, y up."""
  fills, subpaths, cur, rgb, stack = [], [], None, None, []
  ctm, saved = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0), []

  def pt(x, y):
    a, b, c, d, e, f = ctm
    return a * x + c * y + e, b * x + d * y + f

  def concat(m):
    a, b, c, d, e, f = ctm
    p, q, r, s, t, u = m
    return (p * a + q * c, p * b + q * d, r * a + s * c, r * b + s * d,
            t * a + u * c + e, t * b + u * d + f)

  for line in body(path):
    line = line.split('%', 1)[0].replace('[', ' ').replace(']', ' ')
    for tok in line.split():
      if tok in MOVE:
        y, x = stack.pop(), stack.pop()
        cur = [pt(x, y)]
        subpaths.append(cur)
      elif tok in LINE:
        y, x = stack.pop(), stack.pop()
        cur.append(('l', *pt(x, y)))
      elif tok in CURVE:
        v = stack[-6:]
        del stack[-6:]
        cur.append(('c', *pt(v[0], v[1]), *pt(v[2], v[3]), *pt(v[4], v[5])))
      elif tok == 'setcolor':
        rgb = tuple(stack[-3:])
        del stack[-3:]
      elif tok == 'cmyk':
        c, m, y, k = stack[-4:]
        del stack[-4:]
        rgb = ((1 - c) * (1 - k), (1 - m) * (1 - k), (1 - y) * (1 - k))
      elif tok in NONZERO or tok in EVENODD:
        fills.append((rgb, subpaths, tok in EVENODD))
        subpaths = []
      elif tok in DROP:
        subpaths = []
      elif tok == 'scale':
        sy, sx = stack.pop(), stack.pop()
        ctm = concat((sx, 0, 0, sy, 0, 0))
      elif tok == 'translate':
        ty, tx = stack.pop(), stack.pop()
        ctm = concat((1, 0, 0, 1, tx, ty))
      elif tok in ('ct', 'concat'):
        m = stack[-6:]
        del stack[-6:]
        ctm = concat(m)
      elif tok == 'gsave':
        saved.append(ctm)
      elif tok == 'grestore':
        ctm = saved.pop()
      else:
        try:
          stack.append(float(tok))
        except ValueError:
          pass
  return fills


def svg_path(subpaths, h):
  out = []
  for sp in subpaths:
    x, y = sp[0]
    out.append(f'M{x:.3f} {h - y:.3f}')
    for s in sp[1:]:
      if s[0] == 'l':
        out.append(f'L{s[1]:.3f} {h - s[2]:.3f}')
      else:
        _, a, b, c, d, e, g = s
        out.append(f'C{a:.3f} {h - b:.3f} {c:.3f} {h - d:.3f} {e:.3f} {h - g:.3f}')
    out.append('Z')
  return ''.join(out)


def write_svg(fills, out, w, h):
  with open(out, 'w') as f:
    f.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}">\n')
    for rgb, sps, evenodd in fills:
      col = '#%02x%02x%02x' % tuple(round(c * 255) for c in rgb)
      rule = 'evenodd' if evenodd else 'nonzero'
      f.write(f'<path fill="{col}" fill-rule="{rule}" d="{svg_path(sps, h)}"/>\n')
    f.write('</svg>\n')


if __name__ == '__main__':
  fills = parse(sys.argv[1])
  write_svg(fills, sys.argv[2], *size(sys.argv[1]))
  print(len(fills), 'fills', sum(len(s) for _, s, _ in fills), 'subpaths')
