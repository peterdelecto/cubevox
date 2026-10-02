# cubevox — Autotune (pitch correction) module

Date 2026-10-01. Owner: "add a simple autotune module with key selection and response
time parameter." Adam (text exchange): "U can set the general key and the sensitivity"
= "how corrective". On the Zoom V3 the one KEY knob serves harmony and pitch correct.

## Decisions

1. Lives on the shared pitch front end (`PitchFx`): the tracker already gives the sung
   pitch; correction = nearest scale note (key's major scale = relative minor's notes,
   as Harmony) or nearest semitone when `chromatic`.
2. RESPONSE (ms) is the one-pole time constant on the correction in semitones. Short
   (≤ 10 ms) snaps hard; long (100–300 ms) nudges. Default 40 ms.
3. The corrected voice is produced by an `EpochShifter` (formant preserved, formant
   0) and REPLACES the dry voice at PitchFx's output while the module is on. Octave
   and Harmony add the same correction to their own targets so they build on the
   corrected note. Their shifters still read the raw ring (one shift each, no
   cascade).
4. KEY: `AutotuneParams::key` with `followHarmonyKey = true` by default (the panel has
   one encoder for both; the prototype shows a combo greyed while following).
5. Unvoiced frames: correction target holds; the shifter keeps the last period as
   elsewhere. `muteUnvoiced` not offered.
6. Level rule row "autotune key C response 40" with its own `trimDb`.

## Engine

### `engine/autotune.h`

```
struct AutotuneTuning {
  float maxCorrectionSemis = 2.0f;   // never pull more than this (owner may widen)
  float formant = 0.0f;              // semitones, 0 = same singer
  float trimDb = 0.0f;               // level rule
  int   engine = 1;                  // 0 grid PSOLA, 1 epoch PSOLA (default)
};

struct AutotuneParams {
  bool on = true;
  bool followHarmonyKey = true;
  int key = 0;                       // kKeyRoot index, used when !followHarmonyKey
  bool chromatic = false;
  float responseMs = 40.0f;          // panel knob, 1..300 (log slider)
  AutotuneTuning tuning;
};

class Autotune {
 public:
  void reset();
  // Per block: computes the target correction from the tracker result.
  void prepare(const PitchResult& pr, const AutotuneParams& p, int harmonyKey);
  float correctionSemis() const;     // smoothed, for Octave/Harmony to add
  // Per sample: the corrected voice (wet only).
  float tick(const VoiceRing& ring, const VoiceRing& lpRing, long writeCount, float period);
};
```

Behaviour:

1. `midi = 69 + 12 log2(hz/440)`; target note = nearest scale note (circular distance,
   ties down, as Harmony's degreeOf) or `round(midi)` if chromatic; `corr = target −
   midi` clamped to ±maxCorrectionSemis. Smoothed per sample by a one-pole with
   `responseMs`; snaps to the target on the first voiced frame after reset.
2. Shifter target semis = smoothed corr; gain = dbToLin(trimDb).
3. Float only, `std::array`, no heap/I/O.

### `engine/pitch_fx.h`

`PitchFxParams` gains `AutotuneParams autotune`. Per block: `autotune.prepare(pr, p,
p.harmony.key)`; Harmony and Octave `prepare` receive `corrOffset = autotune.on ?
autotune.correctionSemis() : 0` and add it to their targets. Per sample: `dryOut =
active_at · at.tick(...) + (1 − active_at) · in` (20 ms smoother with snap, so off is
bit-exact) and the existing mix maths uses `dryOut` where it used `in`.

## Emulator (four columns)

Eleven modules no longer fit three columns with any Tuning open. Face becomes FOUR
columns at 1440×840 (Harmony no longer needs the wide interval rows). Signal order
down then across:

| Col 1 | Col 2 | Col 3 | Col 4 |
|---|---|---|---|
| INPUT GATE | HARMONY | SLAPBACK | REVERB |
| AUTOTUNE | UNISON | DISTORTION | OUTPUT EQ |
| OCTAVE | | GATE | |

AUTOTUNE block: checkbox, `KEY` combo + `Follow Harmony` checkbox (combo greyed when
following), `Chromatic` checkbox, `RESPONSE` log slider 1–300 ms "%.0f ms". Tuning:
max correction, formant, engine A|B, trim. Reverb's Chasm and Parker Drive nodes split
in two if column 4 overruns. `--layout` probe: four columns, scenarios for the new
nodes, accordion per column unchanged. CLAUDE.md face rule → four columns.

## Render CLI

`--autotune` enables; `--atkey <0..11>` (implies not following), `--atchrom`,
`--response <ms>`; keys `atMaxCorrectionSemis`, `atFormant`, `atTrimDb`, `atEngine`.

## Tests (`test/autotune_test.cpp`, ctest `autotune`)

1. Snap: 440 Hz × 2^(+35/1200) (35 cents sharp of A4), key C, response 5 ms → output
   f0 (fresh tracker, seconds 1–2) within ±5 cents of 440.00.
2. Scale: 466.16 Hz (B♭4, not in C major) key C → output within ±5 cents of either
   440 (A) or 493.88 (B), and specifically 440 (ties snap down / nearest is A at
   −100 cents vs B at +100: equidistant → down → A). Chromatic → stays 466.16 ± 5 cents.
3. Response: step from 440 to 440×2^(+40/1200) at t = 1 s, response 200 ms: measured
   correction (output cents vs input cents over 20 ms windows) reaches 63 % of −40
   cents at 200 ± 40 ms after the step.
4. Harmony follows: autotune on + Harmony High in C on the 35-cents-sharp A4 → harmony
   voice lands on C5 (523.25) ± 10 cents, not on C5 + 35 cents.
5. Bypass: `on = false` → bit-exact; max correction 0 → output f0 equals input.
6. No allocation. `level_test` row.

## Done when

`ctest` passes; four-column face; owner tries RESPONSE from 1 ms (robot) to 300 ms.
