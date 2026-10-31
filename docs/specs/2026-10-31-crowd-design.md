# cubevox — Crowd card (four formant-shifted voices, WIDTH and VOICES)

Date 2026-10-31. Owner design in chat 2026-10-31; this file is the implementation reference.
The wiring map with file and line references is `build/audit/2026-10-31-doubler-map.md` (gitignored).

## Purpose

Adam sings in a speaky, near-monotone style, so effects that need a key do not serve him. The
Crowd card makes a crowd out of his own voice. It adds up to four copies, each a different person:
owner 2026-10-31, "we are supposed to be offsetting by a few semitones and formants to give the
effect of the voice belonging to a totally different person. then we use a little lfo or delay to
make it sound like it's not the same performance." Each copy has a pitch offset of a few
semitones, a formant offset (a throat) and its own tone shelf, lower people with longer throats
and higher people with shorter ones. Its own lag behind the lead makes it a different performance.
The crowd as a whole has a fixed low cut. It never reads the key. It reads the pitch period from the
tracker Autotune uses. The goal is the effect of several other people saying the same lines as
Adam (owner 2026-10-31, see Simplified under Built).

The card cuts on one pitch, between about 70 Hz and 1 kHz, so a single melodic line works through it
and a chord does not. The bottom tenth of WIDTH keeps that line in tune (WIDTH law below).

## Decisions (owner 2026-10-31)

1. A new library card and its own effect. It is not a replacement for Unison and is not compared
   against it (owner 2026-10-31); which slot it takes is a layout decision for the owner. This
   spec leaves the default layout as it is.
2. Lower pot VOICES, the amount knob. 0 is dry. The four voices fade in one at a time, then the
   lead fades out, so the top of the knob is the crowd alone. Owner: "if the effect is dimed, we
   don't hear Adam, we hear the crowd."
3. Upper pot WIDTH. It spreads the copies' pitch and formant offsets and stretches their lags, so the
   bottom is four plain delayed doubles of the lead on its own pitch and the top is the crowd at its
   full offsets and longest lags (owner 2026-10-31, "nix
   the distance knob and replace it with a 'width knob'. the width knob will spread the voices.").
   The bottom tenth is for instruments and melodic lines (WIDTH law below).
4. Autotune is not expected on with this card. The tracker runs whenever the Crowd card is on.
5. The voices read the pitch front end's ring and add Autotune's correction to their ratio, as
   Octave does. Octave's wet is not doubled.
6. Four voices with linear grain reads, benched first. If over budget, three voices before any
   CPU pass elsewhere.
7. Made in this spec, for the owner to confirm: the lead leaves from 80 % of the knob, not 85 %,
   so each voice owns a round fifth of the travel and the percent readout tells the count
   (20 % one voice, 40 % two, 60 % three, 80 % four).
8. The card is named Crowd (owner 2026-10-31).

## Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | WIDTH | continuous, 0..100 % | `WIDTH 35 %` |
| lower (knob B) | VOICES | continuous, 0..100 % | `VOICES 60 %` |

Card id `crowd`, name `Crowd`, Group Pitch, Engine Crowd. Both knobs are the registry's
percent knob with no decimals, so the readout sweep test holds as it is.

### VOICES law, knob position k in 0..1

1. Span A, k in [0, 0.8]. u = k / 0.2 runs 0..4. Voice v (0..3, fade order below) has weight
   w_v = sin(π/2 · clamp01((u − v) / holdFrom)). Each voice fades in over the first three
   quarters of its fifth and holds for the last quarter.
2. Span B, k in (0.8, 1]. s = (k − 0.8) / 0.2. Every w_v = 1. Crowd power rises from P_4 to 1
   and the lead falls to 0.
3. Level law at every k. Each voice carries the fixed gain g = 10^(voiceDb / 20); with voiceDb
   −9 dB four voices sum to the power P_4 = 4 g² ≈ 0.50. Crowd power is P = Σ (g w_v)² in span A
   and P = P_4 + (1 − P_4) · sin²(π/2 · s) in span B. The lead gain is sqrt(1 − P). The crowd
   multiplier is sqrt(P / Σ (g w_v)²), which is 1 in span A and reaches sqrt(1 / P_4) at the top,
   so each voice goes from −9 dB to −6 dB there. Total power is unity at every k, so the level
   rule holds by construction; trimDb in tuning takes up the residue level_test measures on the
   180 Hz program.
4. Fade order, low, high, low, high, by voice index (full pitch, throat, delay before the lag scale):
   0 (−1.75 st, −1.75 st, 40 ms), 1 (+3 st, +1.65 st, 100 ms), 2 (−3 st, −1 st, 70 ms),
   3 (+1 st, −1.5 st, 75 ms). One voice is a second singer, two a pair, four the crowd. Voice 1 is the
   owner's higher voice (owner 2026-10-31, "voice 2" in the owner's count): its tone shelf is flat, and
   it carries a tilt of +2 dB and a shimmer of 1 dB (Per-copy tilt and shimmer, below). The other three
   carry neither.
5. Gains are computed per block and ramped per sample across the block, as Octave and the reverb
   MIX do. A voice whose ramped gain sits at 0 is reset and does not tick, so one voice costs a
   quarter of four.

### WIDTH law, knob w in 0..1

Owner 2026-10-31: "i'd like 'width' from about 0 to 10 % to be more suitable for instruments or
melodic lines." And: "from about 0 to 10 percent, we need much less detuning. for a melodic
line, should the user want to pipe an instrument through that effect, we don't want it to be out of
tune."

Owner 2026-10-31, after a piano loop through the bottom tenth sounded discordant: "we might want to
start with a baseline of 0 st per voice and maybe even 0 formant at 0 on the width slider." And on
the top of the knob: "at max width, add a little bit more time spread between voices, maybe adding
20 ms to the max."

WIDTH scales the pitch offset, the formant offset, the drift and the contour of every copy, and it
stretches the lags. The pitch and formant offsets take the same factor f(w), which is linear between
three points. Per copy and per block, copy v's pitch offset in semitones is pitchSt[v] × f(w) and its
formant offset is formantSt[v] × f(w).

| Point | f (pitch and formant factor) |
|---|---|
| w = 0 | 0 |
| w = `widthKnee` (0.1) | `widthKneeFrac` (0.012) |
| w = 1 | 1 |

Between the points f is linear in w, so f(0.5) = 0.012 + 0.988 × (0.4 / 0.9) = 0.451. At w = 0 f is
exactly 0, so every copy sits at pitch ratio exactly 1 with a grain playback rate of exactly 1, and
the slicing returns the input delayed by the copy's lag (test 19). A copy whose ratio and rate are both
exactly 1 keeps its marks on the period grid and skips the epoch snap, so it reproduces the delayed
input to float rounding with epoch align on, and the snap resumes as soon as either differs from 1.
A melodic line or an instrument gets four plain delayed doubles on its own pitch. At w = 0.1 the −3 st copy sits 3.6 cents off the
lead (−0.036 st) and carries 1.2 % of its formant move. At w = 1 every copy sits at pitchSt[v] and
formantSt[v]. A pitchSt or formantSt of 0 stays 0. Tone shelves do not follow the knob. The w the
factor reads follows the knob through the 20 ms one-pole of `engine/smooth.h`, updated per block, so a
knob step glides the copies over a few blocks and does not jump their pitch in one. It takes the knob
value outright on the first block after a reset, and a settled w equals the knob bit for bit.
`widthKnee` is clamped to 0.02..0.9 and `widthKneeFrac` to 0..1.

