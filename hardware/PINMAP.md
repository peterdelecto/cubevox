# cubevox STM32H743VIT6 pin map and schematic structure

Source of used pins: `h7core_block.json` U1 rows, names cross-checked against
`cubevox_h7core.kicad_sym`. AF numbers come from STM32duino
`PeripheralPins_WeActMiniH7xx.c` (H743 V-I package set, local Arduino15 install).
Verified against DS12110 Table 10 and RM0433 (Codex, 2026-10-03): PE2-PE6 are SAI1
MCLK_A, SD_B, FS_A, SCK_A, SD_A on AF6; PB10/PB11 are I2C2 SCL/SDA on AF4; at reset PB4
has a pull-up and PB5 no pull. On this board (U1 at 96, 84, rotation 0) pins 1-25 face
south, 26-50 east, 51-75 north and 76-100 west, measured from the pad positions.

## 1. Fixed pins

These nets are tied to a peripheral or to parts already placed at the pin.

| Pin | Port | Net | Why fixed |
|---|---|---|---|
| 1 | PE2 | ADC_SCKI_SRC | SAI1_MCLK_A AF6 (section 5) |
| 2-5 | PE3-PE6 | ADC_DOUT, I2S_LRCLK, I2S_BCLK_SRC, I2S_DATA | SAI1 AF6 |
| 12/13 | PH0/PH1 | HSE_IN/OUT | 25 MHz crystal |
| 14 | NRST | NRST | reset |
| 15 | PC0 | MUX1_SIG_ADC (POT_MUX_OUT) | ADC1/2/3_INP10 |
| 63 | PC6 | MUTE_SW (was FX_ON_SENSE, 2026-10-07) | R120/C162 sit at the pin (D0169) |
| 68 | PA9 | VBUS_SENSE | R6/R7 divider, native sensing off (AN4879) |
| 70/71 | PA11/PA12 | USB_DM/DP | OTG_FS |
| 72, 76, 89 | PA13, PA14, PB3 | SWDIO, SWCLK, SWO | SWD/SWO |
| 94 | BOOT0 | BOOT0 | DFU button |

## 2. Pin study (2026-10-03, D0186 and D0194)

Every movable net leaves the U1 edge that faces its destination. Along an edge the pins
follow the order the nets fan out in, so neighbours never cross on F.Cu. The render is
`renders/cubevox-mcu-pinmap.png`.

The board decides the ports:
1. South edge. The SAI stubs, the crystal and the VDDA cluster wall it, so only its fixed nets use it.
2. West edge. SWCLK (76) and SWO (89) both run into J3's east pad column, which traps pins
   77-88. Those stay NC. Pins 90-98 run west under J3 and R3/D1, then south through the open gap west of the crystal.
3. East edge. It opens onto the OLED shadow. The codec band (y 93-107, x 88-170) blocks the
   south, so south-panel nets turn down the channel at x 108-112 between U1's decoupling and U8.
   North-east nets run to the OLED and ENC101.
4. North edge. The decoupling row C6/C31/C59/C32 walls pins 51-62, so it carries only MUTE_SW (the former FX_ON_SENSE), VBUS, USB and SWDIO.

### East edge, pin 47 (north) to 28 (south)

| Pin | Port | Net | Note |
|---|---|---|---|
| 47 | PB11 | I2C_SDA | I2C2_SDA AF4 |
| 46 | PB10 | I2C_SCL | I2C2_SCL AF4 |
| 45 | PE15 | ENC_MENU_A | EXTI15 |
| 44 | PE14 | ENC_MENU_B | EXTI14 |
| 43 | PE13 | ENC_MENU_SW | EXTI13 |
| 42 | PE12 | MUX_S0 | |
| 41 | PE11 | spare | |
| 40 | PE10 | MUX_S1 | |
| 39 | PE9 | MUX_S3 | |
| 38 | PE8 | MUX_S2 | |
| 37 | PE7 | TOGGLE7 | |
| 36 | PB2 | TOGGLE5 | |
| 35 | PB1 | VA_SENSE | ADC12_INP5 |
| 34 | PB0 | TOGGLE3 | |
| 33 | PC5 | XSMT | output, R16 pull-down |
| 32 | PC4 | TOGGLE2 | |
| 31 | PA7 | TOGGLE1 | |
| 30 | PA6 | TOGGLE4 | |
| 29 | PA5 | TOGGLE6 | |
| 28 | PA4 | TOGGLE8 | |

