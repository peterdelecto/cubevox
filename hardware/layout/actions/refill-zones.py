"""Refill every zone. fm edit adds vias without refilling, so planes keep stale fill."""
import sys

import pcbnew

b = pcbnew.LoadBoard(sys.argv[1])
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])