Two more scalings keep the bottom in tune, since a slow pitch walk or a contour that bends a copy
away from the lead would sound out of tune on a melodic line.

1. Drift follows f. The drift range in cents is driftCents × f(w): 0 cents at w = 0, 0.18 cents at
   the knee and the full driftCents at w = 1.
2. Contour follows f. The contour a copy uses is 1 + (contour[v] − 1) × f(w): exactly 1.0 at w = 0,
   within 0.5 % of 1.0 at the knee and the slider value at w = 1.

The tilt and the shimmer of every copy, voice 5 included, scale by the same f(w) (owner 2026-10-31): the tilt
in dB and the shimmer in dB applied are the settings times f(w), exactly 0 at w = 0 and the settings at w = 1.

At w = 1 f is exactly 1, so the drift and the contour are what they were before the scaling. Timing
scatter, loudness scatter, the tone shelves, epoch alignment, VOICES and solo do not follow the knob.

Lag multiplier. WIDTH also moves every lag by one multiplier m(w), linear in w. m(0) is
`widthLagBottom` (0.5). m(1) is 1 + `widthLagMs` / L, where L is the longest delayMs[v] × `lagScale`
(120 ms at the defaults) and `widthLagMs` is 20 ms, so the longest copy sits 20 ms further back at
w = 1 than `lagScale` alone puts it and the others sit further back in proportion. At the defaults
m(1) = 1.1667 and m reaches 1 at w = 0.75. The lags are 24, 60, 42 and 45 ms at WIDTH 0 and 56, 140, 98
and 105 ms at WIDTH 1. m reads the knob value and not the smoothed w, so the target steps with the
knob, and each copy takes the new lag at its next quiet gap (Lag law below). `widthLagBottom` is
clamped to 0.2..1 and `widthLagMs` to 0..60.

### Lag law

Copy v's lag target is delayMs[v] × `lagScale` (1.2) × m(w) (WIDTH law above), capped at `maxLagMs`
(200 ms) and floored at the voice's own minimum (1.5 periods plus 4 samples). Only WIDTH moves it. At
the defaults the four copies lag 24, 60, 42 and 45 ms at WIDTH 0 and 56, 140, 98 and 105 ms at WIDTH 1.

A copy changes its lag only in a quiet gap. Each block it measures the mean square of the ring at
its read point (the block's samples `lag` behind the head). While a new target is pending (a tuning
move, or a fresh scatter draw), the copy counts the blocks in which the ring is under
`gapFloorDb` (−50 dBFS) both at the lag it plays and at the lag it would move to. After two such
blocks it takes the new lag at once by resetting its grain marks there, and the grains in flight
finish. The jump then replays or skips only silence. While material sounds it keeps its lag, so a
copy never bends in pitch mid-word. The first lag after a reset or a fade-in lands at once, since
nothing is playing yet. A gap shorter than the move (the gap between the two read points) never
admits the move, so a large lag step waits for a long pause.

Scatter. With `scatterMs` above 0, each gap gives the copy a fresh uniform random offset in
[−scatterMs, +scatterMs] on top of its target, drawn from a per-voice xorshift32 seeded at reset,
so a run repeats. The copy takes one offset per gap.

Pitch drift. With `driftCents` above 0, each copy holds a drift in cents that walks inside
[−driftCents × f(w), +driftCents × f(w)] (WIDTH law above). A new random target comes every `driftHoldS` (2 s) and the drift slews
toward it with a time constant of 1 s, updated per block. The pitch ratio gains the term
2^(drift / 1200) through exp2Fast. With `driftCents` at 0 the term is exactly 0 and the ratio is
the same as without the code.

Filter coefficients update per block only, since the setters call libm.

## Engine `engine/crowd.h`

### Interface

```
namespace cv {
struct CrowdTuning {
  std::array<float, 4> pitchSt     = {-1.75f, 3.0f, -3.0f, 1.0f};    // full pitch offset 2^(st / 12) at WIDTH 1
  std::array<float, 4> formantSt   = {-1.75f, 1.65f, -1.0f, -1.5f};  // grain playback rate 2^(st / 12)
  std::array<float, 4> tiltDb      = {-3.0f, 0.0f, 3.0f, 5.0f};       // high shelf gain above tiltHz
  std::array<float, 4> tilt        = {0.0f, 2.0f, 0.0f, 0.0f};        // whole-spectrum tilt in dB at WIDTH 1, -6..6, 0 skips it
  std::array<float, 4> shimmer     = {0.0f, 1.0f, 0.0f, 0.0f};        // per-cycle random gain in dB at WIDTH 1, 0..1, 0 draws nothing
  std::array<float, 4> delayMs     = {40.0f, 100.0f, 70.0f, 75.0f};  // lag behind the lead before lagScale
  float lagScale     = 1.2f;      // a copy's lag is delayMs times this times the WIDTH multiplier
  float tiltHz       = 2500.0f;
  float voiceDb      = -9.0f;     // one voice against the lead at unity
  float trimDb       = 1.0f;      // level rule residue
  float leadFadeFrom = 0.8f;      // VOICES position where the lead starts to leave
  float holdFrom     = 0.75f;     // part of each voice's fifth spent fading; the rest holds
  float hpHz         = 120.0f;    // low cut on the crowd sum
  float maxLagMs     = 200.0f;    // ceiling on a copy's lag
  float gapFloorDb   = -50.0f;    // a read point under this level counts as a gap
  float scatterMs    = 12.0f;     // peak random lag offset taken at each gap, 0..30
  float driftCents   = 15.0f;     // peak of each copy's slow random pitch walk, 0..50
  float driftHoldS   = 2.0f;      // time between new drift targets
  float widthKnee    = 0.1f;      // WIDTH where the pitch and formant factor reaches widthKneeFrac, 0.02..0.9
  float widthKneeFrac = 0.012f;   // that factor at the knee, 0..1 (it is 0 at WIDTH 0)
  float widthLagBottom = 0.5f;    // lag multiplier at WIDTH 0, 0.2..1
  float widthLagMs   = 20.0f;     // the longest lag grows by this at WIDTH 1, 0..60
};
struct CrowdParams {
  bool on = false;
  float width    = 1.0f;   // knob A, 0..1
  float voices   = 0.5f;   // knob B, 0..1
  CrowdTuning tuning;
};
class Crowd {
 public:
  void reset();
  // in is this slot's input, the lead. pitch has already run this block, so the block's
  // samples sit at ring index head() - n + i.
  void process(const float* in, float* out, int n, const CrowdParams& p, const PitchFx& pitch);
};
}
```

Off: out equals in bit for bit.

### Per-voice chain

ring → voice (PSOLA grains at playback rate f_v, pitch ratio r_v, read delay D_v) → tilt → tone shelf →
× g w_v → crowd sum. The tilt and the shimmer (a random gain on each grain) are described under
Per-copy tilt and shimmer, below; at 0 the tilt stage is skipped and the voice draws no shimmer.