### West edge, pin 90 (north) to 98 (south)

| Pin | Port | Net | Note |
|---|---|---|---|
| 90 | PB4 | spare | NJTRST pull-up at reset; left unconnected |
| 91 | PB5 | MUTE_N | |
| 92 | PB6 | spare (was TOGGLE1, moved east 2026-10-08) | |
| 93 | PB7 | USER_LED | output, R3/D1 west of the pin; dark until init |
| 94 | BOOT0 | BOOT0 | fixed |
| 95 | PB8 | spare (was ENC_KEY_A) | |
| 96 | PB9 | spare (was ENC_SEMI_B) | |
| 97 | PE0 | spare (was ENC_SEMI_A) | |
| 98 | PE1 | spare (was TOGGLE2, moved east 2026-10-08) | |

### Why this order

1. The I2C pair reaches the OLED from the north, SDA passing R142 on its way to R143, so SDA is the north pin. I2C2 is the only I2C on that corner.
2. MENU_A and MENU_B approach R127/R128 from the north, A outermost. MENU_SW runs flat east to R129.
3. TOGGLE bus (2026-10-08, layout/TOGGLE_BUS_BRIEF.md). Pins 28-34, 36, 37 carry XSMT and the
   eight toggles south on F.Cu lanes x 110.1-113.3, the order set by where each lane lands:
   TOGGLE8/6/4 to the west via comb, TOGGLE1/2 on west to SW101/SW102, XSMT to its B.Cu run
   at y 106.88, TOGGLE3/5/7 to the east comb. The MUX selects take the free north rows
   38-40, 42 and drop to B.Cu east of the lanes.
4. ADC pins exist only at 28-35 on this edge, so VA_SENSE sits at 35. It leaves inward through one via and runs B.Cu north of the 3V3 island and back east. Its divider R151/R152 moved beside U106's 5VA pin so the run is flat east. C163 sits at the MCU end, near (117.5, 73.5) on the B.Cu run, as the ADC's local reservoir (D0188-D0190).
5. The west edge carries only MUTE_N, BOOT0 and USER_LED (pin 93, straight west to R3 and D1).
6. BOOT0 (94) sits inside the west group and NRST (14) wraps the south-west corner, so both reach SWBOOT1, SWRESET1 and J3 on B.Cu.
7. MUTE_N avoids PB4 and PA15. Their JTAG pull-ups are on at reset and would turn Q101 on, unmuting the output before firmware runs.
8. CC1/CC2 are off PA0/PA3. USB-C carries data only (spec item 12), and R4/R5 make it a sink without the MCU.

EXTI lines in use: 13, 14, 15 (MENU encoder). The toggles are polled, so TOGGLE8/TOGGLE2 (PA4/PC4) and TOGGLE1/TOGGLE7 (PA7/PE7) sharing lines 4 and 7 costs nothing.

Free pins: 7-9 (PC13-PC15), 16-18 (PC1, PC2_C, PC3_C), 22-25 (PA0-PA3), 41 (PE11), 92 (PB6), 95-98 (PB8, PB9, PE0, PE1),
51-62 (PB12-PB15, PD8-PD15), 64-67 (PC7-PC9, PA8), 69 (PA10), 77-88 (PA15-PD7, the SWD pocket).

### Firmware changes from the previous map

