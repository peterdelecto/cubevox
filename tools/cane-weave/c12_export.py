#!/usr/bin/env python3
"""Export the C12 circle list as lid face coordinates to JSON."""

import json
import sys
from pathlib import Path

from candidates_b import FACE, ROOT
from candidates_c import c12_circle_list

DEFAULT = ROOT / 'build' / 'cane' / 'c12-circles.json'


def main():
  out = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
  circles = [
      [round(float(u + FACE[0]), 4), round(float(FACE[3] - v), 4), round(float(r), 4)]
      for u, v, r in c12_circle_list()
  ]
  doc = {
      'units': 'mm',
      'face': [float(x) for x in FACE],
      'count': len(circles),
      'circles': circles,
  }
  out.parent.mkdir(parents=True, exist_ok=True)
  out.write_text(json.dumps(doc) + '\n')

  rs = [c[2] for c in circles]
  xs = [c[0] for c in circles]
  ys = [c[1] for c in circles]
  print(f'count {len(circles)}')
  print(f'r min {min(rs)} max {max(rs)}')
  print(f'x min {min(xs)} max {max(xs)}')
  print(f'y min {min(ys)} max {max(ys)}')
  print(f'wrote {out}')


if __name__ == '__main__':
  main()
