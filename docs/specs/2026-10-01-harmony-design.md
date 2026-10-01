# cubevox — harmony stage (Zoom V3 model)

Date 2026-10-01. Approved in chat ("lets do psola", "just /go"). Implementation reference.
Builds on `2026-10-01-unison-emulator-design.md` for the engine contract, emulator
skeleton, threading, and build layout. Nothing there changes.

## Purpose

Stage 3 on the panel. Adam uses the Zoom V3's HARMONY (and OCTAVE) effects. The V3
manual (E_V3_2.pdf p.12) describes HARMONY as: a KEY knob with 12 major keys paired
with their relative minors; five harmony voices Lower / Low / Fixed / High / Higher,
each cycling high → medium → low → off, at most two active (a third activation
cancels the first); intervals in scale degrees (Higher +5 or +6, High +3 or +4, Fixed
= key root, Low -3 or -4, Lower -5 or -6); EFFECT ADJUST balances original against
harmony.

Panel for this stage (owner 2026-10-01): **KEY** (detented encoder, detent count does
not matter) and **MIX** (pot). Voice selection lives in the menu.

Updated panel table:

| # | Stage | Knob 1 | Knob 2 |
|---|---|---|---|
| 1 | Input (XLR / 1/4" combo, SM58) | Gain | |
| 2 | Soft gate (always on) | menu | |
| 3 | Harmony | Key (detented encoder) | Mix |
| 4 | Octave | Semitones | Mix |
| 5 | Unison | Depth | |
| 6 | Slapback | Intensity | |
| 7 | Distortion (always on) | Drive | |
| 8 | Gate | Threshold | |
| 9 | Spring reverb | Tension | Dwell |
| 10 | Output | menu | |

## Decisions

1. Shifter is pitch-synchronous overlap-add (PSOLA, Lent 1989 form). It keeps the
   singer's formants, runs in the H7 budget, and is the closest model of what the V3
   does. Owner pick 2026-10-01 over a delay-line shifter and a phase vocoder.
2. Tracker is two-stage YIN: coarse at 12 kHz, refined at 48 kHz. Cost ~12 M MAC/s.
3. Harmony voices are parallel to the sung pitch (ratio from an integer interval), so
   a flat singer gets a flat harmony. A snap-to-scale switch exists in tuning, off.
4. Dry stays zero-latency. Harmony voices lag by `kGrainDelay` plus one sung period,
   about 23 ms at 1000 Hz and 36 ms at 70 Hz. That reads as a backing singer, not a
   flam. Lower `kGrainDelay` if the owner hears one.
5. Interval tables default to pure diatonic thirds and fifths and are editable under
   Tuning so the V3's "3 or 4" can be matched by ear.
6. `engine/pitch.h` and `engine/psola.h` are shared with the later Octave stage.

## Engine

### `engine/pitch.h` — monophonic tracker

```
namespace cv {

struct PitchResult {
  float period = 0.0f;   // samples at kSampleRate; 0 until first voiced frame
  float hz = 0.0f;
  bool voiced = false;
};

class PitchTracker {
 public:
  void reset();
  // Feed n <= kBlock samples. Produces a new result every kHop samples.
  void push(const float* in, int n, float voicedThreshold);
  const PitchResult& result() const;
};

}
```

Behaviour:

1. Constants: `kHop = 256`, `kMinHz = 70`, `kMaxHz = 1000`, so period in
   `[48, 686]` samples. Full-rate ring `std::array<float, 2048>`. Decimated ring
   `std::array<float, 512>` at 12 kHz, fed through a 2-pole Butterworth lowpass at
   2.5 kHz then every 4th sample.
2. Every hop: YIN difference function on the decimated ring, integration window
   `W = 256`, lags `tau in [12, 171]`. Cumulative mean normalised difference
   `d'(tau) = d(tau) * tau / sum_{k=1..tau} d(k)`. Candidate = first `tau` where
   `d'` falls below `voicedThreshold` and is a local minimum; else the global minimum.
   `voiced = min d' < voicedThreshold && frame RMS > -50 dBFS`.
3. Refine at 48 kHz: difference function with `W = 1024` over lags
   `[4*tau - 4, 4*tau + 4]` (clamped to `[48, 686]`), parabolic interpolation on the
   minimum. Result period = that lag, `hz = kSampleRate / period`.
4. Median of the last three voiced periods is published, to suppress octave jumps.
   Unvoiced frames leave `period` and `hz` at their last voiced values with
   `voiced = false`.
5. No allocation, no I/O. All buffers `std::array`.

### `engine/psola.h` — one shifted voice

```
namespace cv {

constexpr int kVoiceRingLen = 4800;              // 100 ms shared input ring
using VoiceRing = std::array<float, kVoiceRingLen>;

class PsolaVoice {
 public:
  void reset();
  // One output sample. writeCount is the absolute index of the sample just
  // written at ring[writeCount % kVoiceRingLen]. period is the sung period in
  // samples (last voiced), ratio the pitch ratio (clamped 0.5..2.0).
  float tick(const VoiceRing& ring, long writeCount, float period, float ratio);
};

}
```

Behaviour:

1. Fixed `kGrainDelay = 1040` samples. Input pitch marks form a grid spaced `period`
   apart; `lastMark_` is the newest mark at or before `writeCount - kGrainDelay`
   (advance `lastMark_ += period` while it falls a full period behind). Marks need no
   epoch detection; any phase works because every grain is cut on the same grid.
2. Output grains launch every `outPeriod = period / ratio` samples. A launched grain
   is centred on the grid mark nearest `writeCount - kGrainDelay`, spans
   `centre ± period`, Hann windowed, read with Catmull-Rom from the ring at absolute
   input positions (so its delay behind the write head stays constant while it
   plays). Pitch up re-uses marks; pitch down skips them.
3. Up to `kMaxGrains = 8` concurrent grains (overlap is `2 * ratio <= 4`); if none is
   free the oldest is dropped. Output is the grain sum times `1 / ratio` (coherent
   overlap scales amplitude by `ratio`).
4. Ring reads never cross the write head: with `kGrainDelay >= 1.5 * 686 + 4` the
   furthest forward read is at least 4 samples behind it.

### `engine/harmony.h` — the stage

```
namespace cv {

enum class HarmonyVoice : uint8_t { Lower = 0, Low, Fixed, High, Higher };

struct HarmonySlot {
  HarmonyVoice voice = HarmonyVoice::High;
  int level = 0;                 // 0 off, 1 low, 2 medium, 3 high
};

struct HarmonyTuning {
  float levelDb[3] = {-12.0f, -6.0f, 0.0f};   // low / medium / high
  float glideMs = 15.0f;                      // interval change glide
  float voicedThreshold = 0.15f;
  bool muteUnvoiced = false;
  bool snapToScale = false;
  // Semitones relative to the sung note, indexed [voice][scale degree 0..6].
  // Fixed has no row; it is computed (see below).
  int8_t lower[7]  = {-7, -7, -7, -6, -7, -7, -7};
  int8_t low[7]    = {-3, -3, -4, -3, -3, -4, -4};
  int8_t high[7]   = { 4,  3,  3,  4,  4,  3,  3};
  int8_t higher[7] = { 7,  7,  7,  7,  7,  7,  6};
};

struct HarmonyParams {
  int key = 0;                   // encoder index 0..11, see kKeyRoot / kKeyName
  float mix = 0.5f;              // 0 dry .. 1 harmony only
  std::array<HarmonySlot, 2> slots{};
  HarmonyTuning tuning;
};

// Encoder order follows the V3 knob: circle of fifths from C.
constexpr int kKeyRoot[12]        = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
constexpr const char* kKeyName[12] = {"C / Am", "G / Em", "D / Bm", "A / F#m",
  "E / C#m", "B / G#m", "F# / D#m", "Db / Bbm", "Ab / Fm", "Eb / Cm", "Bb / Gm",
  "F / Dm"};

class Harmony {
 public:
  void reset();
  void process(const float* in, float* out, int n, const HarmonyParams& p);
  const PitchResult& pitch() const;     // for the emulator readout
};

}
```

Behaviour:

1. Owns one `VoiceRing`, one `PitchTracker`, two `PsolaVoice`. Each sample is written
   to the ring, pushed to the tracker per block, and both voices tick.
2. Scale degree: `midi = 69 + 12 log2(hz / 440)`, `rel = midi - kKeyRoot[key]` folded
   to `[0, 12)`. Major scale offsets `{0, 2, 4, 5, 7, 9, 11}` (relative minor shares
   them). Degree = nearest offset by circular distance; ties snap down.
3. Interval per slot: table row at that degree for Lower/Low/High/Higher. Fixed =
   `-offset[degree]`, and `-12` when the degree is the root. With `snapToScale`
   the target becomes the exact scale note instead (`interval - (rel - offset)`).
4. Target semitones are smoothed per voice by a one-pole with `glideMs` time
   constant; `ratio = 2^(semis / 12)` clamped `0.5..2.0`.
5. Voice gain = `dbToLin(levelDb[level - 1])`, 0 when level 0. `muteUnvoiced` zeroes
   the voice while `voiced == false`. Unvoiced frames otherwise keep shifting with the
   last period.
6. Mix: `dry = cos(mix * pi / 2)`, `wet = sin(mix * pi / 2)`,
   `out = dry * in + wet * (v0 * g0 + v1 * g1)`. `mix` is one-pole smoothed (20 ms).
7. Bypass: `active = 1` if any slot level > 0 else 0, one-pole smoothed (20 ms) and
   snapped to exact 0. Effective `dry = lerp(1, cos(..), active)`, `wet *= active`.
   With no voices the output equals the input sample-exact.
8. Chain order in the emulator and on the box: Harmony, then Unison.

## Emulator (`emulator/main.cpp`)

Panel mirror, above the existing UNISON block:

1. `HARMONY` label, `KEY` combo listing `kKeyName`, `MIX` slider `%.0f %%`
   (opens at 50 %).
2. `Menu` header (open by default; this is a real user-facing menu, not dev-only):
   five rows Lower / Low / Fixed / High / Higher, each a radio group
   Off / Low / Med / High. At most two rows non-off; activating a third sets the
   earliest-activated row to Off, matching the V3.
3. `Tuning` header gains a `Harmony` tree node: level dB (3), glide ms, voiced
   threshold, mute unvoiced, snap to scale, the four 7-entry interval tables as
   `InputInt` cells labelled by degree (Do Re Mi Fa Sol La Ti), and a live readout
   `Detected: <note><octave> <+/-cents> <voiced|unvoiced>` from `Harmony::pitch()`.
   The existing unison sliders move under a `Unison` tree node. `Reset to defaults`
   and `Print tuning` cover both tunings (Print emits both initialisers).
4. `ProtoParams` gains `cv::HarmonyParams harmony`; `ProtoState` gains the pitch
   readout fields. Same double-buffer rules as before.
5. Audio callback: `harmony.process(in, tmp)` then `unison.process(tmp, out)`.

## Render CLI (`tools/render.cpp`)

Existing flags unchanged. New: `--harmony` enables the stage, `--key <0..11>`,
`--mix <0..1>`, `--voice <lower|low|fixed|high|higher>=<0..3>` (up to two). Unison
runs only when `--on 1` or `--depth` is given. Order Harmony then Unison.

## Tests (`test/harmony_test.cpp`, ctest `harmony`)

Test tone = ten harmonics at `1/k` amplitude (vocal-like), peak-normalised to 0.5.

1. Tracker: 110, 220, 440 Hz tones, 1 s each; after 100 ms every published result is
   voiced and within ±5 cents. 1 s of white noise at -20 dBFS: no voiced frame after
   100 ms.
2. Shift: 220 Hz tone, key C, one slot High level 3, mix 1. Measure the output f0
   with a fresh `PitchTracker` over seconds 1–2: within ±10 cents of 261.63 Hz
   (A3 is degree 5, High = +3).
3. Diatonic: tones at 261.63, 293.66, 329.63 Hz, same setup; measured intervals
   +4, +3, +3 semitones each within ±10 cents.
4. Bypass: mix 0 with voices on, and any mix with both slots off: after 100 ms
   `|out - in| < 1e-6` per sample.
5. No allocation inside `process()` (same `operator new` guard as `unison_test`).

Also in ctest, unchanged: `unison`, `proto_layout`.

## Out of scope

Octave stage (next; reuses pitch.h and psola.h), pitch correction, formant shift,
stereo spread of the two voices, live mic input.

## Done when

1. `ctest` passes `unison`, `harmony`, `proto_layout`; output pasted with the claim.
2. `cubevox-proto.app` loads a vocal loop, KEY and MIX and the menu voices behave as
   above, and the owner has listened.
3. Owner either accepts the defaults or pastes a `Print tuning` block to bake.
