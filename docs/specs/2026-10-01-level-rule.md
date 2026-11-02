# Level rule

Owner 2026-10-01: every stage, engaged at its default setting, outputs near unity or
a tad hotter, 0 to +0.5 dB RMS over its input.

## Method

`test/level_test.cpp` (ctest `level`) synthesizes 3 s of program material:
180 Hz tone, 10 harmonics at 1/k, 5 Hz vibrato of +-20 cents, 2 Hz raised-cosine
phrase envelope, plus pink-ish noise at -40 dBFS, normalised to -18 dBFS RMS.
Out/in RMS is measured over seconds 1 to 3. PASS is [0, +0.5] dB with 0.1 dB slack.
Each trim was solved by bisection to land near +0.25 dB, then rounded to 0.1 dB.

Method change 2026-10-02: stages with a MIX knob are measured at MIX 1 (wet only),
Harmony with one voice at level High (3). The first pass trimmed at MIX 0.5 with a
Medium voice, which left the wet path about 8 dB hot, so Harmony clipped at full
MIX (render of a -6 dBFS peak vocal: 0.0 dBFS on engine A, +0.5 dBFS on engine B).
The equal-power crossfade then stays near unity at every MIX; the row
"harmony A mix .5" reports +0.12 dB against a -1.5..+1.0 band, with no trim chasing.

Harmony scales the summed voices by 1/sqrt(active count), applied to the target
gains so the existing smoothers cover voice switching. Three voices at High, mix 1,
measure within 0.1 dB of one voice on all three engines (harmony_test).

## Results

| Stage and setting | Trim field | Old trim | New trim | New dB |
|---|---|---|---|---|
| Harmony A, High, level 3, mix 1 | HarmonyTuning.trimDb[0] | +8.4 | +2.2 | +0.30 |
| Harmony B | HarmonyTuning.trimDb[1] | +7.7 | +1.4 | +0.21 |
| Harmony C | HarmonyTuning.trimDb[2] | +8.5 | +2.2 | +0.24 |
| Octave A -12, mix 1 | OctaveTuning.trimDbA | -5.3 | -1.7 | +0.26 |
| Octave B +12, mix 1 | OctaveTuning.trimDbB | +6.2 | +6.5 | +0.28 |
| Octave C -12, mix 1 | OctaveTuning.trimDbC | +2.6 | +2.2 | +0.23 |
| Reverb CHASM, mix 1 | ChasmTuning.wetDb | +17.4 | +19.0 | +0.27 |

Measured with the old trims under the new method, before retrimming: Harmony A/B/C
+6.50/+6.51/+6.54, Octave A/B/C -3.34/-0.02/+0.63, Reverb SPRING/CHASM
-2.57/-1.33 dB. Rows not listed (gates, autotune, unison, slapback, distortion,
polish) keep their trims and results from the first pass.

Unison correction 2026-10-02: the first pass cut wetMaxDb to -12.4 dB, which removed
the effect (Unison has no MIX knob, so its wet is the effect). wetMaxDb is back at the
owner's -4.0 dB, and the level comes from UnisonTuning.trimDb = -1.4 dB, applied to
the whole output as dbToLin(trimDb x depth), so depth 0 is bit-exact unity. Depth 0.8
measures +1.38 dB without the trim and +0.26 dB with it. The unison_test level band at
DEPTH 1 is now 0.91 +-1.5 dB. Slapback keeps wetMaxDb +4.9 dB (+0.18 dB at 0.5).

Slapback correction 2026-10-07: the slapback row was a coin flip on TIME (the tone adds
or cancels with its own echo). The row now averages over TIME 60..250 ms; wetMaxDb stays
+4.9 dB and SlapbackTuning.trimDb = -4.9 dB x INTENSITY lands the mean at +0.25 dB.

Echo and Unison 2026-11-02 (owner 2026-11-01: the dry never drops). Both trims now sit on the
wet alone, as one constant, and the dry passes at exact unity at every setting; this replaces
the whole-output trims above. The rows run on Adam's three clips and are judged on the mean:
Echo at AMOUNT 25 % (SlapbackTuning.trimDb = -5.3 dB, power sum over TIME 60..250 ms) reads
+0.25 dB; Unison at its default DEPTH (UnisonTuning.trimDb = -0.7 dB) reads in band. Because
the wet adds on top of a unity dry, the output rises with the knob: Echo +1.10 dB at 50 % and
+3.38 dB at 100 %, Unison +2.18 dB at 75 % and +3.03 dB at 100 %. Those rows print as notes,
because the rule binds the default.

The reverb rows take every setting from the registry: the card placed and switched on, TIME
and dwell where the box leaves them (TIME 50 %, dwell 0.20), and the crossfade forced to the
wet alone. Both reverbs' rows run on Adam's three clips and are judged on their mean (owner
2026-11-01: Adam's clips govern the reverb level). After the reverb speed pass (v2.01.11),
ChasmTuning.wetDb = +17.7 dB reads +0.83 / -0.13 / +0.07 dB, mean +0.26 dB, and
SpringTuning.wetDb = +0.37 dB, the one gain ahead of the loops, reads +0.92 / +0.23 / -0.54 dB,
mean +0.20 dB. The table's +19.0 and the later +19.2 dB were Chasm's eight-allpass loop. MIX 50 %
rows report against the -1.5..+1.0 dB band and sit within ±0.15 dB for both reverbs. Harmony and
Octave A and B are out of the code, so their rows are gone.

## Notes

1. Wet-only at MIX 1, out/in near unity means wet RMS near dry RMS. Equal power then
   keeps the middle of the knob near unity.
2. Polish was too quiet, not too hot. The program has no content near 3.5 kHz or
   10 kHz, so raising presence and air cannot lift it. The trim keeps the EQ shape.
3. The trims are program dependent. They hold on this synthetic voice, not on every loop.
4. Existing per-effect tests that pin response or level shape set their own trims
   (polish and octave B zero the new trims); the unison level band is now 0.61 +-1.5 dB
   at DEPTH 1.
5. The emulator and render CLI build defaults from the engine structs, so Reset to
   defaults and `cubevox-render` pick the new values up. The emulator slider ranges
   for slapback, unison and chasm wet level were widened to fit.
