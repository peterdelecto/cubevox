# cubevox — distortion stage (Boss BD-2 Blues Driver model)

Date 2026-10-01. Adam uses a Boss Blues Driver. Panel knob is DRIVE only (owner:
"its not intended to push an amp, so no level needed"). Stage is always on with a
menu bypass; in the prototype the block checkbox is that bypass. Engine contract,
emulator skeleton, threading unchanged.

References: analogisnotdead.com/article25 (topology) and an owner-supplied
breadboard analysis of the BD-2 / Cobalt circuit (numbers below come from it).

## BD-2 signal path (owner-supplied analysis)

1. Input buffer, 1 MΩ input impedance. Clean at low GAIN.
2. Gain stage 1: discrete op-amp (Q5 BJT makes most of the gain and sets the
   overload). Rail-to-rail swing, well-controlled overload, not FET-style distortion.
   Max gain a little over 40 dB (100×), peaking between 2 and 3 kHz. Bass rolls off
   from 700 Hz (R5/C4); C5 rolls off the top above the peak.
3. Fender tone stack, Treble 0 / Mid 6 / Bass 10. Mostly puts back the bass lost in
   stage 1 and loses a lot of level doing it. Because of that loss, stage 1 saturates
   long before the clipping diodes conduct. **The diodes do nothing audible**; the
   author scoped it with them in, out, and jumpered. They are omitted from the model.
4. Gain stage 2: same discrete op-amp, max gain just under 40 dB, flat 100 Hz to
   about 6 kHz. Both GAIN gangs turn together (audio taper, dual A250K).
5. Fixed treble cut: −6 dB above 1 kHz.
6. TONE: acts above 1 kHz, flat at noon. The panel TONE knob maps to the treble
   shelf gain, noon 0 dB, fully down `toneMinDb`, fully up `toneMaxDb`.
7. LEVEL, then a gyrator (32 H) resonating with C17 for a +6 dB bump at 120 Hz.
8. Max end-to-end gain 52 dB. Distortion fades out abruptly as a note decays, which
   is the signature of rail saturation rather than a soft knee.

Notes from the same author's Boneyard / BD-2w mod write-up (part 4):

