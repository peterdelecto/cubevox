#!/usr/bin/env python3
"""Export the C12 circle list as lid face coordinates to JSON (inlay_export.py C12,
written to build/cane/c12-circles.json unless a path is given)."""

import sys

from inlay_export import OUT_DIR, export

DEFAULT = OUT_DIR / 'c12-circles.json'


def main():
  export('C12', sys.argv[1] if len(sys.argv) > 1 else DEFAULT)


if __name__ == '__main__':
  main()
