# cubevox STM32H743VIT6 pin map and schematic structure

Source of used pins: `h7core_block.json` U1 rows, names cross-checked against
`cubevox_h7core.kicad_sym`. AF numbers come from STM32duino
`PeripheralPins_WeActMiniH7xx.c` (H743 V-I package set, local Arduino15 install).
DS12110 itself was not fetched. LQFP-100 pins 1-25 face east, 26-50 north,
51-75 west, 76-100 south (Teensy-H7-Port `pin-audit-2026-09-25.md`).

## 1. Pins already used by h7core (do not reuse)

| Pin | Port | Net | Function | AF / note |
|---|---|---|---|---|
| 1 | PE2 | ADC_SCKI_SRC | SAI1_MCLK_A (PCM1808 SCKI, see section 5) | AF6 |
| 2 | PE3 | ADC_DOUT | SAI1_SD_B (PCM1808 data) | AF6 |
| 3 | PE4 | I2S_LRCLK | SAI1_FS_A | AF6 |
| 4 | PE5 | I2S_BCLK_SRC | SAI1_SCK_A (33 R) | AF6 |
| 5 | PE6 | I2S_DATA | SAI1_SD_A (DAC) | AF6 |
| 12/13 | PH0/PH1 | HSE_IN/OUT | 25 MHz crystal | |
| 14 | NRST | NRST | reset, SWD | |
| 15 | PC0 | MUX1_SIG_ADC | ADC1/2/3_INP10 | analog, reused as POT_MUX_OUT |
| 22 | PA0 | CC1 | USB-C CC sense | avoid |
| 25 | PA3 | CC2 | USB-C CC sense | avoid |
| 36 | PB2 | QSPI_CLK_SRC | QUADSPI_CLK (shared by both banks) | AF9 |
| 37-40 | PE7/PE8/PE9/PE10 | QSPI_IO0/IO1/IO2/IO3 | QUADSPI_BK2_IO0..3 | AF10 |
| 58 | PD11 | TOGGLE7 | GPIO (moved from PE8) | |
| 68 | PA9 | VBUS_SENSE | GPIO | |
| 70/71 | PA11/PA12 | USB_DM/DP | OTG_FS | |
| 72 | PA13 | SWDIO | SWD | avoid |
| 76 | PA14 | SWCLK | SWD | avoid |
| 89 | PB3 | SWO | SWO | avoid |
| 79 | PC11 | QSPI_NCS | QUADSPI_BK2_NCS | AF9 |
| 92 | PB6 | ENC_MENU_A | GPIO, EXTI6 (moved from PE9) | |
| 94 | BOOT0 | BOOT0 | DFU button | |
| 88 | PD7 | USER_LED | GPIO | |
| 43, 45 | PE13, PE15 | HP_EN, XSMT | GPIO (XSMT feeds PCM5102A, keep) | |
| 84, 85, 87, 90 | PD3, PD4, PD6, PB4 | MUX_S0..S3 | GPIO | reused as is |
| 95, 96 | PB8, PB9 | I2C1_SCL, SDA | I2C1 | AF4, reused for OLED |
| 92, 41, 62 | PB6, PE11, PD15 | ENC_A, ENC_B, ENC_SW | GPIO | EXTI6, 11, 15; reused for MENU |

fxbox-only pins cubevox frees (all are no_connect or deleted at the top):
PC1, PC4, PC5 (MUX2-4), PC2_C, PC3_C, PE10, PE12 (SYNC), PD8, PD9 (MIDI), PE0, PE1 (UART8),
PD5 (WS2812), PC10, PC12, PD0-2 (SPI OLED), PE14, PA15, PB5 (SPARE_*).
PE7, PE10 and PC11 are now QSPI bank 2 (section 5). PD12, PD13 and PD14 are free.
Keep them unassigned. Keep PE13 (HP_EN) only if the TPA6132A2 stays in the child.

## 2. New assignments (free pins)

EXTI line n is shared across ports, so each EXTI input needs its own pin number.
Lines taken: 6, 11, 15 (MENU encoder). New encoders use 10, 12, 13, 14.

