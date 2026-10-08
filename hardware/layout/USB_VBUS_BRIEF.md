# USB pair diagonals and VBUS_SENSE spacing (2026-10-08)

## Why

1. USB_DP (113.85,62.3)->(109.05,67.1) and USB_DM (114.25,62.607)->(109.357,67.5) are 6.8 mm 45° runs. Canon 136 caps a 45° run at 6 mm.
2. VBUS_SENSE runs 12.8 mm beside USB_DM on F.Cu at 0.65 mm pitch (0.46 at the 45°), under canon 51's 3W (0.6 pitch for 0.2 traces). It also carries a 0.25 mm jog at x 107 (canon 136).

Both layers of the touched area were surveyed with `foreman band`. F.Cu is clear at x 112.5–117.5, y 61–69 (only B.Cu crossers: 5V 60.8, ENC_MENU_A/B/SW 65.4/66.05/66.7). The VBUS_SENSE lane y 69–73.5 holds, on F.Cu: USB_DP 70.3, USB_DM 70.7, 3V3 71.975 (x >= 107.225) with its 45° (104.925,74.275)->(107.225,71.975), 3V3 stub x 94.5 y 72.4–73.2 + via (94.5,73.2), GND (103.86–104.7, 72.725) + via (104.7,72.725), MUTE_SW x 96 y 72.4–74.19.

## Numbers

Trace 0.2. Clearance 0.25 (DRC). 3W pitch 0.6. Via 0.6/0.3, via centre to other-net trace >= 0.6.

## Geometry

### USB_DP (F.Cu, 0.2)
- Vertical x 113.85: end moves from y 62.3 to **63.2**.
- 45°: (113.85,63.2) -> (109.95,67.1). Run 5.52 mm.
- Horizontal y 67.1: (109.95,67.1) -> (105.034,67.1). Rest unchanged.

### USB_DM (F.Cu, 0.2)
- Vertical x 114.25: end moves from y 62.607 to **63.507**.
- 45°: (114.25,63.507) -> (110.257,67.5). Run 5.65 mm.
- Horizontal y 67.5: (110.257,67.5) -> (105.2,67.5). Rest unchanged.

Pair pitch stays 0.4 on every segment.

### VBUS_SENSE (F.Cu, 0.2), from the east end
- Unchanged: vertical x 118.235 (44.365–70.665), 45° (118.235,70.665)->(117.8,71.1).
- Horizontal y 71.1: (117.8,71.1) -> **(107.0,71.1)**. (3V3 at 71.975 below: pitch 0.875.)
- 45°: (107.0,71.1) -> (106.6,71.5). Parallel to the 3V3 45°, 0.78 apart.
- Horizontal y 71.5: (106.6,71.5) -> (94.02,71.5). Pitch to USB_DM (70.7) 0.8. R120 pads 1 and 2 (3V3 at x 94.5, MUTE_SW at x 96.0; pad copper y 71.968–72.832, x ±0.403) are 0.368 below the trace edge. GND via (104.7,72.725) edge 0.825.
- 45°: (94.02,71.5) -> (93.5,72.02). Its line x+y = 165.52 passes 0.385 (centre) from R120 pad 1's NW corner (94.097,71.968), edge 0.285. Keep this corner where it is; the pad fixes it.
- Vertical x 93.5: (93.5,72.02) -> (93.5,76.45) pin. Pitch 1.0 to USB_DM x 92.5 and to R120 pad 1 centre x 94.5.
- Delete the old segments: y 71.1 west of 107.0, the 45° (107.2,71.1)->(106.95,71.35), y 71.35 run, 45° (94.2,71.35)->(93.5,72.05), and vertical 93.5 from 72.05.

Correction (agent scratch DRC, 2026-10-08): what the first draft called a "3V3 stub at (94.5,72.4)" is R120 pad 1, 0.806 x 0.864 mm. The y 71.7 lane hit it. The lane is now y 71.5, the furthest the pad allows, and the final 45° returns to the existing corner. The gain on the DM run is pitch 0.65 -> 0.8 plus the x 107 jog removed.

## Not touched

POT_MUX_OUT runs 28 mm beside MUX_S2 at 0.8 pitch (4W) on F.Cu over the In1 GND plane. Firmware waits `kMuxSettleUs` after every select change before it reads (controls.cpp:103), so the only coupling moment is discarded. Leave it; register the reason if the foreman raises it.

## Order of work

1. USB_DP, then USB_DM. DRC after both.
2. VBUS_SENSE. DRC.
3. `hardware/tools/gates.sh`. Expect the angle gate to pass; DRC may still show the owner's D105 silk overlap (owner accepted it).
4. Zoom render of x 90–120, y 60–78 to `hardware/renders/cubevox-usb-vbus-zoom.png`.

Stop clause: any spot under 0.25 clearance is a brief defect. Stop and report it. Never squeeze.