| Net | Was | Now |
|---|---|---|
| ENC_MENU_A / B / SW | PB6 / PE11 / PD15 | PE15 / PE14 / PE13 |
| ENC_KEY_A / B | PB12 / PB13 | removed 2026-10-07 (KEY is pot POT15, now on mux I12) |
| ENC_SEMI_A / B | PB14 / PD10 | removed 2026-10-07 (SEMITONES is pot POT16 on mux I15) |
| FX_ON_SENSE | PC6 | MUTE_SW on PC6 (K101 removed) |
| TOGGLE1-8 | PA4, PA5, PA6, PA7, PB0, PB1, PD11, PB10 | PA7, PC4, PB0, PA6, PB2, PA5, PE7, PA4 |
| MUX_S0-S3 | PD3, PD4, PD6, PB4 | PE12, PE10, PE8, PE9 |
| XSMT | PE15 | PC5 |
| VA_SENSE | PC4 (ADC12_INP4) | PB1 (ADC12_INP5) |
| MUTE_N | PD12 | PB5 |
| USER_LED | PD7 | PB7 |
| OLED I2C | I2C1 PB8/PB9 (I2C1_SCL/SDA) | I2C2 PB10/PB11 (I2C_SCL/SDA) |
| CC1 / CC2 | PA0 / PA3 | not connected |

### U101 mux channels (CD74HC4067, 2026-10-08)

POT01-08 sit on I0-I7 (pins 9 to 2). The west column is re-mapped so each pin's via column
matches its pot row's order down the band (RUNLOG.md, west POT re-map); firmware
slots.cpp follows this table.

| Pin | Input | Net |
|---|---|---|
| 23 | I8 | POT13 |
| 22 | I9 | POT11 |
| 21 | I10 | POT14 |
| 20 | I11 | POT12 |
| 19 | I12 | POT15 (KEY) |
| 18 | I13 | POT09 |
| 17 | I14 | POT10 |
| 16 | I15 | POT16 (SEMITONES) |

## 3. How fxbox joins top and h7core

fxbox is hierarchical with a real `(sheet ...)` symbol (`fxbox.kicad_sch` line 18111,
Sheetname h7core_block, Sheetfile h7core_block.kicad_sch). The file has 0 `global_label`
rows in either sheet. Each child stub that names a port becomes a `hierarchical_label`;
the top stubs each sheet pin outward and ends it in a plain net label, so the label text
joins the net. `cubevox.json` is exactly this fxbox top: `sheets` with one row, `pins`
with offset/len, and every other part drawn on the top.

schematic.py docstring, lines 39-52:

    Hierarchical mode: a spec with a "ports" list is a child sheet -- every stub naming a
    port becomes a `hierarchical_label` ... and every port must be used or the spec
    is refused. A spec with a top-level "sheets" list is a hierarchical top ...
    A pin naming a port its child lacks, a child port with no top pin, or
    a pin whose net nothing else on the top already drives, is refused, naming the port.
    A pin row with `"no_connect": true` gets its sheet pin and a `no_connect` at the pin ...

B96 (`QUEUE.md` line 70, still open): `driven_nets` is computed once from the top body, so
one child cannot connect to a second child. A second sub-sheet is not buildable.

Recommendation: option (c), top plus one h7core child, which is what the tool supports.
1. Draw preamp, relay, output buffer, power input, mux and pots, encoders, toggles and OLED
   header as parts on the cubevox top spec. They drive the nets the sheet pins need.
2. h7core_block.json is cubevox's own copy. Add a port per new net (TOGGLE1..8,
   MUTE_SW; ENC_KEY/ENC_SEMI and JACK_TRS_N are gone) and a matching U1 pin row.
   Add a `sheets[0].pins` row on the top for every added port, and grow `size`
   (it is `[45, 102]`, last offset 74; each new pin is 2 grid units).
3. No global labels and no flat single sheet. A flat sheet works but throws away the
   existing 150-part core and its ERC history.

## 4. Deletions and joins on the cubevox top

Delete from `cubevox.json` (refs in the current file):
1. Bridge and surge: Q102, Q103 (AO4606), R115-R118, D106-D109 (BZT52C15), D112-D115 (SMBJ70CA),
   C116 (PE_BST/PE_SW), C112/C113 rewired to 9V_IN. Keep J106 PJ-002A and D105 SMBJ15CA
   (rewire BAR_A/BAR_B to the item-11 chain: SS34 then 9V_IN).
