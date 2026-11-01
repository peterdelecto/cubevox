# Formant card: Adam's throat, bigger or smaller, at the same pitch

Owner 2026-11-01: "let's add a formant shift. spec it out and we will build in a bit." Offered for
slot 7 in place of Gate (owner: "in case a second backup gate is overkill"), through the slot 7 row
(`2026-11-01-slot-7-choices-design.md`). Spec only, not built.

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
   `costs::kFormant`.
2. SHIFT is stepped like Octave's SEMITONES, so 0 is findable and the readout names whole
   semitones. A step lands within one pitch period, because each grain takes the rate at its launch
   and the grains overlap; no extra smoothing.
3. Defaults (level rule, tests, emulator start): SHIFT −3 st, MIX 100 %.
4. MIX is the amount knob, the lower pot, as on every card (owner 2026-10-30).

## 3. Where it acts

1. The shift is a second voice inside the pitch stage (`PitchFx`), so it acts at the first
   pitch-group card's slot, slot 2 in the default layout, wherever its own card sits. Octave works
   this way today. In slot 7 its sound still comes ahead of Octave's voice, MOB, Echo and Distortion.
2. It cannot act at slot 7 itself. The shift cuts grains one pitch period apart; on slot 7's input
   the Echo's repeats, Octave's interval and the distortion would be cut on the same grid and smear.
3. The formant voice reads the pitch stage's input ring, the tracker's period and Autotune's
   correction, the same three things MOB's copies read.

## 4. Engine (`engine/pitch_fx.h`, `engine/formant.h`)

1. `FormantParams { bool on = false; int shiftSt = -3; float mix = 1.0f; float trimDb = 0.0f; }`
   joins `PitchFxParams` as `formant`.
2. The voice is `FormantPsolaVoice` from `engine/crowd.h`, moved to its own header so the Crowd
   card and this card share one copy. Pitch ratio = Autotune's ratio (1 while Autotune is off),
   grain playback rate = 2^(shiftSt / 12), read delay = Autotune's voice delay, so with Autotune on
   both voices line up sample for sample.
3. Range. ±7 st covers a gender swap (about ±3 to ±4 st) and a giant or a child (±6 to ±7 st);
   past that the voice turns to a cartoon. It also bounds the cost: a grain lasts 2p / rate, so
   about 2 / rate grains overlap, three at −7 st and four at −12 st. The read stays behind the
   write head at every shift in range: at +7 st a grain's read moves 0.67 of a period toward the
   head over its life, half of it past the grain's centre, 228 samples at 70 Hz against the 354
   that Autotune's delay leaves there. The voice's read-span clamp (`setReadSpan`) still guards it.
4. Mix law. The base is today's output ahead of Octave (the dry voice, or Autotune's crossfade
   to the corrected voice). `base' = cos(mix·π/2)·base + sin(mix·π/2)·formant`, equal power,
   then Octave's mix as today. The card's toggle drives a 20 ms crossfade (`formantFade_`, the
   `kSmoothCoef` one-pole like `octaveFade_`); toggled off and settled, the voice does not tick.
5. Latency. The formant voice lags the input by 23 to 36 ms, as Autotune's voice does. With
   Autotune off and MIX between 0 and 100 %, Adam's dry voice and the shifted voice sit that far
   apart and read as a tight double. With Autotune on they are aligned.
6. Tracker. It runs while Autotune, MOB or Formant is on, and its 48 kHz fine pass runs while
   Autotune or Formant is on. The formant voice is the lead, so its pitch must be the sung pitch to
   the cent; the coarse lag alone is off by up to about 14 cents at 200 Hz.
7. Enable edge. The voice resets on its enable edge only when `formantFade_` sits at exactly 0
   (engine rule).
8. Memory. `PitchFx` grows by one `FormantPsolaVoice`, about 0.5 kB of DTCM; the ring is unchanged.
   Check `arm-none-eabi-nm -S` after the build.

## 5. Firmware

1. `registry.cpp`: the card entry, `applyKnob` (SHIFT index to `shiftSt = -7 + index`, MIX to
   `mix`), `applyToggle` (`out.pitchFx.formant.on`).
2. `layout.cpp`: `layoutCost` adds `kTracker` once when Autotune, Crowd or Formant is placed.
3. `costs.h`: `kFormant{3, 4, false}`, an estimate. One `FormantPsolaVoice` is about one MOB copy
   (MOB's eight measured 12.3 % mean, so about 1.5 % each), plus the mix and the fine pass, which
   is counted here so a layout without Autotune is covered; with Autotune placed the fine pass is
   counted twice, about one point. The stress run replaces the estimate.
4. `chain_run`: no change; Formant is a pitch-group card with Engine Pitch and writes nothing at
   its own slot.

## 6. Checks

1. `formant_test`: on a steady 150 Hz sawtooth, SHIFT 0 at MIX 100 % keeps the spectrum's
   centroid within 2 % of the input's; ±3 and ±7 st move the spectral envelope's first peak by
   2^(st/12) within 5 %; the output pitch stays within 3 cents of the input at every SHIFT, with
   Autotune off and with it on (then on the tuned note).
2. Settled off is a plain copy: toggle off, settled, output == input of the pitch stage.
3. `knob_step_test`: every SHIFT step and the toggle hold steady-state curvature.
4. `level_test`: SHIFT −3 st, MIX 100 % lands 0 to +0.5 dB RMS on program through `trimDb`.
5. `core_registry`, `core_layout`: the card, its knobs, its cost and the tracker term.
6. Stress run with Formant in slot 7 on the default and MOB + Chasm layouts.

## 7. CPU

| Layout | Mean % | Worst % |
|---|---|---|
| default, Formant in slot 7 | 66 | 75 |
| MOB and Chasm, Formant in slot 7 | 73 | 85 |
| lines | 75 | 95 |