| Net | Pin | Port | Capability | Reason |
|---|---|---|---|---|
| POT_MUX_OUT | 15 | PC0 | ADC1/2/3_INP10 | existing MUX1_SIG_ADC; rename net |
| MUX_S0..S3 | 84, 85, 87, 90 | PD3, PD4, PD6, PB4 | GPIO | existing |
| ENC_MENU_A / B / SW | 92, 41, 62 | PB6, PE11, PD15 | GPIO, EXTI6/11/15 | ENC_A moved PE9 to PB6 (I01) |
| ENC_KEY_A | 51 | PB12 | GPIO, EXTI12 | free, west side |
| ENC_KEY_B | 52 | PB13 | GPIO, EXTI13 | free |
| ENC_SEMI_A | 53 | PB14 | GPIO, EXTI14 | free |
| ENC_SEMI_B | 57 | PD10 | GPIO, EXTI10 | free |
| TOGGLE1 | 28 | PA4 | GPIO | free, north cluster |
| TOGGLE2 | 29 | PA5 | GPIO | free |
| TOGGLE3 | 30 | PA6 | GPIO | free |
| TOGGLE4 | 31 | PA7 | GPIO | free |
| TOGGLE5 | 34 | PB0 | GPIO | free |
| TOGGLE6 | 35 | PB1 | GPIO | free |
| TOGGLE7 | 58 | PD11 | GPIO | moved PE8 to PD11 (I01) |
| TOGGLE8 | 46 | PB10 | GPIO | free |
| (JACK_TRS_N, deleted rev 2) | 47 | PB11 | none | freed: the 1/4 in detect is gone (audit A01), pin is no-connect |
| FX_ON_SENSE | 63 | PC6 | GPIO, 3.2 V in | free |
| OLED_SCL | 95 | PB8 | I2C1_SCL AF4 | existing I2C1_SCL port |
| OLED_SDA | 96 | PB9 | I2C1_SDA AF4 | existing I2C1_SDA port |

Remaining free after this: PD12, PD13, PD14, PA1, PC13-15 (weak drivers), PA2, PB7, PB15, PC7-9, PA8, PA10.
Toggles use PA4-PA7, PB0, PB1 although they are ADC-capable. The pull-ups to 3V3 make them
safe as digital inputs, and 8 contiguous north-side pins keep the routing short.
PB15 is skipped because EXTI15 belongs to PD15. PA10 is OTG_FS_ID and is left alone.
PB8/PB9 are 5 V tolerant FT_fa pins (from memory, DS12110 not read); pull-ups go to 3V3 anyway.

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
2. h7core_block.json is cubevox's own copy. Add a port per new net (ENC_KEY_A/B,
   ENC_SEMI_A/B, TOGGLE1..8, JACK_TRS_N, FX_ON_SENSE) and a matching U1 pin row.
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
4. Relay driver: Q101, R111, R112, NT101; K101 stays but is rewired (coil from 5V through
   the bypass toggle, D104 stays across it). R113/R114 go away.
5. J101 7-pin SPI OLED becomes a 4-pin I2C header (GND, 3V3, SCL, SDA, matches the module order). R107/R108 stay for ENC.
Inside `h7core_block.json` (the child holds these, not the top): U12 TLV9062 and R34/R35/R36/
R41/R42/C70-C73 (ADC driver), U10 TPS63070, U11 LP5912 and their passives
(ADC_PRE/ADC_VAUX/ADC_BB_*), U7 TPA6132A2 (Section=hp_out), and the OLED SPI,
MIDI, SYNC, UART8, MUX2-4 and WS2812 ports.

New blocks connect to: 5V (preamp, relay coil, PCM1808 VCC through 10 R), 5V_PRODUCT
(buck output, TPS2116 input), 3V3 and 3V3A (pots, mux, pull-ups), GND, VBUS (TPS2116 pin 6),
ADC_VINL / ADC_VINR (PCM1808 pins 13 and 14, currently internal to the child, so add ports),
DAC_OUTL (PCM5102A pin 6; the child already exposes LINE_L after a 470 R / 2.2 nF filter,
so decide whether the item-8 coupling cap hangs on LINE_L or on a new DAC_OUTL port).

SGND in fxbox: the TPA6132A2 headphone amp sense pin (U7 pin 15), the sleeve return of
J103/J105 and D114/D115, bonded to GND at the jack sleeve by net tie NT101. Cubevox drops
U7 and every jack that used it, so SGND has no load. Delete the port, the pin row and NT101;
one GND plane, matching the spec and canon 27.

## 5. ADC master clock (I01)