2. Jacks: J102, J103 (PJ-325M), J104, J105 (PJ-611E), R101-R103, R119-R122, D110, D111.
3. Controls and LEDs: SW101-SW103 (B3F-4055), D101-D103 (WS2812B), U102, R109, R110,
   RV101-RV105 (RK09K; replace with RK09D1130C1B (C470304) x15 and set the mux inputs), C106-C110.
4. Relay driver: Q101, R111, R112, NT101. K101 and D104 removed 2026-10-07 (bypass became
   MUTE; K102 mutes). R111-R114 are the difference-amp resistors and stay.
5. J101 7-pin SPI OLED becomes a 4-pin I2C header (GND, 3V3, SCL, SDA, matches the module order). R107/R108 stay for ENC.
Inside `h7core_block.json` (the child holds these, not the top): U12 TLV9062 and R34/R35/R36/
R41/R42/C70-C73 (ADC driver), U10 TPS63070, U11 LP5912 and their passives
(ADC_PRE/ADC_VAUX/ADC_BB_*), U7 TPA6132A2 (Section=hp_out), and the OLED SPI,
MIDI, SYNC, UART8, MUX2-4 and WS2812 ports.

New blocks connect to: 5V (preamp, relay coil, PCM1808 VCC through 10 R, buck
output), 3V3 and 3V3A (pots, mux, pull-ups), GND,
ADC_VINL / ADC_VINR (PCM1808 pins 13 and 14, currently internal to the child, so add ports),
DAC_OUTL (PCM5102A pin 6; the child already exposes LINE_L after a 470 R / 2.2 nF filter,
so decide whether the item-8 coupling cap hangs on LINE_L or on a new DAC_OUTL port).

SGND in fxbox: the TPA6132A2 headphone amp sense pin (U7 pin 15), the sleeve return of
J103/J105 and D114/D115, bonded to GND at the jack sleeve by net tie NT101. Cubevox drops
U7 and every jack that used it, so SGND has no load. Delete the port, the pin row and NT101;
one GND plane, matching the spec and canon 27.

## 5. ADC master clock (I01)

Sources. Net and pin rows come from `h7core_block.json` and the local STM32duino
`PeripheralPins_WeActMiniH7xx.c`, which lists no SAI. The datasheet PDF could
not be fetched (timeout) at first. The SAI1 pins used (PE2-PE6, AF6) are now verified
against DS12110 Table 10; the fallbacks below are still from memory.

### Choice: option (a), SAI1_MCLK_A on PE2

Why. PCM1808 slave mode needs SCKI synchronous with LRCK/BCK (256/384/512 fs). SAI1 block A
generates BCLK, LRCLK and MCLK from one kernel clock and one MCKDIV divider, so all three are
phase-locked by construction. The fxbox design clocked SCKI from PD14 as SAI3_MCLK_B (AF6, PLL3 P), a different SAI block
with its own MCKDIV counter, so its phase against block A's BCLK/LRCLK is not defined.

Alternatives.
1. (b) SAI3_MCLK_B on PD14 (AF6, from memory) needs no pin moves. SAI1 and SAI3 share the
   SAI1/2/3 kernel clock mux, so both would run from PLL3P, but each SAI has its own MCKDIV
   counter, so the phase offset is fixed per start-up and not defined. Keep as a fallback.
   SAI1_MCLK_B (PF7) and SAI2_MCLK pins on PF/PE0 are not usable or not bonded on LQFP-100
   (NOT VERIFIED).
2. (c) PCM1808 as master needs an external 12.288 MHz or 24.576 MHz oscillator for SCKI,
   MD0/MD1 changed to a master-mode strap, and PCM1808 BCK/LRCK wired to both SAI1 blocks
   as slaves, and to the PCM5102A. It also puts a new part and new nets on the board.
   Rejected for cost.

