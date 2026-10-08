# Stray trace / via audit — 2026-10-06, board at 48c9bb0

Tool: `tools/route_audit.py` (KiCad python, read-only). Scope: dangling or floating
segments, zero-length and duplicate segments, mid-track joins, vias with copper on one
side only, same-net vias under 1 mm apart, and every F.Cu→B.Cu→F.Cu hop tested for a
straight, L or 45° two-segment F.Cu path with 0.2 mm clearance.

| Count | |
|---|---|
| segments | 1312 |
| vias | 276 (98 signal, 178 GND) |
| floating / dangling / zero-length / duplicate | 0 |
| one-sided or orphan vias | 0 |
| B.Cu hops | 30 |
| F.Cu hops | 1 |

## 1. Stray traces
1. TOGGLE2 sliver, F.Cu (86.05,122.65)→(86.00,122.70), 0.07 mm. Hook from the y 122.65
   run onto a pad whose centre sits 0.05 mm off the row. Cosmetic.
2. Endpoint near-misses, 5–14 µm, electrically continuous, DRC clean:
   3V3A at x 73.10 (y 103.37/103.38 and 107.92/107.93), 3V3 at x 83.06 (y 103.37/103.38),
   3V3 B.Cu at (91.49,70.30)/(91.50,70.31). Cosmetic; snap if touched.
3. USB_DM via-less T at (115.75,37.25): J1.A7 branch joins the J1.B7→D2.4 vertical
   1.1 mm below the pad. Breaks the branch-only-at-pad-or-via rule; conventional for
   USB-C paired pins. Owner's call.
4. VDDA bridge U1.20↔U1.21 at y 91.55 lies inside pad copper. Not a defect.
5. TOGGLE1 and TOGGLE2 already carry SW101/SW102 → R/C partial routes at y 115…122.65.
   All pad-anchored; the open items are the U1 ends.

## 2. Stray vias
None. 35 GND vias carry no track; each sits within 2 mm of a signal via (return vias
per via_audit). Every signal via has copper on both layers.

## 3. Unnecessary vias
1. 3V3 under U8, vias (124.60,103.60) and (124.60,107.55), 3.95 mm B.Cu. A straight
   F.Cu run at x 124.60 is clear. Removing both leaves a via-less T onto the y 107.55
   trunk. Two vias vs one T-branch; owner's rule call.
2. U101 GND pins 15/16/17: three vias in a 1.3 mm column at x 143.38. One via with
   straight stubs from the outer pads would serve. Optional.
3. Q101.2 / R125.2 GND vias 0.93 mm apart. One shared via would serve. Optional.
4. U8 pins 11/12 GND vias 1.1 mm apart. ADC ground; keep.

## 4. Hops an F.Cu path could replace
Only the 3V3 hop above passes the straight/L/45 test. The other 29 B.Cu hops are
blocked on F.Cu by named copper; the short ones are I2S_DATA_SRC (5.0 mm, under the
U1 pin-1..4 escapes) and VBUS (3.9 mm, boxed by USB_DP and C18). POT_MUX_OUT's
four-via chain is justified: the 3V3A B.Cu trunk at y 106.3 blocks B.Cu, MUX_S0/S1
block F.Cu at y 120.

Limit: the path test tries straight, L and one 45° bend only. "Blocked" means no
simple path, not that no path exists.