Sources. Net, pin and QUADSPI rows come from `h7core_block.json` and the local STM32duino
`PeripheralPins_WeActMiniH7xx.c`, which lists QUADSPI but no SAI. The datasheet PDF could
not be fetched (timeout), so every SAI alternate function below is from memory and is
NOT VERIFIED against DS12110.

### Choice: option (a), SAI1_MCLK_A on PE2, QSPI moves to bank 2

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

QSPI. QUADSPI_BK1_IO2 exists only on PE2 (AF9) in the local file, so QSPI cannot stay on
bank 1 once PE2 is taken. Bank 2 is fully available on LQFP-100: BK2_IO0 PE7, IO1 PE8,
IO2 PE9, IO3 PE10 (all AF10), BK2_NCS PC11 (AF9, only BK2 NCS in the file). QUADSPI_CLK on
PB2 (AF9) is shared by both banks. PB6 (BK1_NCS) cannot select bank 2, so NCS moves to PC11.

### Before / after

| Pin | Port | Old net (AF) | New net (AF) |
|---|---|---|---|
| 1 | PE2 | QSPI_IO2 (AF9) | ADC_SCKI_SRC, SAI1_MCLK_A (AF6) |
| 37 | PE7 | unassigned | QSPI_IO0, BK2_IO0 (AF10) |
| 38 | PE8 | TOGGLE7 (GPIO) | QSPI_IO1, BK2_IO1 (AF10) |
| 39 | PE9 | ENC_MENU_A (GPIO, EXTI9) | QSPI_IO2, BK2_IO2 (AF10) |
| 40 | PE10 | unassigned | QSPI_IO3, BK2_IO3 (AF10) |
| 58 | PD11 | QSPI_IO0 (AF9) | TOGGLE7 (GPIO) |
| 59 | PD12 | QSPI_IO1 (AF9) | unassigned |
| 60 | PD13 | QSPI_IO3 (AF9) | unassigned |
| 61 | PD14 | ADC_SCKI_SRC (SAI3_MCLK_B, fxbox) | unassigned |
| 79 | PC11 | unassigned | QSPI_NCS, BK2_NCS (AF9) |
| 92 | PB6 | QSPI_NCS (BK1_NCS AF10) | ENC_MENU_A (GPIO, EXTI6) |
| 36 | PB2 | QSPI_CLK_SRC (AF9) | unchanged |

ENC_MENU_A takes EXTI6, which no other input uses (taken: 6, 10-15). Schematic work: swap the
U1 pin numbers on the QSPI nets and the SCKI net, keep net names, and rewire the flash pads
to the same net names (IO0..IO3 keep their roles, so the flash symbol does not change).
The old PD11-13 routes to the flash are replaced by PE7-PE10 and PC11.

### PCM1808 straps

Unchanged. MD0/MD1 stay at slave, I2S 24-bit as now. In slave mode the part auto-detects
the 256/384/512 fs ratio from SCKI/LRCK, and 12.288 MHz at 48 kHz is 256 fs.

### Firmware

1. PLL3 feeds the SAI1 kernel clock (SAI1SEL = PLL3P). With HSE 25 MHz use DIVM3 = 25,
   DIVN3 = 196, FRACN3 = 4981, DIVP3 = 4 for 49.152 MHz (NOT VERIFIED, check PLL3 range bits).
2. SAI1 block A is master transmitter (DAC, SD_A on PE6), MCKEN = 1, MCKDIV chosen so MCLK =
   256 fs = 12.288 MHz, 64-bit frames, BCLK = 64 fs. MCLK, FS_A and SCK_A are all outputs of
   this block.
3. SAI1 block B is slave receiver (ADC, SD_B on PE3), SYNCEN = synchronous with block A.
4. Enable block A (starts MCLK) before the PCM1808 leaves power-down or reset, and keep MCLK
   running whenever PCM1808 is powered.
5. QUADSPI runs in single-flash mode with FSEL = 1 (flash 2) on bank 2 pins.

NOT VERIFIED: every AF number in this section that is not a QUADSPI row of the local file
(SAI1_MCLK_A on PE2 AF6, SAI3_MCLK_B on PD14, PF7, SAI2 pins), the PC11-only BK2_NCS claim
against DS12110, the PE2 AF6 and PC11 AF9 assignments (the pin numbers 1, 37-40, 58-61, 79 and 92 were
checked against the pin names in cubevox_h7core.kicad_sym and are correct), PLL3 settings, and the MCKDIV value.
