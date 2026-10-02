# cubevox — Autotune (pitch correction) + four-column face

Date 2026-10-01. Owner: "add a simple autotune module with key selection and response
time parameter." Adam on the V3's: "U can set the general key and the sensitivity ...
I mean how corrective." Panel: KEY (the box's one key encoder, shared with Harmony)
and RESPONSE. In the prototype Autotune has its own KEY combo plus a `Link key to
Harmony` checkbox (default on).

## Decisions

1. Correction runs inside `PitchFx` on the shared tracker and ring. It REPLACES the
   dry path (corrected voice out, dry crossfaded away by the active smoother) and its
   offset in semitones is added to every Harmony and Octave target, so their voices
   are built on the corrected note.
2. Target note = nearest note of the key's major scale (relative minor shares it) to
   the sung pitch; `chromatic` = nearest semitone. Correction `c = target − sung`
   (semitones, continuous), one-pole smoothed with time constant RESPONSE
   (5–500 ms, log slider, default 60). RESPONSE 5 ms is the hard "mechanical" snap;
   200+ ms is a gentle lean.
3. Shifter: engine A (grid PSOLA) or B (epoch PSOLA, formant preserved, formant 0),
   reusing `engine/shifters.h`. Default B. No granular option (C can't hold a steady
   pitch within a cent).
4. Unvoiced frames: correction holds its last value and the shifter keeps the last
   period, as Harmony does; `muteUnvoiced` not offered.
5. Latency equals the other pitch voices (kGrainDelay + period); the corrected voice
   replaces the dry, so the whole box gains ~25–35 ms when Autotune is on. This is
   inherent to pitch correction and matches the V3.
6. Level rule: `AutotuneTuning::trimDb` set by `level_test` row "autotune key C".

## Engine

### `engine/autotune.h`

```
struct AutotuneTuning {
  float trimDb = 0.0f;
  float maxCorrectSemis = 6.0f;   // never pull more than this (tracker glitch guard)
  ShifterTuning shifter;          // grainPeriods / epochSearch / epochLpHz (B)
};

struct AutotuneParams {
  bool on = true;
  int engine = 1;          // 0 A, 1 B
  bool chromatic = false;
  int key = 0;             // kKeyRoot index, as Harmony
  float responseMs = 60.0f;
  AutotuneTuning tuning;
};

class AutotuneVoice {
 public:
  void reset();
  // Computes the correction from pr; returns it in semitones (0 when inactive).
  float prepare(const PitchResult& pr, const AutotuneParams& p);
  float tick(const VoiceRing& ring, const VoiceRing& lpRing, long writeCount, float period);
  float correctionSemis() const;
};
```

### `engine/pitch_fx.h`

`PitchFxParams` gains `AutotuneParams autotune`. Per block: `c = autotune_.prepare(...)`;
Harmony `prepare` and Octave `prepare` receive `c` and add it to their semitone
targets (new optional argument, default 0 so existing tests hold). Per sample:
`corrected = autotune_.tick(...)`; `base = lerp(in, corrected, activeAt)` (20 ms
smoother, snap); then the existing Harmony/Octave mix uses `base` where it used `in`.
Both-off and Autotune-off paths stay sample-exact.

## Emulator: four columns

Window 1500×840, four columns, label room 150 px:

| Col 1 | Col 2 | Col 3 | Col 4 |
|---|---|---|---|
| INPUT GATE | HARMONY | SLAPBACK | REVERB |
| AUTOTUNE | UNISON | DISTORTION | OUTPUT EQ |
| OCTAVE | | GATE | |

Signal order still reads down then across (Autotune → Octave → Harmony is the
pitch block's internal order; Octave is drawn before Harmony per owner). Column 2
is wider (Harmony menu rows). The accordion rule and the `--layout` probe stay;
probe scenarios gain `autotune`.

AUTOTUNE block: checkbox, `Engine` A | B radio, `KEY` combo (disabled when
`Link key to Harmony` is checked, which copies Harmony's key each frame),
`Chromatic` checkbox, `RESPONSE` slider 5–500 ms log "%.0f ms", readout
`Correction: %+.2f st`. Tuning node: trim, max correction, shifter B fields. Off on
open. Reset/Print cover the tuning.

## Render CLI

`--autotune`, `--atkey <0..11>`, `--response <ms>`, `--atchromatic`, `--atengine 0|1`;
keys `atTrimDb`, `atMaxCorrectSemis`, `atGrainPeriods`, `atEpochSearch`, `atEpochLpHz`.

## Tests (`test/autotune_test.cpp`, ctest `autotune`)

1. Passthrough: off → bit-exact after 100 ms; on with the ten-harmonic tone exactly on
   A3 (220 Hz, key C) → after 300 ms `|out − in|` RMS ≤ −40 dB re input (in-tune input
   is left nearly alone; PSOLA at ratio 1 is not bit-exact but must be close).
2. Correction: tone at 226.4 Hz (A3 + 50 cents), key C, response 20 ms → output f0
   220 Hz ± 5 cents after 300 ms (engine A and B).
3. Chromatic vs scale: tone at 233.1 Hz (B♭3) in key C: scale mode pulls to 220 or
   246.9 (nearest scale note, ±5 cents); chromatic leaves it at 233.1 ± 5 cents.
4. Response time: step from 220 Hz to 226.4 Hz at t = 1 s, response 200 ms → the
   correction reaches 63 % of −50 cents between 150 and 250 ms after the tracker
   first reports the new pitch.
5. Feeds harmony: autotune on, Harmony High level 3 mix 1, tone at A3 + 50 cents → the
   harmony voice lands on C4 (261.6 Hz ± 10 cents), not C4 + 50 cents.
6. No allocation; `level_test` row "autotune key C" in 0..+0.5 dB.

## Done when

`ctest` passes; four-column face with AUTOTUNE below INPUT GATE; owner sings a loop
through it at RESPONSE 5 ms and 200 ms.

## Matched to Adam's box (owner 2026-10-02)

Reference: two clips of Adam through his own autotune (one held note each, A#2 and C#3).
He sings near-monotone and lets the box hold him on the note, artifacts included.

| Source | median off note | 90th pct | wobble on held note (sd) |
|---|---|---|---|
| His box | 2.1-2.3 cents | 6.5-9.6 | 2.7-5.3 |
| Ours before (B, 100 ms) | 24 | 45 | 10 |
| Ours now (A, 1 ms) | 2.4 | 10.6 | 1.7 |

1. Correction reads `PitchResult::rawHz`, the newest frame before the tracker's
   median-of-three. The median cost one hop of lag, about 7 cents on vibrato. Octave
   glitches the median guarded against do not change the correction, since the scale
   repeats every octave.
2. RESPONSE runs 1..500 ms, default 1 ms (hard tune). Slower settings add natural wobble back.
   The knob reads like the V3 (owner 2026-10-02): NATURAL (500 ms) on the left to MECHANICAL
   (1 ms) on the right, log taper, `AutotuneVoice::responseMsAt` / `mechanicalOf`. No MIX knob;
   the footswitch turns it on or off.
3. Default engine A. Engine B drops the share of frames a tracker reads as voiced from 33 %
   to 20 % on Adam's dry loop, i.e. it roughens the voice; A keeps it at 33 %.
4. `autotune_test` hard-tune check: 5 Hz, +-30 cent vibrato on A3 at the defaults must come
   out within 4 cents RMS (3.8 now; 7.3 with the median pitch).