1. Diodes confirmed inaudible a second time ("lightly clip the very lowest freq
   signals when GAIN is above noon", then stage 2 clips hard anyway).
2. BD-2w S/C switch = TIGHT (extra bass cut at the stage-2 input, about 2 dB more
   with 3.3 kΩ than 10 kΩ) + FAT (more capacitance in the TONE network, more treble
   cut). In this model TIGHT is `s2HpHz` raised, FAT is TONE turned down. Measured
   on the -2w (part 2): C position is 4.5 dB weaker at 50 Hz and 1 dB stronger at
   500 Hz at the stage-2 input; overall S vs C differs little, "a little louder in
   C". Owner 2026-10-01: Adam's BD-2 is stock, not modded, not a Waza. The notes
   above are reference only; the model is the stock pedal.
3. In clean mode the stock response is roughly flat below 500 Hz, so the stage-1
   shelf cut and the tone-stack bass boost should net near 0 dB there at low DRIVE.
4. The bass boost sits after the clippers. Once stage 2 limits everything to the
   rails, the post boost makes everything below 200 Hz louder than the rest. This is
   why `bassPeak*` is in the post chain and why its level matters at high DRIVE.

## Decisions

1. Behavioural model, one cheap block per stage above, every constant a tuning item.
2. DRIVE sweeps both stage gains exponentially: `gN = gainNMin * (gainNMax / gainNMin)^d`.
   Owner 2026-10-02: the knob was dead until 50-60 % and far too saturated at 100 %.
   Each stage now starts with its gain near clipping (defaults 16 to 45 and 2 to 7.5),
   so the 3rd harmonic of a -12 dBFS 220 Hz sine runs about -41, -29, -21, -15, -12 dB at
   DRIVE 0.1, 0.3, 0.5, 0.7, 1 (old: nothing below 0.45, -11.4 at 0.7, -11.2 at 1).
   DRIVE 0 is sample-exact passthrough so the always-on stage is transparent at zero.
   Owner 2026-10-30, board 1 with Adam's samples: "distortion is a little subtle at max",
   and the BD-2's voice is what Adam picked, so the top of the knob gets reach without a
   change of character, as a boost in front of the pedal does. From DRIVE 0.6 to 1 stage 1
   gains up to `topBoostDb` = 6 dB more, linear in dB over that span (`topBoostFrom` = 0.6),
   on top of the exponential sweep; stage 2, both rails, the tone stack and the post chain
   are unchanged, and DRIVE 0 to 0.6 is bit-exact against before. The owner asked for 6 dB
   ("split the difference" against a 12 dB proposal). Renders of the three samples at DRIVE
   1, before and after, in `build/drive-ab/`.
3. The only nonlinearity is rail saturation, applied after stage 1 and after stage 2,
   as a clamp with a short soft edge. A small tunable rail asymmetry stands in for
   imperfect biasing (default 0.05). The soft edge is a quadratic knee since v2.01.16
   (audit fixes, item 6).
4. No LEVEL knob. A static makeup law keeps the output level of Adam's three clips flat
   across DRIVE; `trimDb` sets where it sits (audit fixes, item 4).
5. The two saturators run 2× oversampled (small half-band FIR) to keep aliasing off a
   vocal. The box has no non-oversampled path.
6. Chain order: PitchFx → Unison → Slapback → Distortion.

## Engine (`engine/distortion.h`)

```
namespace cv {

struct DistortionTuning {
  float inputHpHz = 90.0f;        // input cap
  // stage 1 shaping (before its saturator)
  float s1BassHz = 700.0f;        // low shelf cut corner
  float s1BassDb = -12.0f;        // low shelf depth below s1BassHz
  float s1LpHz = 6892.0f;         // top roll-off (C5)
  float gain1Min = 16.0f;         // stage gain at DRIVE 0
  float gain1Max = 45.0f;         // stage gain at DRIVE 1
  float topBoostDb = 6.0f;        // extra stage 1 gain at DRIVE 1, ramped in dB
  float topBoostFrom = 0.6f;      // DRIVE where the top boost starts
  // tone stack between the stages (bass 10 / mid 6 / treble 0)
  float stackBassHz = 400.0f;     // low shelf boost corner
  float stackBassDb = 10.0f;
  float stackTrebleHz = 2000.0f;  // high shelf cut corner
  float stackTrebleDb = -4.0f;
  float stackLossDb = -20.0f;     // insertion loss
  // stage 2
  float s2HpHz = 100.0f;
  float s2LpHz = 6645.0f;
  float gain2Min = 2.0f;
  float gain2Max = 7.5f;
  float railAsym = 0.05f;         // 0..0.5; positive rail 1, negative -(1 - asym)
  float railSoft = 0.1f;          // soft edge as a fraction of the rail
  // post
  float trebleCutHz = 1000.0f;    // fixed high shelf cut
  float trebleCutDb = -2.7f;      // half the BD-2's cut (owner 2026-11-01, "a bit dark")
  float toneMinDb = -12.0f;       // TONE fully down: shelf gain added above trebleCutHz
  float toneMaxDb = 6.0f;         // TONE fully up; noon adds 0 dB
  float bassPeakHz = 120.0f;      // gyrator bump
  float bassPeakDb = 2.0f;
  float bassPeakQ = 1.0f;
  float trimDb = 2.87f;           // level rule, fitted on Adam's clips
  float fadeDrive = 0.05f;        // dry-to-chain fade span from DRIVE 0
};

struct DistortionParams {
  bool on = true;                 // panel toggle
  float drive = 0.0f;             // panel knob 0..1
  float tone = 0.5f;              // panel knob 0..1; noon is flat
  DistortionTuning tuning;
};

}
```

`applyTuning` clamps every dB field to ±24 dB (`topBoostDb` 0 to 24 dB) and the stage gains to
0.1 to 1000; `cubevox-render` refuses a value outside those limits.

Behaviour per sample:

1. `x = hp1(in, inputHpHz)`.
2. Stage 1 shaping: `x = lowShelf(x, s1BassHz, s1BassDb)`; `x = lp1(x, s1LpHz)`;
   `x *= g1`; `x = rail(x)`, with `g1 = gain1Min * (gain1Max / gain1Min)^drive` plus the top boost.
3. Tone stack: `x = lowShelf(x, stackBassHz, stackBassDb)`;
   `x = highShelf(x, stackTrebleHz, stackTrebleDb)`; `x *= dbToLin(stackLossDb)`.
4. Stage 2: `x = hp1(x, s2HpHz)`; `x = lp1(x, s2LpHz)`; `x *= g2`; `x = rail(x)`.
5. Post: `x = highShelf(x, trebleCutHz, trebleCutDb + toneShelfDb)`, where `toneShelfDb` runs
   `toneMinDb`..0 over tone 0..0.5 and 0..`toneMaxDb` over 0.5..1 (`tone` smoothed with the same
   20 ms one-pole as `drive`); `x = peakEq(x, bassPeakHz, bassPeakDb, bassPeakQ)`; DC blocker `hp1`
   at 20 Hz (the lowest corner `OnePole::set` takes).
6. `rail(x)`: limits `[-(1 - railAsym), 1]`. Each side is linear up to `limit - edge`
   (`edge = railSoft * limit`), then the knee `v - t^2 / (4 edge)` over two edge widths, which
   meets the limit with zero slope. It has no branch. Both rails run inside the 2× oversampler.
   The half-band's four odd taps sum to exactly 0.25, so each pass is unity at DC.
7. Makeup: `x *= dbToLin(trimDb) / makeup(g1 * g2)` with
   `makeup(g) = a*g / sqrt(1 + (a*g / s)^2)`, a = 0.06, s = 12.53. Fitted on Adam's three clips:
   the default (DRIVE 30 %, TONE 50 %) reads +0.25 dB on their mean, and DRIVE 10 % to 100 %
   stays within -0.32 to +0.34 dB of that.
8. DRIVE and TONE are smoothed with the 20 ms one-pole. The stage turns on when the knob reaches
   `fadeDrive` (5 %) and off when it falls under 0.8 × `fadeDrive` (4 %), keeping its last state
   between the two. On, the smoother aims at the knob or `fadeDrive`, whichever is higher; off, at
   exactly 0. The dry-to-chain mix moves toward `smoothstep(clamp(drive_ / fadeDrive, 0, 1))` by at
   most 1 / (16 ms × 48 kHz) a sample, both ways. Drive and mix both at 0 skips the chain and copies
   the input exactly. While they sit at 0, TONE follows the knob, so an enable starts from the knob.
9. Shelves and peak are RBJ biquads. While TONE moves, the treble shelf's coefficients interpolate
   across each 64-sample chunk; settled, they stay fixed.
10. A NaN knob reads as the clamp's bound. If a block leaves a non-finite filter state, the chain
    clears.

Box code:

1. A call runs in 64-sample chunks, each stage by stage: five loops, so each recursion keeps its
   state in registers, with the rails and half-bands working on linear histories in the engine's
   scratch.
2. A settled chunk (DRIVE and TONE at their targets) runs with fixed gains, no smoothers and no fade.
3. `reset()` clears the state and keeps the coefficients, which depend on the tuning alone.
4. The `Distortion` object sits in DTCM, 2,624 B.
5. The panel toggle switches the stage, which settles off to a plain copy.

## Emulator

`DISTORTION` checkbox block under SLAPBACK with a `DRIVE` slider `%.0f %%`, opens
at 30 %, then a `TONE` slider `%.0f %%` opening at 50 %. Tuning node `Distortion` with every field above in real units (log sliders
for Hz spanning decades). Reset / Print cover it. `ProtoParams` gains
`cv::DistortionParams distortion`; callback order PitchFx, Unison, Slapback,
Distortion.

## Render CLI

`--drive <0..1>` enables the stage; `--tone <0..1>` sets TONE (default 0.5). Tuning keys via
`--tuning k=v`, named `dist` + field name in lowerCamel (e.g. `distS1BassHz`, `distGain1Min`,
`distGain1Max`, `distGain2Min`, `distRailAsym`, `distToneMinDb`, `distToneMaxDb`, `distTrimDb`).
`distortionTuningInRange` refuses a value outside the engine's limits.

## Tests

`test/distortion_test.cpp`, ctest `distortion`. Test tone = 220 Hz sine at −12 dBFS (amplitude
0.251). Harmonic levels by Goertzel over seconds 1–2. Readings as of v2.01.16.

1. Passthrough: drive 0, and `on = false` with drive 1, equal the input exactly (`==`).
2. Switch-off: off or DRIVE 0 after DRIVE 0.5 and 1 equals the input exactly after 0.5 s.
3. Engage: DRIVE 4.5 % from fresh stays a copy; 6 % engages; 4.5 % after 6 % stays engaged;
   3.9 % turns it off.
4. Call sizes 1, 17, 64, 200 and 1000 from a settled state equal 64-sample calls exactly.
5. Non-finite: a NaN sample recovers; NaN DRIVE and TONE keep the output finite.
6. Harmonics: 3rd at drive 0.1 / 0.3 / 0.5 / 0.7 / 1 within ±3 dB of −40 / −28 / −20 / −14 /
   −11.6 and rising (reads −40.7 / −27.9 / −20.4 / −13.5 / −11.2).
7. Clean: at drive 0.1 the 3rd is ≤ −38 dB.
8. Level on the synthetic tone: within ±4 dB at drive 0.15, 0.5, 1.
9. Asymmetry at drive 0.2: 2nd ≥ −46 dB at `railAsym` 0.05, ≤ −55 dB at 0.
10. DC: `|mean(out)|` at drive 1 < 1e-3.
11. Tone: at 8 kHz, tone 1 over tone 0 by ≥ 12 dB (reads 18.0), noon equal to no TONE shelf.
12. No allocation inside `process()`; the top boost and gain-law cases.

`level_test` holds the default on Adam's three clips, mean 0 to +0.5 dB, and DRIVE 10 % to 100 %
within 0.5 dB of the default; the synthetic program prints as a note. `knob_step_test` places
Distortion at DRIVE 30 % and judges 25 rows at ×1.05 of steady-state curvature: the enable edge and
DRIVE up from 0 at DRIVE 30 / 60 / 100 % × TONE 0 / 50 / 100 %, the four crossings of 5 %, and a
fast 6 to 3 % jump.

## Audit fixes (2026-11-02, v2.01.16)

Owner 2026-11-02, after the efficiency and correctness audits
(`build/audit/2026-11-02-distortion-audit-*.md`): "we must address and fix all".

1. Enable edge. An enable, or DRIVE up from 0, replayed a TONE glide from noon or a stale value:
   the treble started up to 7 dB off, curvature ×48.7 at DRIVE 30 % / TONE 0 %. TONE now follows
   the knob while the stage is dry, and the 16 ms fade lets the filters settle. Reads ×1.04 / 1.00
   / 1.00 at DRIVE 30 % and TONE 0 / 50 / 100 %. 16 ms is the shortest of 8, 12, 16 and 20 ms that
   passes every row; 20 ms reads ×1.27.
2. TONE zipper. The shelf stepped once a block, a 750 Hz buzz at −33 to −40 dB peak on a fast
   sweep. The coefficients now interpolate; a TONE sweep reads ×0.99.
3. Bottom of DRIVE. Parked at 1 to 4 % the dry and the 14-sample-late chain combed (−12 dB at
   1.5 kHz at 2.5 %). The on/off thresholds keep the knob from parking there. The smoothstep goal
   keeps a fast 6 to 3 % jump at ×0.99, where a plain clamp read ×1.19 at every fade length.
4. Level. The default read +0.74 dB on Adam's clips (+1.06 on clip 3) and +2.5 dB at DRIVE 100 %,
   against a synthetic program that read +0.16. Trim 3.3 → 2.87 dB and makeup s 10.3 → 12.53.
5. CPU. Stage-major loops, the settled path, fixed rail constants, closed-form smoothers, cached
   shelf sine and cosine, `exp2Fast` gains, coefficients kept on reset, a word compare of the
   tuning, the object in DTCM. M7 model: a settled sample 517 → 355 cycles; block 41.5k / 63.7k →
   23.2k / 29.8k cycles mean / costliest. `kDistortion` is {5, 6} until a box stress run with the
   Distortion sweeps measures it (it was {8, 11}, measured).
6. Knee. A branch-free quadratic knee replaces the `tanh` soft edge. It makes the cost flat at every
   level. Output differs from the `tanh` rail by −68 dB RMS at the default and −57 dB RMS at DRIVE
   100 % / TONE 100 %. Before and after at DRIVE 100 % in `build/audition/distortion-fix-knee.wav`.
7. Half-band taps scaled to unity at DC (they summed to 0.49628 a pair, −0.03 dB a pass and a
   −48.6 dB image leak). DC blocker named 20 Hz, which it always was.
8. Guards: NaN-proof knobs, a chain that clears on a non-finite state, tuning clamps, render ranges
   and the two missing render keys.
9. Tests: clip rows fail on an empty clip when `samples/` exists; passthrough with `==`; switch-off,
   call-size, engage and NaN cases; the `knob_step_test` rows above.
10. Not built. Stage 1 at the base rate (it would fold stage 1's harmonics into the band; decision
    5). Brightness unchanged: at the default the top sits 0.5 to 3 dB under the dry voice and TONE
    full up puts it 2 to 4 dB over, and the open ear item is closed.

## Done when

`ctest` passes all stages; face shows DISTORTION under SLAPBACK; owner listens with
a vocal loop and tunes toward the Blues Driver by ear.