External flash. PE2 was QUADSPI_BK1_IO2 in fxbox. The QSPI flash (U5, IS25LP064A) and its
pull-ups, clock series resistor and decoupling are removed (owner 2026-10-02). Settings live in
internal flash bank 2. The earlier plan to move QSPI to bank 2 pins (PE7-PE10, PC11, PB2 clock)
is superseded.

### Pin changes

| Pin | Port | fxbox net (AF) | cubevox net |
|---|---|---|---|
| 1 | PE2 | QSPI_IO2 (AF9) | ADC_SCKI_SRC, SAI1_MCLK_A (AF6) |
| 36 | PB2 | QSPI_CLK_SRC | TOGGLE5 (section 2) |
| 37 | PE7 | SPARE_PE7 | TOGGLE7 (section 2) |
| 38 | PE8 | unassigned / SYNC / SPARE | MUX_S2 (section 2) |
| 39 | PE9 | unassigned / SYNC / SPARE | MUX_S3 (section 2) |
| 40 | PE10 | unassigned / SYNC / SPARE | MUX_S1 (section 2) |
| 58 | PD11 | QSPI_IO0 (AF9) | free (section 2) |
| 59 | PD12 | QSPI_IO1 (AF9) | free (section 2) |
| 60 | PD13 | QSPI_IO3 (AF9) | free |
| 61 | PD14 | ADC_SCKI_SRC (SAI3_MCLK_B, fxbox) | free |
| 79 | PC11 | unassigned | free (NC) |
| 92 | PB6 | QSPI_NCS (BK1_NCS AF10) | free (NC) |


### PCM1808 straps

Unchanged. MD0/MD1 stay at slave, I2S 24-bit as now. In slave mode the part auto-detects
the 256/384/512 fs ratio from SCKI/LRCK, and 12.288 MHz at 48 kHz is 256 fs.

### Firmware

1. PLL3 feeds the SAI1 kernel clock (SAI1SEL = PLL3P). With HSE 25 MHz use PLL3M = 25,
   PLL3N = 196, PLL3FRACN = 4981, PLL3P = 4 for 49.152 MHz (HAL ratio values, as in audio.cpp; the raw RCC fields DIVN3 and DIVP3 encode N-1 and P-1, so 195 and 3) (NOT VERIFIED, check PLL3 range bits).
2. SAI1 block A is master transmitter (DAC, SD_A on PE6), MCKEN = 1, MCKDIV chosen so MCLK =
   256 fs = 12.288 MHz, 64-bit frames, BCLK = 64 fs. MCLK, FS_A and SCK_A are all outputs of
   this block.
3. SAI1 block B is slave receiver (ADC, SD_B on PE3), SYNCEN = synchronous with block A.
4. Enable block A (starts MCLK) before the PCM1808 leaves power-down or reset, and keep MCLK
   running whenever PCM1808 is powered.
5. Settings are written to internal flash bank 2 while the code runs from bank 1 (dual bank, no stall).
6. MUX_S0-S3 (PE12, PE10, PE8, PE9) stay low, or input with no pull, until LDO_EN has released
   3V3A. U101 (CD74HC4067) is on 3V3A, which U107 holds off until 3V3 is good, and a select
   driven high into the unpowered mux back-powers it through the input clamp. Budget the
   supervisor's maximum release delay, not the 3V3 rise time.
7. MUX_S0-S3 at the lowest GPIO speed setting. They run 50 mm on B.Cu as one bundle.
8. MUX1_SIG_ADC (PC0, ADC1/2/3_INP10) has a 100 R / 1 nF RC at the pin (tau 100 ns). Use a
   sample time of at least 2 us (64.5 cycles at a 25 MHz ADC clock). Wait at least 0.5 ms per
   channel change before the first conversion. The mux output passes R126/C142 (1k/10n, tau 10 us,
   more at mid-travel of a 10k pot) ahead of the R30/C55 (100R/1n) stage.

VERIFIED (DS12110 Table 10): SAI1 on PE2-PE6, AF6, including SAI1_MCLK_A on PE2.
NOT VERIFIED: the unused fallbacks (SAI3_MCLK_B on PD14, PF7, SAI2 pins), PLL3 settings and the MCKDIV value.