1. Voice. A new class in crowd.h, a PSOLA voice with a grain playback rate. It is built from the
   deleted `EpochPsolaVoice` (`git show 9b715ec^:engine/shifters.h`, launch at its lines 143-152,
   play at 182-193) and the period-grid mark scheduling of today's `PsolaVoice`
   (engine/psola.h `advanceMarks`). Grain length 2p, Hann from `HannRotor`, launch interval
   p / r_v, grain duration 2p / f_v, a per-sample read at a fractional delay that moves by
   (1 − f_v) per sample, grain gain f_v · 2 / (len / p) / r_v. Reads are linear, two taps, not
   Catmull-Rom (decision 6). No epoch search in the first cut: grains sit on the period grid as
   today's PsolaVoice does. If the crowd sounds rough in the emulator, the fallback is one shared
   low-passed ring and the old peak search, with the marks shared across the four voices.
2. Read delay D_v is the copy's adopted lag in samples (lag law above), a per-voice member
   set per block, not `PsolaVoice::kGrainDelay`. The voice clamps it to at least 1.5 × the clamped
   period + 4 (the floor kGrainDelay encodes for the lowest pitch) and at most
   kVoiceRingLen − 2 × kMaxPeriod − 8, which is 9,660 samples (201 ms) on the 230 ms ring. The
   defaults keep every copy above the 21.7 ms floor (the shortest lag is 24 ms, at WIDTH 0) and the 200 ms ceiling leaves the longest grain
   (686-sample period, rate 0.5) inside the ring.
3. Ratio r_v = 2^(pitchSt[v] · f(w) / 12) · 2^(drift_v / 1200) · 2^(pitch.correctionSemis() / 12),
   and the grain playback rate is 2^(formantSt[v] · f(w) / 12) (WIDTH law above). The PSOLA voice
   clamps the ratio to 0.5..2.0.
4. Period from `pitch.pitch().period`. Period 0 gives silence from the voice, as PsolaVoice does
   today. Unvoiced frames keep the last period, as Autotune's voice does.
5. Biquads from dsp.h. Each voice has one tone shelf, `setHighShelf(tiltHz, tiltDb[v])`, set at
   reset and when the tuning changes, never per sample. The crowd sum has one `setHighPass(hpHz,
   kButterworthQ)`, set at reset and when the tuning changes.

### Output

out[i] = lead(i) · in[i] + crowdMul(i) · dc(crowd[i]) · 10^(trimDb / 20), with lead and crowdMul
ramped per sample from the previous block's values to this block's. A 5 Hz DcBlocker sits on the
wet sum before it meets the lead, per the output-safety spec.

## Wiring

1. `Card::Crowd` appended before `Count`; `Engine::Crowd` appended before `Count`.
   kEngineCount 7 → 8 and kFadeHoldBlocks follows (core/layout_fade.h). Six switch statements gain
   a case: registry.cpp applyKnob and applyToggle, chain_run.cpp resetEngine and runStage,
   test/knob_step_test.cpp runCard, test/firmware/core_registry.cpp engineValue.
2. Registry row: card Crowd, id "crowd", name "Crowd", Engine Crowd, two knobs
   kPercent("WIDTH") and kPercent("VOICES"), Group Pitch, costs::kCrowd. applyCrowdKnob maps
   knob 0 to width and knob 1 to voices. applyToggle sets `params.crowd.on` and
   `params.pitch.crowdOn`.
3. `PitchFxParams::crowdOn` (new, default false). pitch_fx.h line 49 becomes
   `tracker_.push(in, n, p.autotune.on || p.crowdOn || activeAt_ > 0.0f)`. PitchFx gains
   `const VoiceRing& ring() const` and `int head() const` (the next write index, already wrapped).
4. chain_run: PitchFx runs at the first slot whose card is in Group Pitch (Autotune, Octave,
   Crowd), with that slot's input, when any Group Pitch toggle is on. Today's rule keys on
   Engine Pitch; it becomes the group, so a Crowd card placed ahead of both pitch cards, or alone,
   still gets a written ring and a running tracker. runStage's Crowd case calls
   `engines.crowd.process(in, out, n, params.crowd, engines.pitch)`. The Crowd card's own slot
   is where the crowd joins the chain, so Slapback, Distortion, Gate and the reverb act on it.
5. ChainEngines gains `Crowd crowd` as a plain global, not DTCM. chain_params gains `crowd`
   with on = false. layout.cpp gets no new rule and defaultLayout is unchanged.
   core_registry.cpp amountCards gains Crowd (knob B is the amount) and the pitch group count
   becomes 3.
6. costs.h: `kCrowd{19, 21, false}` as the host estimate until a board bench, with the comment
   "estimate: four voices at ~28k, EQ ~6k, room ~14k". test/firmware/core_layout.cpp requires
   `measured` only for cards in `defaultLayout()`; a card outside it may carry an estimate, which
   the app already shows as "est.". layoutValid still counts the estimate, so a layout over the
   lines is refused until the bench lowers the row or the engine is cut to fit.
7. The record stores the id string, so flash layouts are unaffected. app/fixtures/hello.json is
   refreshed by `hello_fixture_refresh`; app/internal/server/server_test.go card counts move
   10 → 11.

## Emulator and render tool

1. cubevox-proto: a CROWD block (checkbox, WIDTH and VOICES percent sliders, a Tuning header
   with tuning nodes) in whichever column the layout probe accepts; state keys
   `crowd.on`, `crowd.width`, `crowd.voices`; a `gCrowd` global reset with the others;
   chain position after the pitch stage and before Unison, with gPitchFx passed in. Pedal mode is
   unchanged by this spec; the owner listens to the card in the module view.
   `--layout` must print ok. The tuning nodes are Formants (4 sliders, −4..+4 st), Pitch (4,
   −12..+12 st, the full values), Delay (4, 5..200 ms, lagScale 0.5..2, Lag at bottom 0.2..1 and Lag at top
   0..60 ms), Tone (4 tiltDb, tiltHz and the 4 tilts in one row, -6..+6 dB), Scatter (scatterMs 0..30 ms,
   driftCents 0..50 and 4 shimmers, 0..1 dB) and Solo (4 checkboxes). Print tuning lists every CrowdTuning field. The Crowd
   tuning has no state keys.
2. cubevox-render: `--crowd`, `--width <0..1>`, `--voices <0..1>`; tuning keys prefixed `crowd`
   (`crowdVoiceDb`, `crowdTrimDb`, `crowdFormantSt0..3`, `crowdPitchSt0..3`, `crowdTiltDb0..3`,
   `crowdTiltHz`, `crowdDelayMs0..3`, `crowdLagScale`, `crowdScatterMs`, `crowdDriftCents`, `crowdHpHz`,
   `crowdMaxLagMs`, `crowdWidthLagBottom`, `crowdWidthLagMs`). `pitchOn` becomes true when the crowd card is on. tools/engine_ab.sh gains a
   `crowd` case and a crowd term in the chain case.

## Tests

