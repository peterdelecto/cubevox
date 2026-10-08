# cubevox schematic audit, round 2

Scope: the schematic as it stands after revision 2 of the analog chain and
the removal of the QSPI flash. Nothing is routed. Findings cost a
regenerate now and a re-route later, so this round runs before placement.

## Attach

1. `hardware/renders/cubevox.pdf` (all sheets)
2. `hardware/PARTS.md`
3. `hardware/PINMAP.md`
4. `docs/specs/2026-10-02-hardware-design.md`
5. `hardware/SCHEMATIC-AUDIT.md` and `hardware/SCHEMATIC-AUDIT-RESPONSE.md`
   (round 1 and what was done about it)

## Questions

### A. Analog chain, with the revision 2 values

1. OPA2197 at 5 V single supply, VREF 2.5 V. Input common-mode and output
   swing at every stage with an SM58 (-54 dBV nominal, 0 dBu peaks) and a
   pedal at +4 dBu through the 19.4 dB pad. Where does it clip first?
2. Stage 1 INA: Rf 9.53 k, RG 1 k + 22 uF, diff amp 4 x 10 k 0.1 %. CMRR
   at 50/60 Hz and 1 kHz including the 1.2 k bias legs and the 10 uF
   electrolytic coupling caps. Is the 7 Hz corner from RG + 22 uF a
   settling or pop problem at power-up?
3. Stage 2: pot as feedback over R116 200 R + C159 100 uF. Load on the
   OPA2197 output at minimum gain, stability with C159, and gain range
   against ADC full scale.
4. VREF: divider, follower, R146 22 R, C160 10 uF electrolytic, no per-load
   caps. Stability margin and noise contribution. Does any stage still
   return signal current into VREF?
5. Electrolytic polarity at C120, C121, C124, C125, C139, C141, C158,
   C159, C160 given the DC levels in the response document.
6. DAC filter 470 R + 2.2 nF + 10 uF + 10 k/15 k to VREF, output buffer
   unity, 100 R, 10 uF, 100 k to the NMJ6HCD2. Output impedance and level
   for INSTRUMENT and LINE.
7. Relay G6K-2F-Y with both poles paralleled, coil driven by the panel
   toggle, FX_ON_SENSE 10 k/18 k. Pop on switching and contact rating at
   line level.

### B. Clocks

8. PE2 AF6 = SAI1_MCLK_A and the PLL3 numbers (DIVM 25, DIVN 196,
   FRACN 4981, DIVP 4, 49.152 MHz, MCLK 256 fs at 48 kHz). Confirm against
   the STM32H743 datasheet and reference manual.
9. SAI1 block A master with the PCM1808 as slave on SCKI from PE2 and the
   PCM5102A on the same BCLK/LRCLK. Any jitter or phase concern.
10. Is the 25 MHz crystal plus PLL arrangement enough, or does the PCM1808
    want its own oscillator?

### C. Power and grounding, one board

11. AP63205 5 V buck feeding the analog rail through a ferrite, AP63203
    3V3 buck for digital, TLV75533 3V3A LDO. Loop areas, the ferrite's
    resonance with its caps, and whether the preamp sees the buck ripple.
12. The TPS2116 USB/9 V mux is removed (owner 2026-10-02). The box runs
    from 9 V only; USB-C is data only, VBUS goes to the ESD part and
    nothing else. Anything the USB stack or DFU entry loses without a
    VBUS-powered mode? Brownout and power-sequencing of the 5 V / 3V3 /
    3V3A rails at plug-in and at adapter pull.
13. Grounding plan for a single ground plane carrying a buck, an MCU, a
    codec and a 60 dB mic preamp on a 200 x 150 mm board with the mic jack
    at one rear corner. Where should the analog section sit relative to
    the buck and the MCU?
14. Settings in internal flash bank 2 while running from bank 1 (dual
    bank, no stall). Confirm against the reference manual.

### D. Removed parts

15. QSPI flash removed. Anything that depended on it?
16. PINMAP pins 36 to 40 and 79 freed. Any better use for them?

## Not in this round

PCB review (mic symmetry, USB pair, crystal, SAI clocks, relay contacts)
and the JLC export wait for routing.
