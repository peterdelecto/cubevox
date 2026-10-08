# Pre-fab review (2026-10-08)

Four independent read-only reviews against a fresh netlist, the board file and the part datasheets: power tree, MCU and digital, analog audio, manufacturing and BOM. Astra (Codex) reviewed the USB path separately (`ASTRA_PACKET_USB_RESPONSE_2026-10-08.md`). Board at 79d86f0.

## Verdict

No board-level blocker found in any review. Pin map, power pins, alternate functions, boot, reset, SWD, USB, crystal, OLED, encoder, toggles, mux, LEDs, relay, codec strapping, barrel polarity, protection diodes, both bucks, the LDO, cap polarities and ratings, and all 76 LCSC part values match the schematic and the datasheets. Two firmware defects would stop audio and must be fixed before first power-up, but they do not touch the board.

## Changes before ordering (one schematic + board pass)

| # | Change | Why | Source |
|---|---|---|---|
| 1 | D105 SMBJ15CA -> SMBJ20CA (C113992, stock 40k, same SMB footprint) | 15 V part conducts from 16.7 V; an 18 V pedal supply cooks it. Downstream rated 25 V+ | power |
| 2 | Add 100k (C25803, already in BOM) from 3V3 to LDO_EN | SP809 is push-pull per MaxLinear, LCSC lists open-drain; a floating EN leaves 3V3A off | power |
| 3 | Add 10 uF 0805 on 3V3A within 5 mm of U6 pins 1/8 | TI PCM5102A typical app has 10 uF at AVDD/CPVDD; nearest 10 uF is 70 mm away | audio |
| 4 | R7 82k -> 68k | PA9 sees 3.74 V from a 5.25 V host with the box unpowered; 68k gives 3.53 V | Astra |
| 5 | Test pads: VIN_9V, 9V, GND, 3V3, VBUS, by the owner's silk labels | Owner request + Astra | owner |
| 6 | Board hole-to-copper clearance 0.25 -> 0.30 mm, refill, move the 3V3 track at J3's mounting hole | 35 items at 0.250 against JLC's 0.254 | mfg |
| 7 | DRC exclusion for the D105 silk overlap (owner accepted) or move the silk line | gates.sh baseline is 9 lib_footprint_mismatch only | mfg |
| 8 | Docs: PARTS.md line 72 SP809 is push-pull (not open-drain); hardware spec F3 XSMT is PC5 not PE9; spec item 5 U106A follower feeds nothing (R123 returns to raw BIAS_W) | digital, audio | 
| 9 | Owner decision: RVGAIN1 taper. Linear 10k as rheostat gives 12 of 30 dB in the first tenth of the turn; a 10k log part in the RK09D1130 footprint gives ~6 dB per quarter turn. LCSC has C470304/C470315/C470316/C470317 10k variants; the Alps suffix must be mapped to taper from the datasheet before picking | audio |

Not changed, with reasons: D109 SOD-123F footprint for a SOD-123 1N4148W (pads fit, common practice); 1/4" line headroom 4.4 dB at +4 dBu (pro line is not a use case; SM58 and instrument are); second BAT54S pair on IN_P/IN_N (phantom misuse only; mixer survives at ~7 mA); 1k series on MUX_S0..3 (firmware holds selects low instead); C34 flying cap at 6.1 mm (works); pots on 3V3A vs VREF+ on 3V3 (few % scale error, firmware margin absorbs).

## Firmware before first power-up (spec 2026-10-08 step 0)

1. SystemClock_Config override: HSE 25 MHz, PLL1 M5 N192 P2 Q20, USB from PLL1Q, HSI48 off. Without it PLL3 runs from HSI, the SAI clock is 125.8 MHz and audioInit fails its range check (confirmed by two reviewers).
2. Hold MUX_S0..S3 low for 1.1 s after boot. U107 holds 3V3A off up to 1030 ms; a high select pushes ~20 mA through U101's input clamp into the dead rail.
3. USB soft-disconnect until PA9 reads VBUS; self-powered descriptor.
4. Record silicon revision and ROM bootloader ID (0x1FF1E7FE) on the first board.

## Residual risks only a board proves

1. Crystal start-up margin: gm_crit 1.11 mA/V at C0 = 3 pF against the H7's 1.5 mA/V limit; margin gone at C0 >= 5 pF. Same crystal as the fxbox reference core. Check C0 in the YXC X322525MOB4SI datasheet; measure start-up on the first board.
2. Input current ~0.2 A at 9.6 V estimated. Older Boss PSA units are rated 200 mA, current PSA-120S2 500 mA. Measure.
3. ROM DFU entry with BOOT0 + RESET and from the menu: test on the first board.
4. JLC DFM: J1 plated slots 0.6 mm wide, via drill 0.3 at the limit, THT assembly availability for 27 top-side parts (17 pots, 9 toggles, encoder).

## Stock and quantity

- RK09D1130C2P (C361173): 149 in stock, 17 per board -> 8 boards. JLC minimum order 15.
- NCJ6FA-H (C368458): 47 in stock, owner-soldered; buy now.
- 42 basic, 1 preferred, 33 extended (30 placed by JLC, fee each).
- J101, J106, J108 are on B.Cu, excluded from BOM/CPL, owner-soldered (RUNLOG 2026-10-06).

## Production files

None exist. `hardware/exports/cubevox-bom.csv` and `cubevox-cpl.csv` are stale (224 rows vs 214 placements, list the three owner jacks). Do not upload. Generate after the pass:

```
kicad-cli pcb export gerbers --board-plot-params -o OUT/ hardware/cubevox.kicad_pcb
kicad-cli pcb export drill --format excellon --generate-map -o OUT/ hardware/cubevox.kicad_pcb
kicad-cli pcb export pos --format csv --units mm --side front --exclude-dnp -o OUT/cpl.csv hardware/cubevox.kicad_pcb
```
plus a BOM script grouping by LCSC and skipping BOM-excluded footprints.
