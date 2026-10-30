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
| Attack | 0.5 ms, one-pole on the gain |
| Hold | 50 ms after the detector falls under the close level |
| DECAY pot | the release time, 20 ms to 2 s, log (amended; the draft fixed 150 ms) |
| Range | -80 dB, fixed |
| Lookahead | none; zero latency |

The release went onto the DECAY pot because slot 1 has two pots and a fixed release would leave
one doing nothing. The panel GATE card in slot 7 keeps its soft-knee tuning; both are instances of
the Gate class and the revision is the input gate's tuning.

## LEARN (amended)
A menu action, `GATE LEARN >`. It measures the peak detector for 2.0 s on a quiet input at the
current Input Gain and shows two lines, the measured floor and the pot's threshold, for example
`FLOOR -62 dB` over `THRESHOLD -54 dB`, with a note when the threshold is under the floor + 8 dB.
It writes nothing: the THRESHOLD pot is absolute and the player sets it by eye about 8 dB above
the floor. No stale indicator (the draft had LEARN set the threshold and flag it stale after an
Input Gain move; both fought the pot).

The floor measurement is also a field of the serial status reply, `floor_dbfs`, so the bench
tests below can read it.

## Menu and settings
Three rows join the list, which scrolls: `HPF: 120` and `LPF: 12k` after DEBUG, `GATE LEARN >`
after them, before UPDATE FIRMWARE. HPF and LPF take settings bytes 6 and 7 of the layout record,
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
(4.5 dB unweighted expected at LPF 8k; record the 12k figure); 5 LEARN shows the floor within
2 dB of the status field at Input Gain min, mid, max.

## Hardware, out of scope
Lower floor rather than mask it: the instrumentation amp's gain resistor RG at 1 kΩ adds about
4 nV/√Hz by itself, so a lower RG with Rf scaled is the cheapest win; a lower-noise op-amp or a
mic preamp part; a buffered, filtered VREF; a relay-switched resistive -30 dB output pad in place
of the digital MIC trim.
