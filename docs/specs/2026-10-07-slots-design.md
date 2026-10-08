# Slots design (Adam, 2026-10-07)

## Plan

The panel is eight effect slots. Each slot is two knobs and a bypass toggle. The slot is the fixed hardware unit; the effect inside it, its knob meanings and the chain order are firmware and may change with updates. (Superseded 2026-10-08: Adam places effects from the Mac app and the chain is the slot order, see `2026-10-08-update-and-slots-design.md`.) The top-left toggle is a global vocal MUTE. INTENSITY is relabelled MIX. Input Gain stays an analog face knob outside the slot grid.

Today's chain, left to right, slot 1 to 8:

| Slot | Effect | Knob A | Knob B |
|---|---|---|---|
| 1 | Input gate | Threshold | Decay |
| 2 | Autotune | Key | Response |
| 3 | Octave | Semitones | Mix |
| 4 | Unison | Depth | Rate |
| 5 | Slapback | Mix | Time |
| 6 | Distortion | Drive | Tone |
| 7 | Gate | Threshold | Decay |
| 8 | Reverb | Mix | Time |

## Open questions resolved

1. Mux capacity. U101 (CD74HC4067) carries POT01..POT14 on I0..I13; I14 and I15 (pins 16, 17) are grounded spares. KEY and SEMITONES move from encoders to pots on those two inputs. 16 slot knobs fill the 16 channels. RVGAIN1 is analog in the preamp gain leg and never touches the mux. No second mux, no direct ADC pin.
2. Relays. K101 (bypass, dry path PRE_OUT to BUF_IN) leaves with the bypass, with D104 and the dry path. K102 (output mute, MUTE_N on PB5, tip grounded while off and at reset) stays and is the mute.
3. Mute toggle wiring. Same ST-0-102 part. Common to MUTE_SW on PC6 (the former FX_ON_SENSE, R120 10 k pull-up and C162 100 nF kept), one throw to GND. Firmware soft-mutes the DAC (XSMT), then drops MUTE_N; reverse on release. No MCU-free path is needed because the relay fails to muted.
4. Input Gain. Stays analog and on the face. A menu item cannot turn the pot, and gain ahead of the ADC keeps quiet singing clear of the noise floor.
5. Knob type. All 16 slot knobs are RK09D1130C2P continuous pots. Firmware snaps KEY to its positions and SEMITONES to -24..+24 with hysteresis; the OLED shows the landed value. The detented-knob question is closed.
6. Chain order. Left to right is the chain by default. Firmware may reorder; the OLED menu shows the live order. (Superseded 2026-10-08: the chain is always the slot order.)
7. Firmware update. OLED menu "Update firmware" reboots into the STM32 ROM DFU bootloader over the rear-wall USB-C. The BOOT0 button stays inside as recovery. Settings in flash bank 2 survive because the DFU image excludes it. (2026-10-08: the Mac app's Update screen does the same over the cable; the menu item stays as the fallback.)
8. Mute scope. Output only. The chain keeps running, the OLED shows MUTED, unmute is seamless.
9. Freed GPIOs PB7, PB8, PB9, PE0. Unconnected. No pads, no header.
10. Lid legends. Deferred. Not needed until the enclosure.
11. Emulator. BYPASS becomes MUTE now: output silent, chain keeps running, red while muted.

## Decisions

1. 8 slots x (2 pots + 1 toggle); slot = hardware, effect = firmware.
2. 16 identical pots on the mux; 17 pots per unit with Input Gain.
3. One encoder remains (MENU, EC11E15244B2). EC11E15204A3 is removed from the BOM.
4. One relay remains (K102). K101, D104 and the dry path are removed.
5. PC6 = MUTE_SW. PB7, PB8, PB9, PE0 spare.
6. INTENSITY is MIX everywhere on the face (done, v2.00.05).
7. DFU is entered from the menu.

## Recommended next steps

1. Commit the relabel already in the working tree (CLAUDE.md line 34 needs its glued newline fixed first).
2. Update the stale docs listed in the interview (CLAUDE.md panel, hardware spec items 7, 10, 19, 20, 23 and tables, PINMAP, PARTS, face spec item 9).
3. Schematic: ENC102/ENC103 to RV115/RV116 on I14/I15; delete ENC_KEY/ENC_SEMI nets and ports; delete K101/D104; DAC_ATT to BUF_IN; FX_ON_SENSE to MUTE_SW. ERC.
4. PCB: pots at the encoder positions (64.5, 164) and (88.5, 164), K101 area cleared. Placement render, then stop before routing (owner rule).
5. Emulator: BYPASS button to MUTE.
6. Firmware (later): per-slot pot-to-parameter map, knob-moved OLED readout, KEY/SEMITONES hysteresis, menu DFU entry, MUTE_SW handling.