1. `test/crowd_test.cpp` (CMake line `cubevox_engine_test(crowd)`), on a PitchFx fed the test
   signal through its pass-through path so the ring and tracker are live:
   1. off is bit exact (`==`, after 100 ms).
   2. no allocation inside process (the operator new abort pattern of octave_test.cpp).
   3. pitch moves by pitchSt: one voice alone (VOICES 0.2, WIDTH 1): output f0 within 10 cents
      of 220 × 2^(pitchSt / 12) on a 220 Hz tone, for each of the four voices (Goertzel helpers
      copied from octave_test.cpp).
   4. formant moves: centroid over harmonics 1..10 at +1 st (the probe holds the lag at 90 ms) at
      least 1.03 × the formant-0 centroid, and at voice 0's (−1.75 st) at most 0.94 ×.
   5. level law: total RMS at VOICES 0.2, 0.4, 0.6, 0.8 and 1.0 within −1.5..+1.0 dB of the input
      on the 180 Hz program.
   6. the lead leaves: at VOICES 1.0 a probe accessor reports lead gain 0 after the ramp.
   7. voice count: at VOICES 0.2 only voice 0 ticks and at 0.8 all four (probe accessor).
   8. the lag follows lagScale across a gap: on 220 Hz bursts of an irregular envelope with 120 ms
      of silence between them, one copy alone (formant 0, pitchSt 0) at WIDTH 1 has its cross-correlation
      peak at delayMs × 1.2 × m(1) (105 ms for copy 4, 140 ms for copy 2) and, after lagScale goes to 2,
      at delayMs × 2 × m(1) capped at 200 ms (165 ms and 200 ms). The move comes while the copy's
      output sounds the burst, 100 ms after its first sample, the lag stays unchanged to the end of that
      burst, and the next burst sounds at the new lag (±0.5 ms).
   9. tracker runs without Autotune: PitchFxParams with autotune.on false and crowdOn true gives
      period > 0 on the tone within 60 ms.
   10. voices differ: voice 0 alone and voice 1 alone on the 220 Hz tone differ in spectral
      centroid (harmonics 1..20 of each voice's own f0) by at least 5 % (25.6 % at the defaults).
   11. scatter: scatterMs 15 on copy 4 over ten bursts at WIDTH 1, each burst's lag within ±15 ms of 105 ms
      and varying from burst to burst; f0 inside each burst within 10 cents of the copy's offset.
   12. drift: driftCents 20 on a 6 s tone, the copy's f0 in 0.5 s windows within ±20 cents of its
      offset (3 cents of measurement slack) and not the same in every window. driftCents 0 leaves
      the drift at exactly 0 and the output unchanged by driftHoldS.
   13. WIDTH law: the −3 st copy (formant −1 st) alone at WIDTH 0, 0.1, 0.5 and 1 has its f0 at
      −3 st × f(w) from the lead's 220 Hz (within 1, 1.5, 10 and 10 cents; 0 cents, −3.6 cents,
      −135 cents and −300 cents) and its grain playback rate, read from the engine, at f(w) of its
      full formant move (within 0.5 %; 0, 1.2 %, 45.1 % and 100 %). At WIDTH 0 the output equals the
      output of a copy with no pitch or formant offset sample for sample. Its lag follows m(w) at
      all four.
   14. fade order: at WIDTH 1 and at WIDTH 0.5, at VOICES 0.2, 0.4, 0.6 and 0.8 the output has a
      spectral line at the f0 of each voice that has come in (at least 0.4 of the loudest line) and
      none at the f0 of the voices still to come (at most 0.1), so the order is −1.75 st, +3 st,
      −3 st, +1 st (their offsets times f(w)). The lines are read through a Hann window, since the lead's
      line sits 6 Hz from the +1 st copy's at WIDTH 0.5 and leaks 0.11 of a copy's line into a
      rectangular window.
   15. working set: the defaults hold the working set below, and a run on them stays finite and
      within 2.5 dB of the level of the neutral card. Every other test starts from a neutral tuning
      (no scatter, no drift, no level scatter, contour 1, epoch align off) and turns on only what it
      measures.
   16. melodic bottom: the −3 st copy alone on the working set is within 1 cent of the lead at WIDTH 0,
      3.6 ± 1.5 cents flat of it at 0.1 and 300 ± 10 cents flat at 1. With driftCents 15 on a 6 s tone at
      WIDTH 0.05 the copy's largest drift is at most 15 × f(0.05) = 0.09 cents and its f0 moves under 1
      cent from one 0.5 s window to the next; at WIDTH 1 it moves as test 12 expects. With contour 0.6
      or 1.4 on a copy at WIDTH 0.05, a ±1 st sweep at 1 Hz and at 3 Hz comes out within 2 % of 1.0 × the
      input's swing; at WIDTH 1 within 15 % of the contour.
   17. width glides: 512 samples after a WIDTH step from 0.1 to 1 the copy's grain playback rate sits
      strictly between its two settled values, and a second later it equals the rate of a run that
      held WIDTH 1 throughout.
   18. width lag: each copy alone on 220 Hz bursts has its lag at 24, 60, 42 and 45 ms at WIDTH 0 and
      at 56, 140, 98 and 105 ms at WIDTH 1 (within 0.5 ms). A WIDTH move from 0 to 1 while a burst
      sounds in copy 2's output leaves its lag at 60 ms to the end of that burst and puts the next
      burst at 140 ms.
   19. width 0 is a delay: at WIDTH 0 each copy alone at VOICES 1.0 on the working set, with the tone
      shelf flat and the trim off and the shipped tilt and shimmer in place (they scale to 0), turns a
      steady 220 Hz tone into the gain-scaled input at the lag it took, through the same low cut and DC blocker. The lag is searched within half a period of the
      engine's lag and the gain fitted, since level scatter draws one. With the shipped epoch align on
      the worst sample error is under 1e-4 (4.2e-5, the float rounding of the 120 Hz low cut). The same
      copy at WIDTH 0.1 is more than 100 times further from the delayed input.
   20. epoch snap at WIDTH 0 and 0.1: on the jittery 150 Hz pulse train a copy with formant +2 st and
      pitch 0 at WIDTH 0 has the same samples with epoch align on and off. At WIDTH 0.1 the two differ
      and the pulse cut is cleaner than the grid by at least 2 dB (4.1 dB).
   21. owner voice pitch and throat (voice 1 alone at WIDTH 1, VOICES 0.2, solo bit 1): f0 on the 220
      Hz tone within 10 cents of 220 × 2^(3 / 12), and its centroid over harmonics 1..10 of its own f0
      on the vowel above the unshifted copy's and above a +3 st copy with no throat move.
   22. owner voice tilt: with the shimmer off, the energy above 1 kHz against the energy below 1 kHz
      (harmonics of its f0 up to 4.4 kHz) rises by 1.5 to 2.5 dB over the same voice at tilt 0
      (1.59 dB).
   23. owner voice shimmer: with the tilt off and the pulse cut on, the gain of each cycle read at its
      peak against the same voice at shimmer 0. Neighbouring cycles differ by 0.3 to 1.5 dB on average
      and so does the standard deviation (0.93 and 0.85 dB over 758 cycles). A negative shimmer clamps
      to 0 and gives the samples of shimmer 0.
   24. tilt and shimmer follow WIDTH: at WIDTH 0 voice 1 with its shipped tilt and shimmer gives the
      samples of voice 1 with both at 0, and so does voice 5 with a tilt of 4 dB and a shimmer of 1 dB.
      At WIDTH 0.5 the tilt rise of test 22 and the cycle spread of test 23 are the WIDTH 1 values times
      f(0.5) = 0.451, to within 0.12 of that ratio (0.444, and 0.464 with 0.456 for the standard
      deviation).
   25. tilt and shimmer neutral: the three recorded runs of Measured item 1 (the pinned tuning, with
      voice 1 as it was before the owner's voice) hash to the recorded values with every tilt and
      shimmer at 0, and still do with tilts under 0.005 dB and negative shimmers.
2. level_test: a Crowd card row at VOICES 0.5 and WIDTH 0.5 on the default tuning (the working set) in
   the 0..+0.5 dB band, as a lambda row driving PitchFx then Crowd (the two-stage row pattern at
   level_test.cpp:235-243).
3. test/firmware: core_registry (amountCards, engineValue, knobs move, pitch group count 3),
   core_chain_run (a Crowd card rig; PitchFx runs with the Crowd card on and both pitch toggles off; the
   Crowd ahead of Autotune in a layout still gets a ring; bit-exact pass when off), core_layout
   (a layout with the Crowd card in slot 4 and Unison out sums against the lines with the estimate;
   the measured rule change), parity_test sweepCrowd, knob_step runCard case.
4. Gates: ctest `100% tests passed`; `cubevox-proto --layout` ok; `cd firmware && ./build.sh`
   ends `[warn-gate] ... ok`; `BENCH=1 ./build.sh`.

## CPU and memory

| Piece | Estimate | Basis |
|---|---|---|
| one voice, linear reads | ~28k cycles, 4.4 % | PsolaVoice 22k with launch-time weights; per-sample cubic reads were 36k |
| four voices | ~112k, 17.5 % | |
| five biquads | ~4k, 0.6 % | POLISH EQ's three biquads at 9.2k on the host |
| card | ~17 % mean, ~19 % worst | the first estimate with the room (19 / 21) less the room's ~14k and three biquads; costs.h keeps 19 / 21 until a board bench |
| headroom under the lines in today's default layout | 4 % mean, 12 % worst | costs.h sums 71 / 83 against 75 / 95; a layout with the Crowd card gives up whatever cards the owner chooses |

The bench decides, on the board per the costs.h method with the Crowd card placed in slot 4 by
`set_layout`. Fit order if over: three voices (−4.4 %); then the voices at half rate, reading
every other ring sample and upsampling through a half-band, which halves the voice cost; then a
CPU pass on Spring (15 %). Memory: four voices' grain state and five biquads sit in the plain
global, not DTCM. The shared input ring grows from 100 ms to 230 ms (11,040 floats, 44,160 B) so
a 200 ms lag plus the longest grain fits. It sits in PitchFx, which is in DTCM, so the ring adds
24,960 B there and the firmware build reports DTCM at 76,928 B of 130,816 B (58.8 %).

## Steps

1. The agent working the engine files: crowd.h, the pitch_fx.h accessors and crowdOn,
   crowd_test.cpp, the CMake line, the level_test row. Gate: ctest.
2. The agent working the wiring files, on top of step 1: registry, chain, costs, firmware tests,
   emulator block, render flags, engine_ab.sh, hello.json, app test counts. Gates: all four.
3. The lead merges per the worktree rule (cpu-pass-3 spec item 6), bumps VERSION.txt in the merge
   commit; the owner listens in the emulator; a board bench fills the cost row.

## For the owner

1. 80 % against your 85 % for the lead fade point (decision 7).

## Built 2026-10-31, where the code differs from the text above

Retune 1 (owner 2026-10-31): the copies sounded too similar to the input; each copy now has a pitch offset of a few semitones, a formant offset and its own tone shelf.

Defaults 3 (owner 2026-10-31): copy 4 to formant -1.5 and pitch +1; lags 40, 70, 100, 75 ms.

WIDTH (owner 2026-10-31): the upper pot spreads the copies' pitch offsets from ±0.01 st to their full values; the line order is low, high, low, high; the old lag multiplier is pegged at 1.2.

WIDTH, melodic bottom tenth (owner 2026-10-31): both the pitch and the formant offsets scale with WIDTH through the piecewise-linear factors of the WIDTH law above. Measured on the −3 st copy (formant −1 st) alone with the knee at 0.025: f0 −0.88, −7.35, −137.62 and −300.00 cents from the 220 Hz lead at WIDTH 0, 0.1, 0.5 and 1, and a grain playback rate at 0, 2.5, 45.8 and 100 % of its full formant move.

In tune at the bottom (owner 2026-10-31): knee 0.025; drift and contour scale with WIDTH so an instrument through the bottom tenth stays in tune. On the working set the −3 st copy reads −0.88, −7.35 and −294.86 cents from the lead at WIDTH 0, 0.1 and 1 (the last with 5 cents of drift). At WIDTH 0.05 the largest drift is 0.15 cents against the 0.21-cent limit, the f0 moves at most 0.39 cents from one 0.5 s window to the next, and a copy with contour 0.6 or 1.4 swings 0.995 or 1.005 of the input's swing at 1 Hz and 3 Hz.

WIDTH smoothing: the knob_step test's WIDTH step from 0.2 to 0.215 read 1.50 of steady state with the new knee, a spike at the first sample of the step block from the pitch jump of an unsmoothed knob step. A scan of every 0.015 step from WIDTH 0.02 to 0.97 found the spike (above 1.05) at 13 start points on the old engine and 10 on the new one without smoothing. The 20 ms one-pole on w leaves 2 (0.13 and 0.21), and the 0.2 step reads 0.96.

Clean bottom, wider top (owner 2026-10-31): pitch factor exactly 0 at WIDTH 0, knee 0.012, and the lag multiplier runs from 0.5 at WIDTH 0 to the stretched top, so the longest lag sits 20 ms further back at full WIDTH.

Measured after the clean bottom, on the working set. The −3 st copy reads 0.00, −3.53 and −294.86 cents from the lead at WIDTH 0, 0.1 and 1. Each copy at WIDTH 0 equals the delayed input to 4.2e-5 at worst with epoch align on, because a copy with ratio and rate both exactly 1 skips the epoch snap (it was 1.5e-2 before the skip). The lags read 24, 60, 42 and 45 ms at WIDTH 0 and 56, 140, 98 and 105 ms at WIDTH 1. `trimDb` stays 1.0 dB, and the level_test row at VOICES 0.5 and WIDTH 0.5 reads +0.18 dB. On the neutral card the crowd alone at VOICES 100 % reads −5.20 dB at WIDTH 0 (four copies on one pitch at four lags comb against each other) and −0.50 dB at WIDTH 100 %.

Working set (owner 2026-10-31): contour 0.8/1.2/0.7/1.1, level scatter 2 dB, timing scatter 12 ms, drift 15 cents, epoch align on. These are the defaults of `CrowdTuning`, so the box runs them. `trimDb` is 1.0 dB (from 2.1): the level_test row at VOICES 0.5 and WIDTH 0.5 reads +0.25 dB. On the neutral card (every enhancement off) the level law reads +0.00 to +0.45 dB across VOICES 20 % to 80 % and +0.12 dB at 100 % at WIDTH 0.5; at VOICES 100 % it reads −1.60 dB at WIDTH 0 and −0.38 dB at WIDTH 100 %. Host cost: 3.74 us per block on the working set against 3.42 us on the neutral card (x1.09); `costs.h` keeps its estimate until a board bench.

1. The one highpass runs on the crowd sum after the per-voice gains, not one per voice. The four
   voices share one setting, so the sound is the same and three biquad runs go.
2. `trimDb` was +2.1 dB (now 1.0, see the working set above); the level_test row at VOICES 0.5 and WIDTH 0.5 reads +0.26 dB,
   and the level law reads +0.07 to +0.92 dB across VOICES 20 % to 100 % at WIDTH 0.5. At VOICES
   100 % the crowd reads −0.74 dB at WIDTH 0 and +0.72 dB at WIDTH 1.
3. `PitchFx::correctionSemis()` follows Autotune's fade down and reads 0 once it has settled off, so
   the voices return to the sung pitch within 200 ms of the switch; before, it held the last value.
4. The read delay is the output lag; the correlation peaks sit at the lag the copy holds, 89.98 ms
   and 149.98 ms for copy 4 and 119.98 ms and 199.98 ms for copy 2.
5. Host cost at -O2, same machine and run: Crowd at VOICES 0.8 reads 3.3 us per block against Unison's
   0.91 us, a ratio of 3.64 (3.85 for the build with the room). Scaled by Unison's 46.9k board
   cycles, four voices land near 171k cycles. The cost row stays the 19 / 21 estimate until a board
   bench; layoutValid refuses the card in today's default layout with Unison out at 82 / 96 against
   75 / 95, so the bench needs a bench op or a lighter layout.
6. Solo (owner 2026-10-31): CrowdParams::solo, a bit per voice, emulator tuning hook; the firmware never sets it.

### Simplified (owner 2026-10-31)

After tuning in the emulator the owner said: "the room doesn't really help. distance doesn't have
much change except to add an eq. wobble is kind of subtle to the point that i can't tell what it
does. let's simplify things. remove the room reverb. ... distance should add more time between each
voice and also add delay (not an echo but a lag) with a ceiling of 200 ms. ... the goal is to
emulate the effect of several other people saying the same lines as adam. adam has a sing-speaking
vocal delivery so harmony is not in the question and pitch tracking is not important."

1. Gone: the room and its send, the DISTANCE-driven highpass and high shelf, the sine wobble (lfoHz,
   lfoCents, the phase accumulators) and the DISTANCE smoothing. The crowd sum keeps one fixed
   Butterworth highpass at `hpHz` 120 Hz. The tone shelves, the VOICES law, solo, the lead law,
   trimDb and Autotune's correction term stay.
2. DISTANCE is time only (DISTANCE law above), capped at `maxLagMs` 200. The shared input ring
   grows from 4,800 to 11,040 samples (230 ms) to hold a 200 ms lag and the longest grain, and the
   voice's fixed-point read position drops from 18 to 16 fraction bits because 11,040 samples
   shifted up by 18 bits overflows an int.
3. Lag changes wait for a quiet gap (two blocks under `gapFloorDb`), so a copy never bends in pitch
   mid-word. The gap test reads the ring at the lag the copy plays and at the lag it moves to, not
   only the first, because a lag increase replays the span between the two read points and a
   decrease skips it. A move needs a pause longer than the move itself, which the tests show for
   moves of 75 and 100 ms across a 120 ms gap.
4. New tuning: `scatterMs` and `driftCents`, both 0 by default. With scatterMs 15 on copy 4 the ten
   burst lags read 60.5, 76.4, 60.1, 77.0, 89.5, 64.4, 89.4, 67.6, 88.9 and 89.7 ms against a
   75 ms target. With driftCents 20 the copy's f0 over ten 0.5 s windows reads −13.3 to +9.7 cents
   from its offset.
5. New defaults: formantSt {−1.75, −1, −1, +1}, pitchSt {−1.75, −3, +3, −1}, delayMs {90, 60, 100,
   50}, tiltDb unchanged, voiceDb −9, trimDb +2.1. The Crowd card's cost row in costs.h is not
   re-benched; its comment still names the room.
6. The emulator's Crowd tuning nodes are Formants, Pitch, Delay, Tone, Scatter and Solo. The Levels
   node (voice level, trim, lead leaves from, fade share) went with the others; those four fields
   keep their defaults and print with the tuning.

## Rule (owner 2026-10-31)

"They need to sound like actual people though. Not an alien, not a cartoon." Every move a copy
makes stays inside what a human voice does. Copies read as people inside about ±5 semitones of
pitch and ±3 semitones of formant, paired the natural way, the lower pitch with the longer throat.
Timing scatter stays under about 25 ms, pitch drift under about 30 cents and loudness scatter under
4 dB. The cutting of the copies must not add buzz or doubling that reads as a machine; epoch
alignment exists for that. The pinned sound is the tag `crowd-pin-2026-10-31` on main at v2.00.63.

## Enhancements (owner 2026-10-31)

Three controls make a copy read as a person and not as Adam shifted. The sound the owner pinned is the
tag `crowd-pin-2026-10-31` (v2.00.63). Each control is neutral at 0 (contour 1, epoch align off), where
the output is bit for bit what it is without the control. The defaults are the owner's working set
(see Built, above). Each range stays inside the Rule above, and the engine clamps a value to its
range.

### Controls

1. Loudness scatter, `levelScatterDb`, default 2, range 0..4 dB. At each copy's quiet gap, where the
   timing scatter already re-rolls, the copy also draws a gain offset uniform in
   [-levelScatterDb, +levelScatterDb] from its own xorshift stream and holds it to the next gap. The
   offset multiplies the copy's gain through `dbToGainFast`. The gap rule runs while level scatter is
   on even when timing scatter is 0, and a gap that draws no new lag still draws a new loudness. At 0
   no draw is taken, so the stream and the timing scatter match the engine without it.
2. Contour, `contour[4]`, default {0.8, 1.2, 0.7, 1.1}, range 0.6..1.4. A running mean of log2(period) follows
   the tracker's period with a time constant of about 1 s, updated while the tracker reports a voiced
   frame and held otherwise. Adam's deviation is mean - log2(period), in octaves, positive when he is
   sharp of his mean. A copy's ratio gains 2^((contour - 1) x deviation), so the copy sits `contour`
   times as far from its own mean as Adam sits from his. 1 follows him exactly, 0.7 flattens the
   copy's rises and falls to 70 % of his, 1.3 exaggerates them. The term is added to the exponent
   that `exp2Fast` takes once per block, and a contour of exactly 1 adds exactly 0. The contour a copy
   uses is 1 + (contour − 1) × f(w) (WIDTH law above), so it is exactly 1 at WIDTH 0 and the slider
   value at WIDTH 1.
3. Breath removed (owner 2026-10-31).
4. Epoch align, `epochAlign`, default true, with `epochSearch` 0.25 (range 0..0.3 periods). The cut
   of every grain moves from the period grid to the glottal pulse nearest the grid mark.
   1. PitchFx writes a shadow ring beside the input ring on every sample. It holds the input through
      a one-pole at 800 Hz at the same index and is exposed as `shadow()`. It costs one multiply-add
      per sample and moves no other output, so the toggle is bit exact whether or not the ring is read.
   2. A shared `EpochTrack` (`engine/crowd_epoch.h`) finds the pulses once per period for all four
      copies. It expects the next pulse one tracker period after the last, takes the tallest sample of
      the track's sign within +-`epochSearch` periods of that spot, and places the pulse at the
      centroid of the lobe above half its height. The first search of a track takes the largest
      absolute sample of a whole period and keeps its sign. A ringing voice has positive and negative
      lobes nearly as tall, and following one sign keeps the track from flipping between them.
   3. A copy's launch asks the track for the pulse nearest its grid mark and cuts the grain there, unless
      its ratio and rate are both exactly 1 (WIDTH 0), where it stays on the grid. A
      pulse more than half a period from the mark leaves the grain on the grid. The grid still decides
      when a grain is laid down, so the copy's pitch is unchanged.
   4. A copy cut on the pulses sits at most half a period (measured 0.7 ms at 220 Hz) from the lag it
      has on the grid, since it takes the nearest pulse.

### Tuning

```
pitchSt        = {-1.75, 3, -3, 1}      // st at WIDTH 1; voice 1 is the owner's voice (2026-10-31)
formantSt      = {-1.75, 1.65, -1, -1.5} // st at WIDTH 1
tiltDb         = {-3, 0, 3, 5}          // dB, the tone shelf above tiltHz
delayMs        = {40, 100, 70, 75}      // ms before lagScale
scatterMs      = 12       // 0..30 ms
driftCents     = 15       // 0..50 cents
widthKnee      = 0.1      // 0.02..0.9
widthKneeFrac  = 0.012    // 0..1
widthLagBottom = 0.5      // 0.2..1
widthLagMs     = 20       // 0..60 ms
levelScatterDb = 2        // 0..4 dB
contour        = {0.8, 1.2, 0.7, 1.1}   // 0.6..1.4
tilt           = {0, 2, 0, 0}           // -6..6 dB at WIDTH 1, whole-spectrum tilt, times f(w)
shimmer        = {0, 1, 0, 0}           // 0..1 dB at WIDTH 1, per-cycle random gain, times f(w)
epochAlign     = true
epochSearch    = 0.25     // 0..0.3 periods
trimDb         = 1.0      // dB
```

### Why the grid cut buzzes

This is the reading that fits the numbers below. A copy whose pitch ratio differs from its grain
playback rate lays each input pulse down twice, once from each of the two grains that cover it, at two
output times a fixed distance apart. Where the grid mark falls in the pulse cycle sets how the two
shares split, and a mark that slides across the cycle (the tracker's period is never exact) moves the
split. That reads as buzz and a doubled pulse. A grain centred on its pulse holds one pulse at full
weight and the neighbours at zero, so the split never forms. Copy 1 at the defaults has a ratio
nearly equal to its rate and gains least (2 to 3 dB on the recordings). Copy 2, the furthest from
equal, gains most (12 to 14 dB).

### Where the code differs from the first brief

1. Contour reads the deviation of the current block, not of the moment a copy's lag ago. The grains of
   every copy are laid down at the tracker's current period, so the copy's pitch follows the voice's
   pitch of the moment while only its content is late. A first cut that read the deviation a lag back
   (plus the tracker's delay) missed by about 50 degrees at a 3 Hz glide, and a contour of 0.6 gave a
   swing ratio of 0.82.
2. The tests range over the Rule's ranges (contour 0.6 and 1.4, loudness scatter 3 dB).
3. The pulse search follows one sign and takes a centroid. A first cut that took the largest absolute
   sample and a parabola flipped between the positive and negative lobe of the ring, 30 samples apart,
   and read 26 dB worse than the grid on the pulse train.

### Measured

Host, -O2, 48 kHz, `test/crowd_test.cpp` unless noted.

1. Bit exactness. The 180 Hz program at VOICES 70 % and WIDTH 50 % hashes to b5704f6967ce7680 before
   and after; the burst program at 80 % and 50 % to e8ec16a472153926; the burst program at 60 % and
   80 % with timing scatter 15 ms and drift 20 cents to d6b873e27ac6a7d8. The test prints these three.
   A scratch set of seven runs (those three, the program at 100 % and 100 %, at 20 % and 100 %, with
   scatter and drift, and with a solo) hashes to 02532a5f1bdc5ad1 combined on the tag and on the edited
   engine. Inactive settings (epochSearch, negative values that clamp to 0) move no sample. These
   hashes belong to the pinned engine; the melodic WIDTH law moves the output below WIDTH 1, so the
   test's printed hashes differ from them now. At WIDTH 1 both new factors are exactly 1.0, and a
   scratch run of a 4 s program on the working set at VOICES 70 % hashes to 25b19e219514113a before
   and after the drift and contour scaling.
2. Loudness scatter 3 dB over ten bursts: per-burst gain -2.97, +2.90, +2.88, +2.79, -0.23, -1.09,
   +0.10, +2.47, -1.41, +1.93 dB against the same run with no scatter.
3. Contour on a 220 Hz tone swinging 1 st either side: copy swing over input swing 0.624 at 0.6 and
   1.000 at 1.0 and 1.385 at 1.4 at 1 Hz; 0.627, 1.000 and 1.383 at 3 Hz.
4. Epoch align on a 150 Hz Rosenberg pulse train through two formants with 0.4 % period jitter,
   harmonics 2..10: formant +2 st copy 31.5 dB harmonic to interharmonic cut on the grid, 34.7 dB cut
   on the pulses (+3.2 dB). A copy at -3 st and formant -1.5 st reads 31.0 dB cut on the pulses and
   23.7 dB on the grid against 32.1 dB for the input, so the pulse cut stays 1.2 dB under the input.
   The marks sit within 0.16 samples (spread) of the pulses, which jitter 1.3 samples themselves.
5. Epoch align on Adam's three recordings (`samples/`, scratch measurement outside the test), the
   four default copies alone, harmonics 2..4 in steady 120 ms windows. Each row gives the harmonic to
   interharmonic ratio on the three recordings, grid cut then pulse cut. Copy 1 (-1.75 st, -1.75 st)
   33.3, 36.6, 33.1 then 36.2, 38.8, 35.1 dB. Copy 2 (+2.75 st, -1 st) 19.4, 17.3, 13.5 then 31.3,
   28.9, 27.6 dB. Copy 3 (-3 st, -1 st) 28.0, 26.4, 23.5 then 35.3, 36.1, 32.5 dB. Copy 4 (+1 st,
   -1.5 st) 27.5, 26.8, 25.4 then 34.7, 34.9, 32.9 dB. The pulse cut is better in all twelve cases, by
   2.0 to 14.2 dB, and reads 0.0 to +1.2 dB louder.
6. Host timing, Crowd alone at VOICES 80 %, WIDTH 50 %: 3.36 us per block on the pinned engine, 3.44 us
   with every control neutral, 3.92 us with all three on. PitchFx pass-through 1.38 us per
   block on the pinned engine and 1.60 us with the shadow ring.

### Memory and cost

1. The shadow ring adds 44,160 B to `gPitchFx`. The firmware build prints DTCM at 121,088 of
   130,816 B (92.6 %), 9,728 B free; `gPitchFx` is 121,080 B. The Crowd card's plain global grows by
   about 2.3 KB (the pulse track).
2. The shadow write moves every pitch card's cycles by a few per sample, and the pulse track adds
   cost only while epoch align is on. The working set has it on, so the card runs at the working-set
   cost under Built above. `costs.h` keeps its estimate until a board bench.

### Emulator and render

1. The Scatter node gains Level scatter. A new node Contour has four sliders, 0.6..1.4. The Pitch node
   gains Width knee and Offsets at knee sliders, an Epoch align checkbox and an Epoch search slider.
   The Delay node gains Lag at bottom (0.2..1) and Lag at top (0..60 ms). Print tuning lists every new field.
2. cubevox-render keys `crowdWidthKnee`, `crowdWidthKneeFrac`, `crowdWidthLagBottom` (0.2..1),
   `crowdWidthLagMs` (0..60), `crowdLevelScatterDb`, `crowdContour0..3`, `crowdTilt0..3` (-6..6 dB),
   `crowdShimmer0..3` (0..1 dB), `crowdEpochAlign` (0 or 1) and `crowdEpochSearch`. A value outside its
   range is refused.
3. The firmware never sets any of them.

## Voice 5 trial (owner 2026-10-31)

The owner asked for a fifth copy outside the VOICES law, a trial voice meant to sound like a person of
another sex, age or body size, with its own pitch, throat, contour, tilt, breath, vocal fry, jitter and
shimmer, three presets and a solo. The owner dropped it on 2026-10-31 and the code is gone. Commits
`cb28d41` and `33be645` hold its fields, presets and measurements.

## Per-copy tilt and shimmer, and the owner's voice 2 (owner 2026-10-31)

Owner, 2026-10-31, after trying the fifth voice: "adding a higher pitched voice with these attributes is
convincing: pitch +3 st, formant +1.65 st, contour 1.2, tilt +2, breath 0, fry 0, jitter 0, shimmer 1.
peak and notch didn't seem to make a ton of difference to me... we can sub this for voice 2."

### What changed

1. The four main copies gain the two controls the trial proved, per copy. `tilt[4]` is the whole-spectrum
   tilt of voice 5 (a low shelf at -tilt / 2 and a high shelf at +tilt / 2, both at 1 kHz, -6..+6 dB).
   `shimmer[4]` is the per-cycle random gain of voice 5 (each grain takes a gain within +-1.5 x shimmer dB,
   0..1 dB). Both defaults are 0 except on voice 1.
2. Voice index 1, the second in line, is the owner's higher voice: pitchSt +3, formantSt +1.65, contour 1.2,
   tone shelf tiltDb 0, tilt +2, shimmer 1. Its lag (delayMs 100, the 140 ms top of the lag table) and its
   place in the fade order are unchanged. The tone shelf was -4 dB and the pitch and throat +2.75 and -1 st.
3. Voice 5 loses its peak and notch (`v5PeakHz`, `v5PeakDb`, `v5NotchHz`, `v5NotchDb`), their two biquads, sliders,
   render keys, tests and spec lines. It stays the trial slot, off by default.

### How it runs

1. A copy at tilt 0 and shimmer 0 runs exactly the code it ran before (bit exact, test 25). A tilt under 0.005
   dB skips the two shelves. A shimmer of 0 leaves the voice without a luck source, so its launch clock and
   grain gains are the old ones. Both clamp to their ranges.
2. The tilt filters run on the voice's output before the tone shelf. A stage that wakes from off starts with
   empty filter state. The shelves are set on the first block and when the tilt moves, never per sample.
3. Each copy draws its shimmer from its own xorshift, seeded at reset and when the copy is retired. Voice 5
   keeps the seed it had.
4. Both controls take the WIDTH factor f(w), voice 5's included (WIDTH law, above): the tilt and the shimmer
   applied are the settings times f(w). Voice 1 therefore has its +2 dB tilt and 1 dB shimmer only as WIDTH
   rises, and at WIDTH 0 it is a plain delayed double of the lead. Test 19 runs with the shipped tilt and
   shimmer in place and still meets its bound. Both are set per block, so the shelves move through the
   20 ms WIDTH one-pole and not in a jump.
5. Memory: the Crowd card's plain global grows by about 0.3 KB (four tilt pairs and five luck sources), and the
   firmware build reports D1 SRAM at 191,632 B against the 191,320 B recorded with voice 5. DTCM is
   unchanged at 121,088 B.

### Emulator and render

1. The Tone node gains the four tilts in one row (`SliderFloat4`, "Tilt 1-4 dB"). Four stacked sliders put the
   node at 733 px and overran the column (the Pitch node, the tallest before, reads 710 px). The Scatter node gains Shimmer 1..4 (0..1 dB). Print tuning lists
   `tilt` and `shimmer` after `contour`, and no longer lists the four peak and notch fields. The "Peak and
   notch" node and its probe scenario are gone. `--layout` prints ok.
2. cubevox-render keys `crowdTilt0..3` and `crowdShimmer0..3`, refused outside -6..6 and 0..1. The four
   `crowdV5Peak*` and `crowdV5Notch*` keys are gone. The parity test compares both arrays.

### Measured

Host, -O2, 48 kHz, `test/crowd_test.cpp`.

1. Voice 1 alone at WIDTH 1 reads f0 261.63 Hz against 261.63 Hz (0.01 cents). On the vowel its centroid over
   harmonics 1..10 reads 1239.6 Hz, against 1100.1 Hz for the unshifted copy and 1116.7 Hz for the +3 st copy
   with no throat move.
2. Tilt +2 dB on voice 1 moves the energy above 1 kHz against the energy below by +1.59 dB (+0.47 dB at tilt 2,
   -1.12 dB at tilt 0). Less than the 2 dB between the shelf ends, because the harmonics near 1 kHz sit
   half way up the shelves.
3. Shimmer 1 dB on voice 1, cut on the pulses: over 758 cycles neighbouring cycles differ by 0.93 dB and the
   gains have a standard deviation of 0.85 dB (the draw is uniform within +-1.5 dB). Shimmer 0 and a clamped
   -1 give the same samples.
4. The three recorded runs of Voice 5 trial, Measured item 1 hash to 12873f20ed548ec8, 8ff1eb8ee12b722c and
   d9de9308d8365971 with every tilt and shimmer at 0 on the pinned tuning, and still do with tilts of
   0.003, -0.004 and 0.002 dB and shimmers under 0.
5. Level. `trimDb` stays 1.0 dB. The level_test row at VOICES 0.5 and WIDTH 0.5 reads +0.16 dB, inside the
   0 to +0.5 dB band. The level law reads +0.07, +0.09, +0.07, +0.23 and +0.01 dB at VOICES 20, 40, 60, 80 and
   100 % at WIDTH 0.5, and the crowd alone at VOICES 100 % reads -5.14 dB at WIDTH 0 and -0.37 dB at WIDTH 100 %.
6. Voices differ: the spectral centroids of voice 0 and voice 1 alone differ by 25.6 % (8.9 % before).
7. WIDTH. Test 19 with the shipped tilt and shimmer in place reads a worst error of 2.43e-05, 4.23e-05
   (voice 1), 1.83e-05 and 1.43e-05 for the four copies at WIDTH 0, inside the 1e-4 bound and equal to the
   readings with both zeroed, since both scale to exactly 0. At WIDTH 0.5 voice 1's tilt rise reads 0.71 dB
   (0.444 of its 1.59 dB at WIDTH 1) and its cycle spread 0.43 dB (0.464 of 0.93 dB, and 0.456 for the
   standard deviation), against f(0.5) = 0.451.

### Where the code differs from the brief

1. The four tilts share one row in the Tone node (Emulator and render, item 1).
2. The fade-order test reads its lines through a Hann window. At WIDTH 0.5 and VOICES 60 % the +1 st copy's
   f0 sits 6 Hz from the lead's, and the lead's line leaked 0.11 of a copy's line into a rectangular
   window (0.06 before the owner's voice moved another line). Windowed, the silent copy reads 0.01.

On 2026-10-31 the owner put voice 2 back to pitch +2.75 st, formant -1.0 st, tone shelf -4 dB, tilt 0 and
shimmer 0, and took the per-copy tilt and shimmer controls out. The engine, the emulator, the render tool
and the tests no longer have them. Commit `cb28d41` holds the tilt and shimmer code and measurements.
