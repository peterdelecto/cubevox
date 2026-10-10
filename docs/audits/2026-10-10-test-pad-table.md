# Test pad candidates, 2026-10-10

Board: `hardware/cubevox.kicad_pcb` after the pre-fab fix passes. Tool: `hardware/tools/tp_space.py`
(read-only). Pad recipe as TP1 to TP5: `TestPoint:TestPoint_Pad_D1.5mm`, reference visible on F.Fab
at 1.0 mm above the pad, value (net name) hidden.

## 1. Method

1. Nets excluded: GND, VIN_9V, 9V_JACK, 9V_PLUG (owner's list), unconnected pins. 3V3 and VBUS already
   have TP4 and TP5.
2. Origin pad: the net's IC pin first (U), then relay (K), connector (J), crystal, switch, pot, else the
   first passive pad. Where the origin pin has no room, every other pad of the net is tried in that
   order and the row says `yes, at REF.PIN`.
3. A spot passes when the 1.5 mm pad clears every foreign F.Cu pad, track and via by 0.25 mm, its
   1.0 mm radius courtyard clears every other courtyard, rule area and the board edge, and a 0.3 mm
   stub (straight, or one straight plus one 45) from the origin pad centre clears 0.2 mm. Rings of
   0.5 mm out to 8 mm; the nearest ring wins. `stub` is that ring distance.
4. Nothing is placed. The spot is one that fits; a hand placement can usually do better.

## 2. Table

| net | origin | result | spot (x, y) | stub | other ICs on the net |
|---|---|---|---|---|---|
| 3V3 | U1.6 | has TP4 | | | U107 U3 U6 U8 |
| 3V3A | U101.24 | yes, at U4.5 | (150.82, 51.35) | 2.0 mm | U4 U6 |
| 5V | U103.1 | yes | (93.67, 56.00) | 2.0 mm | U3 U4 |
| 5VA | U104.8 | yes | (204.10, 85.02) | 4.0 mm | U105 U108 |
| 5VA_W | U106.8 | yes | (73.55, 85.85) | 4.5 mm |  |
| A1_FB | U104.2 | yes | (214.08, 80.63) | 6.5 mm |  |
| A2_FB | U104.6 | yes | (206.19, 75.61) | 5.0 mm |  |
| ADC_RC | R119.2 | yes | (115.58, 102.08) | 3.0 mm |  |
| ADC_VINL | U8.13 | yes | (115.62, 106.01) | 6.0 mm |  |
| ADC_VINR | U8.14 | no | | |  |
| BIAS_W | U106.3 | yes | (69.91, 88.65) | 3.0 mm |  |
| BUCK5_BST | U103.6 | yes | (91.19, 52.64) | 2.0 mm |  |
| BUCK5_SW | U103.5 | yes | (91.80, 47.41) | 5.0 mm |  |
| BUF_IN | U106.5 | yes | (78.22, 93.50) | 2.0 mm |  |
| BUF_OUT | U106.6 | yes | (80.10, 90.33) | 2.5 mm |  |
| BUF_R | R124.2 | yes | (76.09, 81.92) | 4.0 mm |  |
| DAC_AC | R122.1 | yes | (68.75, 98.40) | 3.0 mm |  |
| DAC_LP | R147.2 | yes | (62.24, 105.02) | 4.0 mm |  |
| DAC_OUTL | U6.6 | yes, at R147.1 | (71.35, 109.81) | 5.0 mm |  |
| DA_N | U105.2 | no | | |  |
| DA_P | U105.3 | yes | (235.00, 91.44) | 5.5 mm |  |
| ENC_MENU_A | U1.45 | no | | |  |
| ENC_MENU_B | U1.44 | yes, at ENC101.B | (172.31, 60.91) | 2.5 mm |  |
| ENC_MENU_SW | U1.43 | no | | |  |
| G2_BOT | RVGAIN1.3 | yes | (236.32, 70.62) | 2.5 mm |  |
| G2_INV | U105.6 | yes | (233.77, 78.52) | 4.0 mm |  |
| G2_RG | R116.2 | yes | (237.00, 75.50) | 2.0 mm |  |
| I2C_SCL | U1.46 | yes, at J117.3 | (136.43, 53.53) | 2.5 mm |  |
| I2C_SDA | U1.47 | yes, at J117.4 | (138.97, 53.58) | 6.0 mm |  |
| INA_O1 | U104.1 | yes | (208.22, 83.30) | 2.0 mm |  |
| INA_O2 | U104.7 | yes, at R113.1 | (225.13, 92.24) | 4.0 mm |  |
| IN_N | U104.5 | yes | (202.53, 75.60) | 2.0 mm |  |
| IN_P | U104.3 | yes | (203.81, 75.61) | 5.0 mm |  |
| LINE_N | K103.5 | yes | (194.47, 77.07) | 2.5 mm |  |
| LINE_P | K103.3 | yes | (193.77, 88.85) | 2.5 mm |  |
| MIC_N | D108.3 | yes | (199.72, 60.54) | 2.5 mm |  |
| MIC_P | D107.3 | yes | (213.72, 60.54) | 2.5 mm |  |
| MUTE_BASE | Q101.1 | yes | (74.25, 79.41) | 2.0 mm |  |
| MUTE_COIL | K102.8 | yes | (73.57, 64.57) | 2.5 mm |  |
| MUTE_N | U1.91 | yes, at R150.1 | (71.62, 84.65) | 2.0 mm |  |
| MUTE_SW | U1.63 | yes | (97.82, 73.46) | 3.5 mm |  |
| MUX_COM | U101.1 | yes | (151.81, 136.07) | 5.0 mm |  |
| MUX_S0 | U1.42 | yes, at U101.10 | (154.01, 124.72) | 2.5 mm | U101 |
| MUX_S1 | U1.40 | yes, at U101.11 | (148.32, 122.22) | 4.0 mm | U101 |
| MUX_S2 | U1.38 | yes, at U101.14 | (142.05, 125.32) | 2.5 mm | U101 |
| MUX_S3 | U1.39 | yes, at U101.13 | (143.59, 122.11) | 2.0 mm | U101 |
| OUT_AC | K102.3 | yes | (75.92, 79.37) | 6.5 mm |  |
| OUT_TIP | K102.4 | yes | (84.42, 72.55) | 3.0 mm |  |
| PAD_BASE | Q102.1 | yes | (188.57, 75.05) | 4.0 mm |  |
| PAD_COIL | K103.8 | yes | (188.37, 77.15) | 2.5 mm |  |
| PAD_EN | U1.41 | yes, at R160.1 | (188.04, 73.00) | 2.0 mm |  |
| POT01 | U101.9 | yes | (154.91, 124.86) | 3.5 mm |  |
| POT02 | U101.8 | yes, at RV102.2 | (225.27, 173.48) | 7.5 mm |  |
| POT03 | U101.7 | yes, at RV103.2 | (65.82, 147.62) | 2.5 mm |  |
| POT04 | U101.6 | yes | (155.53, 128.32) | 4.0 mm |  |
| POT05 | U101.5 | yes | (155.02, 128.99) | 3.5 mm |  |
| POT06 | U101.4 | yes, at RV106.2 | (119.21, 173.49) | 7.0 mm |  |
| POT07 | U101.3 | yes, at RV107.2 | (135.87, 147.92) | 2.5 mm |  |
| POT08 | U101.2 | yes, at RV108.2 | (143.21, 173.49) | 7.0 mm |  |
| POT09 | U101.18 | yes | (142.46, 127.42) | 2.0 mm |  |
| POT10 | U101.17 | yes | (142.46, 126.77) | 2.0 mm |  |
| POT11 | U101.22 | yes | (142.05, 129.03) | 2.5 mm |  |
| POT12 | U101.20 | yes | (142.46, 128.73) | 2.0 mm |  |
| POT13 | U101.23 | yes | (142.25, 129.22) | 2.5 mm |  |
| POT14 | U101.21 | yes | (142.46, 128.88) | 2.0 mm |  |
| POT15 | U101.19 | yes | (142.46, 128.07) | 2.0 mm |  |
| POT16 | U101.16 | yes | (142.25, 127.08) | 2.5 mm |  |
| POT_MUX_OUT | R126.2 | yes | (148.44, 134.68) | 2.0 mm |  |
| PRE1 | U105.1 | yes | (227.24, 87.84) | 2.0 mm |  |
| PRE_OUT | U105.7 | yes | (228.53, 76.72) | 5.5 mm |  |
| RG_MID | R109.1 | yes | (199.38, 89.00) | 2.0 mm |  |
| TOGGLE1 | U1.31 | yes, at SW101.2 | (71.50, 115.00) | 7.0 mm |  |
| TOGGLE2 | U1.32 | yes, at SW102.2 | (95.50, 115.00) | 7.0 mm |  |
| TOGGLE3 | U1.34 | yes, at SW103.2 | (119.50, 115.00) | 7.0 mm |  |
| TOGGLE4 | U1.30 | yes, at SW104.2 | (129.72, 116.75) | 7.0 mm |  |
| TOGGLE5 | U1.36 | yes, at SW105.2 | (167.50, 115.00) | 7.0 mm |  |
| TOGGLE6 | U1.29 | yes, at SW106.2 | (191.50, 115.00) | 7.0 mm |  |
| TOGGLE7 | U1.37 | yes, at SW107.2 | (215.50, 115.00) | 7.0 mm |  |
| TOGGLE8 | U1.28 | yes, at SW108.2 | (239.50, 115.00) | 7.0 mm |  |
| TRS_R | J101.5 | yes | (172.01, 54.19) | 1.0 mm |  |
| TRS_T | J101.6 | yes | (179.81, 46.33) | 1.0 mm |  |
| U106A_OUT | U106.1 | yes | (70.01, 88.55) | 2.5 mm |  |
| VA_SENSE | U1.35 | yes, at R151.2 | (155.04, 82.34) | 2.0 mm |  |
| VREF | R103.2 | yes | (214.00, 76.54) | 2.0 mm |  |
| VREF_BUF | U108.1 | yes | (157.63, 88.30) | 2.0 mm |  |
| VREF_DIV | U108.3 | yes | (160.23, 92.35) | 2.0 mm |  |
| XLR_2 | J101.2 | yes | (168.38, 49.51) | 1.0 mm |  |
| XLR_3 | J101.3 | yes | (175.23, 54.45) | 1.0 mm |  |
| ADC_DOUT | U1.2 | no | | | U8 |
| ADC_SCKI | U8.6 | yes | (121.66, 97.22) | 6.5 mm |  |
| ADC_SCKI_SRC | U1.1 | yes | (86.56, 95.17) | 5.0 mm |  |
| ADC_VCC | U8.3 | yes, at R20.2 | (137.65, 98.83) | 2.0 mm |  |
| ADC_VREF | U8.1 | yes | (127.42, 106.35) | 2.0 mm |  |
| BOOT0 | U1.94 | yes, at SWBOOT1.1 | (58.13, 92.35) | 2.0 mm |  |
| BUCK_BST | U3.6 | yes | (98.19, 66.69) | 5.0 mm |  |
| BUCK_SW | U3.5 | yes | (98.03, 66.88) | 5.5 mm |  |
| CAPM | U6.4 | no | | |  |
| CAPP | U6.2 | yes | (69.27, 102.28) | 6.5 mm |  |
| CC1 | J1.A5 | yes, at R4.1 | (116.98, 42.99) | 3.5 mm |  |
| CC2 | J1.B5 | no | | |  |
| HSE_IN | U1.12 | yes, at Y1.1 | (96.92, 95.05) | 3.0 mm |  |
| HSE_OUT | U1.13 | yes, at R1.1 | (97.14, 94.91) | 2.0 mm |  |
| HSE_XTAL_OUT | Y1.3 | yes | (97.18, 101.80) | 2.0 mm |  |
| I2S_BCLK | U6.13 | yes, at U8.8 | (120.83, 98.59) | 2.0 mm | U8 |
| I2S_BCLK_SRC | U1.4 | no | | |  |
| I2S_DATA | U6.14 | yes, at R157.2 | (85.75, 95.08) | 1.5 mm |  |
| I2S_DATA_SRC | U1.5 | yes, at R157.1 | (86.25, 95.08) | 1.5 mm |  |
| I2S_LRCLK | U6.15 | yes, at U8.7 | (126.43, 98.59) | 2.0 mm | U8 |
| I2S_LRCLK_SRC | U1.3 | no | | |  |
| LDOO | U6.18 | yes, at C33.1 | (85.85, 102.20) | 2.5 mm |  |
| LDO_EN | U107.2 | yes | (157.55, 51.48) | 4.0 mm | U4 |
| MUX1_SIG_ADC | U1.15 | yes, at R30.2 | (114.58, 91.93) | 2.5 mm |  |
| NRST | U1.14 | yes, at SWRESET1.1 | (58.13, 84.15) | 2.0 mm |  |
| PWR_LED_A | D3.1 | yes | (110.30, 75.32) | 2.0 mm |  |
| SWCLK | U1.76 | yes | (85.96, 76.32) | 3.0 mm |  |
| SWDIO | U1.72 | yes, at J3.2 | (86.02, 76.25) | 3.5 mm |  |
| SWO | U1.89 | no | | |  |
| USB_DM | U1.70 | yes, at J1.B7 | (116.38, 42.59) | 6.5 mm |  |
| USB_DP | U1.71 | yes, at D2.1 | (112.07, 43.06) | 3.0 mm |  |
| USER_LED | U1.93 | yes, at R3.1 | (82.39, 90.34) | 5.5 mm |  |
| USER_LED_A | D1.1 | yes | (84.65, 88.31) | 2.5 mm |  |
| VBUS | J1.A4B9 | has TP5 | | |  |
| VBUS_SENSE | U1.68 | yes, at R6.2 | (121.18, 43.01) | 2.0 mm |  |
| VCAP1 | U1.48 | yes | (110.26, 77.01) | 7.0 mm |  |
| VCAP2 | U1.73 | no | | |  |
| VDDA | U1.20 | yes, at FB1.2 | (104.81, 94.83) | 4.5 mm |  |
| VNEG | U6.5 | no | | |  |
| XSMT | U1.33 | yes, at R16.1 | (98.29, 107.90) | 2.0 mm | U6 |

yes 114, no 12, with a pad already 2

## 3. The twelve without room

ADC_VINR, DA_N, ENC_MENU_A, ENC_MENU_SW, ADC_DOUT, CAPM, CC2, I2S_BCLK_SRC, I2S_LRCLK_SRC, SWO,
VCAP2, VNEG. All sit on 0.5 or 0.65 mm pitch IC pins (U1, U6, U8, U105, J1) whose escapes fill the
8 mm around them, and the nets have no other pad. A pad for any of these means a stub off the trace
in the open, which the tool does not search.
