# cubevox — octave stage + shared pitch front end

Date 2026-10-01. Owner: "make sure the wiring is correct and the fx modules are in
order graphically. let's do octave now." Builds on the harmony spec; the engine
contract, emulator skeleton, and threading rules are unchanged.

## Why a shared front end

Harmony and Octave may run together (panel rule: up to two pitch effects at once).
In series, Octave would track pitch on Harmony's output, which carries the dry voice
plus one or two harmony notes, and a monophonic tracker fed two notes returns
garbage. So both stages track the **dry mic signal** through one tracker and shift
that dry signal; their voices sum, and Unison follows. On the panel this still reads
top to bottom (3 Harmony, 4 Octave, 5 Unison); in the engine 3 and 4 are a parallel
pair.

Tracking speed: the V3 manual (E_V3_2.pdf) has no tracking-speed control on HARMONY,
only the balance knob. The owner expects one. It exists as `glideMs` in
`HarmonyTuning`, surfaced in the emulator as `Tracking speed (ms)`. Panel placement
is the owner's call later; it is a tuning item for now.

## Decisions

1. `engine/pitch_fx.h` owns the input ring, the tracker, and three `PsolaVoice`
   (two harmony, one octave). `engine/harmony.h` keeps the harmony params, key
   tables, and the per-block interval/gain logic as a voice-set helper; `engine/octave.h`
   is the octave counterpart. Neither helper owns a ring or tracker any more.
2. Octave SEMITONES is a stepped knob, −12..+12, centre 0 (owner pick 1). 0 means the
   stage is off; there is no separate switch.
3. Each stage keeps its own MIX, equal-power dry/wet. Dry gain is the product of the
   two stages' dry gains, so Harmony at MIX 100 % with Octave at 0 % still removes the
   dry voice. A stage that is off contributes dry gain 1 and wet 0.
4. Dry stays zero-latency. The octave voice lags like the harmony voices
   (`kGrainDelay` + one period, 23–36 ms). Owner judges by ear.

## Engine

### `engine/harmony.h` (refactored)

Keeps `HarmonyVoice`, `HarmonySlot`, `HarmonyTuning` (unchanged fields), `HarmonyParams`
(unchanged), `kKeyRoot`, `kKeyName`. The class becomes:

```
class HarmonyVoices {
 public:
  void reset();
  // Called once per block before ticking. Computes per-voice target semitones and
  // gains from the tracker result and the params; returns true if any slot is on.
  bool prepare(const PitchResult& pr, const HarmonyParams& p);
  // One sample of both harmony voices summed, gains and glide applied.
  float tick(const VoiceRing& ring, long writeCount, float period);
};
```

Semantics identical to the previous `Harmony` (degree snap with ties down, Fixed
rule, snapToScale, per-voice log-domain glide at `glideMs`, level dB, muteUnvoiced,
fresh-start smoothers, silent voices jump to their new interval).

### `engine/octave.h`

```
struct OctaveTuning {
  float levelDb = 0.0f;
  float glideMs = 15.0f;
  bool muteUnvoiced = false;
};

struct OctaveParams {
  int semitones = 0;          // panel knob, -12..12, 0 = off
  float mix = 0.5f;           // panel knob
  OctaveTuning tuning;
};

class OctaveVoice {
 public:
  void reset();
  bool prepare(const PitchResult& pr, const OctaveParams& p);   // true if semitones != 0
  float tick(const VoiceRing& ring, long writeCount, float period);
};
```

Ratio `2^(semis/12)` with `semis` one-pole glided at `glideMs`; gain from `levelDb`,
0 when off or (muteUnvoiced && !voiced).

### `engine/pitch_fx.h`

```
struct PitchFxParams {
  HarmonyParams harmony;
  OctaveParams octave;
};

class PitchFx {
 public:
  void reset();
  void process(const float* in, float* out, int n, const PitchFxParams& p);
  const PitchResult& pitch() const;
};
```

Behaviour per block: push `in` to the tracker (threshold from `harmony.tuning`;
since `2026-10-08-pitch-cpu-pass-design.md` the tracker's buffers fill every block
but its analysis runs only while Autotune, Harmony or Octave A/B is on);
`prepare` both helpers; per sample write the ring, tick both helpers,
`out = dryH * dryO * in + wetH * harm + wetO * oct` where for each stage
`dry = 1 + (cos(mix*pi/2) - 1) * active`, `wet = sin(mix*pi/2) * active`, `mix` and
`active` one-pole smoothed (20 ms) with snap to exact, as before. Both stages off =
sample-exact passthrough.

## Emulator (`emulator/main.cpp`)

Face order top to bottom, mirroring the panel: transport row; **HARMONY** (KEY, MIX)
with its `Menu` voice rows directly under it; **OCTAVE** (`SEMITONES` int slider
−12..+12 shown as `%+d st`, `MIX` `%.0f %%`, opens at 0 st / 50 %); **UNISON**
(unchanged); `Tuning` header with `Harmony` (glide slider relabelled
`Tracking speed (ms)`), `Octave` (level dB, tracking speed ms, mute unvoiced), and
`Unison` nodes, Reset / Print covering all three. `ProtoParams` holds
`cv::PitchFxParams pitchFx` and `cv::UnisonParams unison`; the callback runs
`pitchFx.process(in, tmp)` then `unison.process(tmp, out)`.

## Render CLI

Add `--octave <-12..12>` and `--omix <0..1>`. Any of `--harmony` / `--octave` turns the
pitch front end on. Order PitchFx then Unison.

## Tests

`test/harmony_test.cpp` is updated to drive `PitchFx` with octave at 0; all five
checks unchanged. New `test/octave_test.cpp` (ctest `octave`):

1. Octave up: 220 Hz test tone, `semitones = 12`, mix 1 → output f0 (fresh
   `PitchTracker`, seconds 1–2) within ±10 cents of 440 Hz. Octave down: −12 → 110 Hz.
2. Fifth: +7 → 329.63 Hz ±10 cents.
3. Bypass: `semitones = 0` at any mix, harmony off → `|out - in| < 1e-6` after 100 ms.
4. Wiring: 220 Hz tone, octave +12 mix 1 AND harmony High level 3 mix 1 at the same
   time; `PitchFx::pitch()` reports 220 Hz ±5 cents voiced on every hop after 100 ms
   (the tracker sees only the dry signal).
5. No allocation inside `process()`.

## Done when

1. `ctest` passes `unison`, `harmony`, `octave`, `proto_layout`; output pasted.
2. Emulator face shows Harmony, Octave, Unison in that order; owner listens.

Level rule (2026-10-01, see `2026-10-01-level-rule.md`): `OctaveTuning::trimDbA` (new, default -5.3 dB) adds to `levelDb` for engine A; A -12 at MIX 0.5 reads +0.26 dB out/in.
