# cubevox — input conditioning and the input gate revision

Owner spec rev 0.1 of 2026-10-30, amended the same day (the amendments are marked). Target: the
H7 firmware, 48 kHz, 64-sample blocks, float32.

## Scope
A fixed input conditioning block, a high-pass and a low-pass, ahead of the eight slots, and a
tighter slot-1 Input gate. Purpose: less hiss and less low-frequency noise reaching the effects
and the output in a loud room. The analog floor (preamp, reference, output pad) is a hardware
change, listed at the end and out of scope here.

## Chain position
```
ADC → input tap (the meter, as today) → HPF → LPF → slot 1 Input gate (detector on the filtered
signal) → slots 2 to 8 → POLISH COMP → POLISH EQ → output trim → soft ceiling → output tap → DAC
```
The block has no toggle; each filter has an OFF entry.

## High-pass
| Item | Value |
|---|---|
| Topology | 2nd-order Butterworth, one biquad, Q 0.7071 |
| Cutoffs | OFF, 80, 100, 120, 150 Hz |
| Default | 120 Hz |
| Response | -3.0 ± 0.3 dB at fc; at most -11 dB at fc/2 |

## Low-pass
| Item | Value |
|---|---|
| Topology | 4th-order Butterworth, two biquads, Q 0.5412 and 1.3066 |
| Cutoffs | OFF, 4, 5, 6, 8, 10, 12 kHz |
| Default | 12 kHz (amended; the owner's draft said 8 kHz) |
| Response | -3.0 ± 0.3 dB at fc; at most -23 dB at 2 fc; ripple under 0.1 dB below fc/2 |

The default moved to 12 kHz because the hiss the ear objects to sits in 2 to 6 kHz, which an
8 kHz low-pass never touches, while a voice's air and sibilance above 8 kHz would go. The lower
cutoffs stay as the noisy-room options. Broadband hiss removed against OFF, flat noise to 24 kHz:
4 kHz 7.8 dB, 5 kHz 6.8 dB, 6 kHz 6.0 dB, 8 kHz 4.8 dB, 10 kHz 3.8 dB, 12 kHz 3.0 dB.

## Biquads
1. Transposed direct form II, float32, per sample over the block; `engine/dsp.h` Biquad with its
   RBJ setters.
2. Coefficients computed on a setting change in the main loop, adopted at a block edge, as the
   other settings adopt.
3. A cutoff change crossfades the old and new filter over one 64-sample block (amended; the draft
   accepted a click). State is not reset.
4. Flush-to-zero set in FPSCR at startup; no software denormal guard. The application does not set
   it today.
5. Cost: three biquads, about 15 flops a sample, under 1 % of a block. Zero added latency.

## Input gate, slot 1
| Item | Value |
|---|---|
| Type | Downward expander, hard range, full band |
| Detector | Peak, 0.1 ms attack, 5 ms release, on the conditioned signal |
| THRESHOLD pot | -80 to -30 dBFS, shown in whole dB |
| Hysteresis | 3 dB: opens at the threshold, closes 3 dB under it |
| Attack | 0.5 ms, the time the gain takes from -80 dB to within 1 dB of open (as built; see below) |
| Hold | 50 ms after the detector falls under the close level |
| DECAY pot | the release time, 20 ms to 2 s, log, the time the gain takes to fall 60 dB (amended; the draft fixed 150 ms) |
| Range | -80 dB, fixed |
| Lookahead | none; zero latency |

The release went onto the DECAY pot because slot 1 has two pots and a fixed release would leave
one doing nothing. The panel GATE card in slot 7 keeps its soft-knee tuning; both are instances of
the Gate class and the revision is the input gate's tuning.

## LEARN (removed)
There is no LEARN. The draft had a menu action that measured the floor for 2 s and set the
threshold from it; the first build (v2.00.45) showed the floor beside the pot's threshold and
wrote nothing; the owner removed it the same day (2026-10-30, "i dont want gate learn"). The
THRESHOLD pot is absolute and the player sets it by ear.

The floor measurement stays as a field of the serial status reply, `floor_dbfs`, the highest
detector level since the previous status, so the bench tests below can read it.

## Menu and settings
Two rows join the list, which scrolls: `HPF  120 Hz` and `LPF  12 kHz` after DEBUG, before UPDATE
FIRMWARE, the value with its unit (owner 2026-10-30). HPF and LPF take settings bytes 6 and 7 of the layout record,
schema unchanged: 0 means not set and reads as the default, otherwise the byte is the cutoff
list index plus one (HPF list OFF, 80, 100, 120, 150; LPF list OFF, 4k, 5k, 6k, 8k, 10k, 12k),
and a byte past the list makes the record invalid as the other settings bytes do. They are
global, not per slot.

## Acceptance
Host, in ctest: 1 frequency response at every cutoff with sine probes at fc, fc/2, 2 fc and
1 kHz (passband 0 ± 0.1 dB); 3 gate timing on a 1 kHz burst at threshold + 10 dB (within 1 dB of
final inside 1 ms; under -60 dB within hold + release + 20 ms); 4 gate stability on noise at
threshold ± 1.5 dB for 10 s (no transitions) and at + 2 dB (open and stays open); 6 cost in
`costs.h` within 1 point; 7 crossfade on a cutoff change at -20 dBFS 1 kHz leaves transients
under -40 dBFS.
Bench, all slots bypassed, COMP and EQ off, LINE, Input Gain max, XLR pins 2 and 3 bridged with
150 Ω, MIC: 2 noise at the output tap with and without HPF 120 and LPF 12k, from `floor_dbfs`
(4.5 dB unweighted expected at LPF 8k; record the 12k figure); 5 `floor_dbfs` at Input Gain
min, mid, max, recorded.

## As built (2026-10-30, v2.00.45)
1. Attack and release are travel times, not time constants. A one-pole read as a time constant needs 4.4 of them to cross 80 dB to within 1 dB, so a 0.5 ms constant could not meet the 1 ms acceptance. The Input gate's tuning carries `travelTimes`; attack 0.5 ms is the rise from -80 dB to within 1 dB of open (time constant 0.114 ms) and DECAY is the fall of 60 dB (time constant DECAY / 1.386). The panel GATE keeps its time constants and its code path. Measured: open within 0.63 ms, under -60 dB within 77 ms at DECAY 20 ms and 2060 ms at DECAY 2 s.
2. Acceptance 4 runs on binary noise whose level wobbles ± 1.5 dB, which the peak detector reads exactly. Gaussian noise through a 0.1 / 5 ms peak detector spreads 3 to 8.6 dB around its mean, so a mean 1.5 dB under the threshold flipped the gate 38 times in 10 s; that is the detector, not a fault. A second test shows the LEARN rule, threshold = floor + 8 dB, holds the gate shut on Gaussian noise for 10 s.
3. Acceptance 7 holds the spec's wording (difference against the settled new filter under -40 dBFS) for every high-pass change, and holds the difference against the ideal blend of the two settled outputs under -40 dBFS for every change (worst 43.1 dB under). A low-pass jump or a change to OFF cannot meet the first wording because the settled filters already differ at 1 kHz by up to 27.6 dB.
4. Flush-to-zero is set in `FPDSCR` as well as `FPSCR`, because the FPU loads `FPDSCR` on exception entry and the audio interrupt would otherwise run without it.
5. Level rule: the default HPF takes the level test's program down 0.51 dB (its 180 Hz fundamental by 0.8 dB). A filter cannot reach 0 to +0.5 dB from below without a trim that would be wrong on other material, so the conditioning rows of `level_test` hold -1.0 to +0.1 dB and carry no trim. The Input gate at its box tuning holds +0.00 dB.
6. Cost: `kInputConditioning{2, 3, false}` in the fixed term of `costs.h` (host bench 0.96 to 0.99 of POLISH EQ, about 8.9k cycles mean, 17k on a crossfade block). The Input gate row keeps its board figures `{4, 5, true}` as an upper bound; the host puts the revised gate at 0.97 of them. The board prints a `COND` cycle bucket in the audio line for the rerun that replaces the estimate.
7. Record bytes 6 and 7 are always written as list index + 1 (OFF is 1). A 0 only comes from an older record and reads as the default.
8. HPF and LPF are not in the serial protocol's `settings` object; the Mac app is parked. `floor_dbfs` is in the status reply, the highest detector level since the previous status, -120 when no block ran the Input gate.
9. LEARN was built in v2.00.45 as a measuring screen and a result screen, and removed by the owner the same day (v2.00.48). The detector's status window and `floor_dbfs` stay.
10. The emulator and `cubevox-render` have their own chains and carry the conditioning by hand at the box defaults (`render --hpf`, `--lpf` in Hz). The emulator's Input gate pots follow the box so the parity test holds. The BYPASS dry copy is taken before the conditioning.

## Hardware, out of scope
Lower floor rather than mask it: the instrumentation amp's gain resistor RG at 1 kΩ adds about
4 nV/√Hz by itself, so a lower RG with Rf scaled is the cheapest win; a lower-noise op-amp or a
mic preamp part; a buffered, filtered VREF; a relay-switched resistive -30 dB output pad in place
of the digital MIC trim.
