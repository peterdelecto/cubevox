# Readout card

The readout screen redrawn as the card the owner sketched on 2026-10-29. One effect's controls
on one screen: the slot and effect name in a strip, a knob picture beside each parameter name,
a switch picture for the bypass toggle, and the big value of the control that moved. Pixel
reference is `tools/card-proof/card_proof.cpp`, family D, drawn through the firmware's canvas and
the Myrtle fonts. Supersedes the Readout section of `2026-10-24-oled-gui-design.md`; the rest of
that spec (priority, hold, dissolve, voice, MUTED, panel) stays.

## Status
1. Owner approved proof D on 2026-10-29 (`tools/card-proof/card-proof-d.png`, frames D, Db, Dc,
   Dd, DT). Rejected on the way: A0 to A2 (stacked names, one row), B0 and B1 (two columns),
   C0 to C2 (big slot digit, names as bare words). C2 failed because five words in one font gave
   no cue which word was a knob.
2. Not built. Open decisions below wait on the owner; build items wait for the word.

## Goal
Adam has no printed names on the panel and eight slots to remember. Every touch of one effect,
either pot or its toggle, shows one screen that names the slot and effect, names both
parameters so he learns which pot is which, and reads out the control that moved. The screen is
a reminder at the moment of use, not a reference to look up.

## Screen, 128 x 64
| Element | Font / size | Position |
|---|---|---|
| Strip | lit rows 0 to 8, full width | text dark inside it |
| Slot number | 5x7 | x 2, top 1, in the strip |
| Effect name | 5x7 | x 14, top 1, in the strip |
| Switch picture | 5 x 7 | x 121 to 125, rows 1 to 7, in the strip |
| Upper knob | 9 x 9 | x 1, rows 11 to 19 |
| Upper name | 5x7 | x 13, top 12 |
| Lower knob | 9 x 9 | x 1, rows 21 to 29 |
| Lower name | 5x7 | x 13, top 22 |
| Value | 16X24 | top 34, in the fixed field of the OLED GUI spec |

1. Every piece of text keeps 1 px clear of edges, the strip, the pictures and other text.
2. The strip is a heading by shape. Both reverb names fit in 5x7 inside it (SPRING REVERB is
   77 px, ending at x 90).
3. No parameter name is truncated. Each name row is 114 px wide and the longest name is 53 px.
4. The value row, its fixed field, the rolling digits and the key names are unchanged from the
   OLED GUI spec. The 9X12 parameter name of the old readout is gone.

## Knob picture
1. A midpoint circle of radius 4 in a 9 x 9 box, with a 1 px pointer from the centre to the rim.
2. The pointer sits at the pot's position: a 270 degree sweep, 7 o'clock at 0, straight up at
   0.5, 5 o'clock at 1. The pointer follows the pot while the card shows.
3. The knob that moved is a lit disc with a dark pointer. The other is a hollow ring with a lit
   pointer. That difference marks the moving control; there is no bar on the names.

## Rows follow the panel
1. The upper row is the upper pot of the slot column, the lower row the lower pot. The panel
   column is toggle, pot, pot from the top (board: switches at y 115, pots at y 142 and y 168).
2. Pot A of the slot table is the upper pot in slots 1 and 4 to 8 and the lower pot in slots 2
   and 3, read from `board/slots.cpp` and the POT nets of `hardware/cubevox.kicad_pcb`. So
   Octave reads MIX over SEMITONES and Autotune reads RESPONSE over KEY. The firmware looks the
   order up per slot; the wiring does not change.
3. Not yet confirmed on glass: that board y 142 is the physical upper row and that the
   silkscreen numbers match the nets. One look at board 1 settles both.

## Toggle
1. A flip of the slot's toggle raises the card like a pot move, with the same hold and priority.
2. Both knobs draw hollow, the switch picture shows the new state (nub at the top for on, at the
   bottom for off), and the value row shows the state word, OFF in the proof.

## Open decisions, owner
1. The state word on the value row. OFF and ON as proofed, or BYPASS. The top switch set to
   Bypass already shows a screen reading BYPASS, so OFF keeps the two controls apart.
2. The switch picture. At 5 x 7 px it is about half a millimetre on the panel and reads as a
   small square. Keep it, enlarge it, or drop it and let the value row carry the state.
3. The hold. 2000 ms after the last move, as the readout has now, or longer for a card meant to
   teach.

## Build items, when told
1. `ui/screens` renders the card in place of `renderReadout`; the knob and switch pictures and
   the strip text move from the proof into the firmware.
2. The face takes a toggle-change event from the panel with the readout's debounce, hold and
   priority. A second control of the same slot moves the disc and the value without a dissolve;
   the dissolve covers the card arriving and leaving, as the GUI spec's rule 6.
3. Per-slot upper/lower lookup from the slot table.
4. The 27 goldens, the sweep test and `tools/oled-proof/oled_motion.cpp` are re-cut from the new
   screen. The fixed-field rule and the sweep gate stay.
5. Version bump in the same commit.
