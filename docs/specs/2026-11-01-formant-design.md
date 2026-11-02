# Formant card: Adam's throat, bigger or smaller, at the same pitch

Owner 2026-11-01: "let's add a formant shift. spec it out and we will build in a bit." Built
2026-11-01 (v2.00.95) for slot 7. Moved to slot 3 as Octave's alternate the same day (v2.00.96,
owner: "formant should be an alternate for octave rather than slot 7"). MIX holds its level from
v2.01.00. The audit fix pass of 2026-11-02 (v2.01.10) set the trim per SHIFT and the 0.04 s power
window. This file states the card as built at v2.01.10; section 10 keeps the history.

## 1. Sound

1. SHIFT moves the voice's formants, the resonances of the throat and mouth, while the sung pitch
   stays where it is. Down sounds like a bigger singer (a giant, a deep male voice); up sounds like
   a smaller one (a child, a female voice, a cartoon at the top).
2. MIX blends Adam's voice with the shifted voice. Partway, it is two singers with two throats.
3. With Autotune on, the shifted voice sits on the tuned note, because it takes Autotune's
   correction as Octave and MOB do.

## 2. Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | SHIFT | stepped, 15 positions, −7 to +7 st | `SHIFT -3 st` |
| lower (knob B) | MIX | continuous, 0..100 % | `MIX 100 %` |

1. Card id `formant`, display name `Formant`, Group Pitch, Engine Pitch (like Octave), cost row
   `costs::kFormant`, card id 11 after Crowd.
2. SHIFT is stepped like Octave's SEMITONES, so 0 is findable and the readout names whole
   semitones. SHIFT glides through the 20 ms one-pole in semitones, because a bare step reads
   x1.23 of steady-state curvature next to SHIFT 0 and the glide holds every step under x1.02. On
   the first call after the voice starts, SHIFT takes the knob outright.
3. Defaults (level rule, tests, emulator start): SHIFT −3 st, MIX 100 %.
4. MIX is the amount knob, the lower pot, as on every card (owner 2026-10-30).
5. The card swaps with Octave in slot 3 through its own SLOT n row (`octaveFormantSlot`,
   `UiAction::SwapOctaveFormant`, values OCTAVE and FORMANT), built like the UNISON / MOB row. The
   emulator's slot 3 carries an OCTAVE over FORMANT switch.

## 3. Where it acts

1. The shift is a second voice inside the pitch stage (`PitchFx`), so it acts at the first
   pitch-group card's slot, slot 2 in the default layout. Its sound comes ahead of Octave's voice,
   MOB, Echo and Distortion.
2. The formant voice reads the pitch stage's input ring, the tracker's period and Autotune's
   correction, the same three things MOB's copies read.

## 4. Engine (`engine/pitch_fx.h`, `engine/formant_voice.h`)

1. `FormantParams { bool on; int shiftSt = -3; float mix = 1.0f; FormantTuning tuning; }` sits in
   `PitchFxParams` as `formant`. `FormantTuning` holds the 15-entry `trimDb` table and an
   `offsetDb` added to every entry for the render tool (`fmTrimDb`, ±24 dB, clamped in the engine).
2. The voice is `FormantPsolaVoice` in `engine/formant_voice.h`, shared with the MOB card. Pitch
   ratio = Autotune's correction (1 while Autotune is off), grain playback rate = 2^(shift / 12)
   at the glided SHIFT, read delay = Autotune's voice delay.
3. Line-up with Autotune. The voice holds one ratio per call, the correction at the call's middle,
   so it matches Autotune's voice within one call. The call that lays the grain marks reads the
   correction at its start, where the first grain launches, and a voice that starts beside a
   running Autotune adopts its grain grid. The alignment test finds no lag between the two voices;
   under vibrato they sit 0.41 samples apart.
4. Range. ±7 st covers a gender swap (about ±3 to ±4 st) and a giant or a child (±6 to ±7 st);
   past that the voice turns to a cartoon. A grain lasts two periods of input at the playback
   rate and never less than two output periods. A shorter Hann window leaves the window sum
   dipping between grains, which pulses breaths and sibilants at the sung pitch; MOB shares the
   same floor. The read stays behind the write head at every shift in range: Autotune's delay is
   1,040 samples plus a period, and at +7 st and 70 Hz the nearest read sits 1,270 samples behind
   the head. The voice's read-span clamp (`setReadSpan`) still guards it.
5. Silence. A grain whose reads all fall inside a run of exact zeros in the ring skips its taps,
   and the output is bit-exact against a voice that plays them.
6. Latency. The formant voice lags the input by 23 to 36 ms, as Autotune's voice does. With
   Autotune off and MIX between 0 and 100 %, Adam's dry voice and the shifted voice sit that far
   apart and read as a tight double. With Autotune on they are aligned.
7. Tracker. It runs while Autotune, MOB or Formant is on, and its 48 kHz fine pass runs while
   Autotune or Formant is on. The formant voice is the lead, so its pitch must be the sung pitch to
   the cent; the coarse lag alone is off by up to about 14 cents at 200 Hz.
8. Toggle and enable edge. The toggle drives a 20 ms crossfade (`formantFade_`, the `kSmoothCoef`
   one-pole like `octaveFade_`). Toggled off and settled, the voice does not tick and the stage's
   output equals its input. The voice resets on its enable edge only when `formantFade_` sits at
   exactly 0 (engine rule).
9. Memory. The formant voice adds about 640 B of DTCM to `PitchFx`; the ring is unchanged. The
   build prints DTCM at 76,608 B.

## 5. MIX and level

1. The base is the stage's output ahead of Octave (the dry voice, or Autotune's crossfade to the
   corrected voice). Gains are `1 - mix` on the base and `mix` on the shifted voice, times the
   toggle's fade.
