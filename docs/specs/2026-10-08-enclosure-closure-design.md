# Enclosure closure (owner 2026-10-07/08)

## Plan

The enclosure is two printed parts, a dish and a lid. The lid carries the rear plate with the jack slots. A straight PETG bead-and-groove on all four walls holds the lid down. A TPU-GF boot with a double bead wraps the seam and carries the drop load. Nothing needs a screw, a heat-set insert or post-processing.

## Decisions

1. Two printed parts, dish and lid. The lid carries the rear plate with the jack slots. No screws, no heat-set inserts, no post-processing. Jack nuts carry no closure load, and only J108 has a nut. No cantilever snap hooks, because they bend about layer lines at a short root and crack.
2. Closure is a straight PETG bead-and-groove on all four walls, a TPU-GF boot with a double bead, and a rabbet at the corners. The continuous wall is the compliant member (10 mm skirt, strain under 1 % across layers). That is why a bead works where a hook fails.
3. The lid and board are one unit, tied by the toggle nuts (hardware spec item 26). Assembly drops lid and board straight into the dish, snaps the four straight runs, then fits the boot. A fingernail notch in the dish front wall releases the lid.

## Joints

| Joint | Construction |
|---|---|
| Front + sides | Lid skirt 1.2 x 10 mm inside the 2.0 mm dish wall. Bead on skirt outer face, groove in dish inner face, bead centre 4 mm below the seam. |
| Rear | Lid rear plate 3.0 mm. Jack slots are open at the bottom for J101, J108 and J106, because the lid drops vertically and the holes cannot be closed. Dish rear wall rises to the jack axis and continues as a 1.2 x 10 mm inner tongue behind the plate. Bead on tongue outer face, groove in plate inner face. Bead segments run between the jack slots and from the outer jacks to the corners. |
| Corners | Rabbet tongue 2.0 mm, 0.45 mm clearance (DrumSynthV3 values). No enclosure bead within 12 mm of any corner, because a straight skirt cannot flex within about one skirt height of a stiff corner. |
| Boot | TPU-GF, sole and wall 2.0 mm. Lower bead into a groove in the dish outer wall, upper bead into a groove in a 4 mm lid rim band. Both beads use the DrumSynthV3 profile (1.3 mm proud, 1.4 mm groove, 45 deg lower face, 2:1 upper face). The boot bead follows the full profile including corners. At the rear it reaches the seam only between the jacks. |
| Release | Fingernail notch in the dish front wall, 1.5 mm wide x 0.8 mm deep (DrumSynthV3 door_nail_groove). |

## Bead profile

Both PETG parts share one profile.

1. Bead height h equals groove depth and sets the interference. A coupon test picks h from 0.5, 0.7 and 0.9 mm. The spec value is TBD until the owner's pull test.
2. The lead-in face sits 30 deg from horizontal, which is 60 deg from the wall plane. It is a gentle ramp. It faces up in print on both parts, so any angle prints.
3. The retaining face sits 45 deg from the wall plane. It is an overhang in print on both parts (the lid prints face down, the dish prints floor down). 45 deg is the printable limit without support, so it is the maximum retention angle.
4. The dish wall inner top edge carries a 1.0 mm x 45 deg chamfer as a lead-in.
5. The bead crest flat is 1.0 mm. The groove opening height is 1.0 + h tan 30 deg + h, which is about 2.1 mm at h = 0.7 mm.
6. Beads run on straight sections only. A bead stops 12 mm from each corner and 3 mm from each jack slot.

## Hold estimate

PETG alone, h = 0.7 mm, 1.2 mm skirt, 2.0 mm wall.

| Run | Bead length | Lift hold |
|---|---|---|
| Sides | 240 mm | about 45 N |
| Front | 160 mm | about 28 N |
| Rear segments | 100 to 120 mm | about 22 N |
| Total | | about 95 N |

The boot carries the drop case. The target is a 1 m drop onto concrete on every face, edge and corner with the boot on, and the box does not open. With the boot off, the beads do not crack.

## Coupon

The coupon is a shelled box (owner 2026-10-08), because flat strips only show the mid-run force and a box adds the stiff corners and the whole-lid behaviour. No boot in the coupon. PETG, 0.2 mm layers, printed in the enclosure's orientation (dish floor down, lids plate down). The Fusion document is `cubevox-closure-coupon`. The STLs are `hardware/exports/closure-coupon-dish.stl` and `closure-coupon-lid-{05,07,09}.stl`.

| Part | Geometry |
|---|---|
| dish | 80 x 60 x 27 outer, square corners. Floor 2.0, walls 2.0 x 25 (front-wall height). Groove on all four inner faces, depth 0.9 (the deepest case, so one dish serves all three lids), centre 4 mm below the top, stopping 12 mm from each corner. Top inner edge chamfer 1.0 x 45 deg. Fingernail notch 12 wide x 0.8 deep x 2 tall on the front outer face under the seam. 5 mm hole in the floor centre for the scale hook. |
| lid-hh | Plate 80 x 60 x 2. Skirt 1.2 x 10 inside the wall with 0.15 mm clearance per side. Bead on all four skirt outer faces, proud by h + 0.15 so the engagement into the groove is h, centre 4 mm below the plate, same run lengths as the grooves. 5 mm hole in the plate centre. |

Pass criteria per lid:

1. Thumb insertion under about 30 N.
2. Pull-off on a luggage scale, hook through the lid hole, cord through the floor hole, of at least 85 N (the whole-box hold the real enclosure needs from the PETG beads alone).
3. No whitening on the skirt or the wall after 10 cycles.
4. The fingernail notch releases the front run without a tool.

Each lid is marked by notches (2 x 2.5 mm) on one plate edge. One, two and three notches mean h = 0.5, 0.7 and 0.9 mm.

The lid that passes with the highest h sets the spec value. The box is smaller than the enclosure, so its runs are stiffer near the corners than the enclosure's long sides; a lid that holds here holds there.

## Board consequences

1. Hardware spec item 27 (M3 x 20 through lid and board into heat-set inserts) is superseded.
2. H101 to H104 stay as plain rest bosses. Removing them from the board is a separate layout task.

## Drawing

`hardware/renders/enclosure-closure-section.png`, made by `hardware/tools/draw_closure_section.py`.
