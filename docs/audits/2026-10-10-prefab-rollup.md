# Pre-fab audit roll-up, 2026-10-10

Five read-only audits ran against commit c2635eb (brief: `hardware/layout/2026-10-10-prefab-agent-brief.md`):
`2026-10-10-prefab-dfm.md` (fab limits), `-bom.md` (assembly), `-electrical.md` (schematic and layout
meaning), `-plane.md` (ground return), renders in `hardware/renders/2026-10-10-prefab-*.png`.
Routing style was already clean (`2026-10-09-prefab-mega-audit.md`, c2635eb).

## 1. Fixed in the follow-up commit

| # | finding | fix |
|---|---|---|
| 1 | K103 pad contacts on the NO pins 4/5, so coil off = pad in (PEDAL); c4788e2 wants coil off = MIC | IN_P to pin 2, LINE_N to pin 7 (NC); pins 4/5 open; IN_N stays on COM 6; board re-laid (`-board-fix.py` 1a/1b, pass2) |
| 2 | C127, the only decoupler on U105 pin 8, reached the pin through two vias and a B.Cu jog (7.3 mm loop) | F.Cu trace C127.1 to U105.8, jog removed; the pin sits in the 5VA chain through the cap |
| 3 | GND via inside D2.2 (USB ESD diode), redundant with the via 0.94 mm south | removed |
| 4 | RESET and BOOT silk at 0.10 mm stroke | 0.15 mm |
| 5 | Project hole-to-hole rule 0.25 mm against JLC 0.50 mm | 0.50 mm, DRC 0 hits |
| 6 | MUX_S0 (PE12) is the ROM bootloader's SPI4 SCK while PE11 PAD_EN holds its NSS low; a floating SCK can lock the bootloader to SPI and USB DFU fails | R162 100k 0402 C25741 MUX_S0 to GND, pass-through on the MUX_S0 vertical at (143.75, 114.0) |
| 7 | PARTS.md 17 LCSC numbers short, three dead rows, K103 set missing; PINMAP and pins.h call PE11 spare; CPL rotations negative | updated; make_bom.py writes 0 to 359 |

## 2. Accepted by the owner (2026-10-10)

1. NRST cap C14 stays at the reset switch, 31 mm from U1.14. The crystal occupies the top-side room next to pin 14 and the net also serves the SWD header.
2. EasyEDA pin-1 dots at 0.06 mm (147) and KiCad library outlines at 0.12 mm (166) stay. JLC prints both on its own parts; widening the dots would push them onto pads.
3. No fuse on the 9 V input. Pedal supplies are current limited; D106 SS34 series and D105 SMBJ20CA stay.
4. Fixed 20 dB first stage. Minimum gain clips at 128.8 dB SPL with an SM58.
5. 48 V phantom into the XLR needs a male-to-male cable into a console mic input; 14 mA into 5VA and 2.8 V reverse on C120/C124, nothing fails short term. Note "phantom off" in the manual.
6. Stitching vias in the pot field, return vias for the LINE_N / VREF layer changes, and the U8.5 / U6.9 ground traces stay as they are.
7. Pot-row slot vias were not added. Both inner planes carry the same 7.9 x 1.1 mm slot, so an In1-In2 via at a slot end shortens no return path.

## 3. Owner checks at upload and on the first boards

1. JLC placement preview: U1, SOT-23s, diodes, polar caps, toggles, J117. (U107, U108 and C19 were on KiCad stock footprints whose zero sits 180 deg from JLC's; swapped to the JLC footprints 2026-10-10 before v4, so the v3 CPL is wrong for those three and must not be uploaded. J117's hand-made footprint matches the JLC socket footprint pad for pad.)
2. JLC DFM viewer: the pot lug ovals (1.25:1 slots), the J1 USB-C 0.6 mm slots, hole-to-hole at exactly 0.50 mm near the buck (GND / 5V vias at y 56.8).
3. First board: K103 continuity, coil off: K103.2 to K103.3 and K103.6 to K103.7 closed, 4/5 open. K102 muted at reset. DFU by menu and by BOOT0 with USB connected.
4. Bench items carried forward: OPA2197 swing, noise dBu, RVGAIN1 rotation sense, input current at 9.6 V.

## 4. Production v4

Upload `cubevox-gerbers-v4.zip`, `cubevox-bom-v4.csv`, `cubevox-cpl-v4.csv`: 4-layer JLC04161H-7628,
standard PCBA top side with THT. v2 and v3 are superseded (v3 carries U107, U108 and C19 rotations
180 deg off JLC's convention). Rule from here: every assembled part uses the JLC/EasyEDA footprint
for its LCSC number; KiCad stock footprints only remain on symmetric two-pad passives (rev-B tidy).