2. A level gain raises the sum to the power-weighted average of the two voices' own powers. It
   reads running powers of the base, the voice and their product over 0.04 s
   (`kFormantPowerSec`), never drops below 1 and is capped at x2 (+6 dB,
   `kFormantLevelGainMax`). It is exactly 1 at MIX 0 and 100 % and for identical voices, and
   1.41 at MIX 50 % for unrelated voices of equal level. The powers clear at silence and when a
   NaN or an infinity gets into them.
3. At MIX 100 % with Autotune on, the corrected voice is not heard and its powers are not read.
   When MIX leaves 100 %, the level gain starts from the shifted voice's power and the correlation
   SHIFT gives (`kFormantCoherence`, 0.95, 0.4, 0.1 and 0 at 0, 1, 2 and 3 st or more, measured on
   Adam's clips). Without it, MIX 100 to 50 % at SHIFT 0 jumped +1.78 dB.
4. No fixed gain curve can hold the level. Near SHIFT 0 the two voices are almost one waveform and
   add in amplitude; at SHIFT −3 on Adam's clips they correlate at about 0.06 and add in power.
5. With Autotune off and MIX in the middle, a sustained note can put the two voices in antiphase,
   and the +6 dB ceiling cannot refill the hole. Left as it is (lead 2026-11-02). Aligning the
   voices would delay Adam's dry voice 22 to 36 ms, and a higher ceiling would boost every
   anti-correlated passage.

## 6. Level rule

1. The shifted voice alone at MIX 100 % is trimmed to unity per SHIFT. The grains sum to a level
   that follows SHIFT, so one trim cannot hold it.
2. `trimDb` from −7 to +7 st: 4.29, 3.87, 3.12, 2.36, 1.98, 1.72, 1.01, 0.20, 0.94, 1.44, 1.51,
   1.77, 1.87, 1.60, 1.64 dB. Fit on Adam's three clips with Autotune off, iterated because the
   level is not linear in the trim. Each SHIFT's mean over the clips reads +0.245 to +0.25 dB.
   The glided SHIFT reads the trim between the two entries around it.
3. `level_test` holds a clip row per SHIFT (each clip in the mid band, mean 0 to +0.5 dB) and the
   MIX 50 % clip rows. The synthetic-program row at −3 st reads +0.64 dB and prints as a NOTE,
   because the clips govern the level.

## 7. Firmware

1. `registry.cpp`: the card entry, `applyKnob` (SHIFT position i to `shiftSt = i - 7`, MIX to
   `mix`), `applyToggle` (`out.pitchFx.formant.on`).
2. `layout.cpp`: `layoutCost` adds `kTracker` once when Autotune, Crowd or Formant is placed, and
   `kPitchStage` once when Octave or Formant is placed without Autotune.
3. `costs.h`: `kFormant{3, 4, false}`, an estimate. One `FormantPsolaVoice` is about one MOB copy
   (MOB's eight measured 12.3 % mean, so about 1.5 % each), plus the mix and the fine pass. The
   fine pass is counted here so a layout without Autotune is covered; with Autotune placed it is
   counted twice, about one point. `firmware/stress.py` places Formant and sweeps SHIFT, so a
   stress run replaces the estimate.
4. `chain_run`: no change; Formant is a pitch-group card with Engine Pitch and writes nothing at
   its own slot.

## 8. Checks

1. `formant_test`: on a 150 Hz vowel under a 1.1 kHz Gaussian envelope, SHIFT moves the envelope's
   first peak by 2^(st/12) within 5 %; the output pitch stays within 3 cents of the input at every
   SHIFT, with Autotune off and with it on (then on the tuned note); MIX 25/50/75 % within 1 dB of
   MIX 100 %; settled off is a plain copy (`==`); the toggle and SHIFT on switch-on are click-free;
   the silence skip is bit-exact; a NaN in the input clears.
2. `knob_step_test` walks every SHIFT step; `formant_test` holds the toggle.
3. `level_test`: section 6.
4. `core_registry`, `core_layout`: the card, its knobs, its cost row and the tracker and stage
   terms.

## 9. CPU

Host bench 2026-11-02, Adam's clips, ns a sample before and after the fix pass (clang / g++ 15):

| Case | Before | After |
|---|---|---|
| Formant alone, MIX 100 % | 20.8 / 25.1 | 19.4 / 22.0 |
| Formant + Autotune, MIX 100 % | 31.0 / 32.7 | 22.8 / 26.3 |
| Formant + Autotune, MIX 50 % | 31.1 / 32.7 | 31.8 / 34.1 |

At MIX 50 % the three running powers cost about 1.4 ns a sample, 2 to 6 % slower than before.

Layout sums from the cost rows at v2.01.11, after the reverb speed pass (lines 75 % mean, 95 % worst):

| Layout | Mean % | Worst % |
|---|---|---|
| default, Formant in slot 3 | 46 | 60 |
| MOB and Chasm, Formant in slot 3 | 54 | 70 |

## 10. History

1. v2.00.95: built for slot 7, with an equal-power MIX law and one trim (1.92 dB at −3 st).
2. v2.00.96: moved to slot 3 as Octave's alternate.
3. v2.01.00: MIX moved to the linear gains and the level gain over 0.12 s. Equal power read
   +2.58 dB at MIX 50 % near SHIFT 0, and a plain linear crossfade dipped Adam's clips 2.4 to
   3.1 dB at −3 st.
4. v2.01.10 (audit fix pass): the trim table per SHIFT, the 0.04 s power window, the coherence
   seed, the shared grain floor, SHIFT taking the knob on switch-on, the mid-call correction
   read, the silence skip and the NaN clear.
