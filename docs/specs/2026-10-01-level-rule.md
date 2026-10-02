# Level rule

Owner 2026-10-01: every stage, engaged at its default setting, outputs near unity or
a tad hotter, 0 to +0.5 dB RMS over its input.

## Method

`test/level_test.cpp` (ctest `level`) synthesizes 3 s of program material:
180 Hz tone, 10 harmonics at 1/k, 5 Hz vibrato of +-20 cents, 2 Hz raised-cosine
phrase envelope, plus pink-ish noise at -40 dBFS, normalised to -18 dBFS RMS.
Out/in RMS is measured over seconds 1 to 3. PASS is [0, +0.5] dB with 0.1 dB slack.
Each trim was solved by bisection to land near +0.25 dB, then rounded to 0.1 dB.

## Results

| Stage and setting | Before dB | After dB | Trim field | Default |
|---|---|---|---|---|
| Input gate -40 | +0.00 | +0.00 | none | none |
| Panel gate -40 | +0.00 | +0.00 | none | none |
| Harmony C, High, level 2, mix 0.5 | -2.38 | +0.23 | HarmonyTuning.trimDb (new) | +8.4 |
| Octave A -12, mix 0.5 | +2.77 | +0.32 | OctaveTuning.trimDbA (new) | -5.3 |
| Octave B +12, formant 0, mix 0.5 | -1.89 | +0.23 | OctaveTuning.trimDbB (new) | +6.2 |
| Unison depth 0.8 | +2.84 | +0.25 | UnisonTuning.wetMaxDb | -12.4 |
| Slapback intensity 0.5 | -0.62 | +0.27 | SlapbackTuning.wetMaxDb | +4.9 |
| Distortion drive 0.3, tone 0.5 | +0.19 | +0.19 | DistortionTuning.trimDb | 0.0 |
| Reverb SPRING, mix 0.5 | -0.24 | +0.24 | SpringTuning.wetDb | +1.3 |
| Reverb CHASM, mix 0.5 | -2.76 | +0.32 | ChasmTuning.wetDb | +17.4 |
| Reverb PARKER, mix 0.5 | -3.59 | +0.32 | SpringCTuning.wetDb | +6.8 |
| Polish EQ defaults | -1.30 | +0.30 | PolishTuning.trimDb (new) | +1.6 |

## Notes

1. Wet-only reverb at MIX 0.5 is equal power, so out/in near unity means wet RMS near
   dry RMS. The three engines re-level to that by measurement.
2. Polish was too quiet, not too hot. The program has no content near 3.5 kHz or
   10 kHz, so raising presence and air cannot lift it. The trim keeps the EQ shape.
3. The trims are program dependent. They hold on this synthetic voice, not on every loop.
4. Existing per-effect tests that pin response or level shape set their own trims
   (polish and octave B zero the new trims); the unison level band is now 0.61 +-1.5 dB
   at DEPTH 1.
5. The emulator and render CLI build defaults from the engine structs, so Reset to
   defaults and `cubevox-render` pick the new values up. The emulator slider ranges
   for slapback, unison, chasm and parker wet level were widened to fit.
