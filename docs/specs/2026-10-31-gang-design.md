# cubevox — Gang card (four formant-shifted voices, WIDTH and VOICES)

Date 2026-10-31. Owner design in chat 2026-10-31; this file is the implementation reference.
The wiring map with file and line references is `build/audit/2026-10-31-doubler-map.md` (gitignored).

## Purpose

Adam sings in a speaky, near-monotone style, so effects that need a key do not serve him. The
Gang card makes a gang out of his own voice. It adds up to four copies, each a different person:
owner 2026-10-31, "we are supposed to be offsetting by a few semitones and formants to give the
effect of the voice belonging to a totally different person. then we use a little lfo or delay to
make it sound like it's not the same performance." Each copy has a pitch offset of a few
semitones, a formant offset (a throat) and its own tone shelf, lower people with longer throats
and higher people with shorter ones. Its own lag behind the lead makes it a different performance.
The gang as a whole has a fixed low cut. It never reads the key. It reads the pitch period from the
tracker Autotune uses. The goal is the effect of several other people saying the same lines as
Adam (owner 2026-10-31, see Simplified under Built).

## Decisions (owner 2026-10-31)

1. A new library card and its own effect. It is not a replacement for Unison and is not compared
   against it (owner 2026-10-31); which slot it takes is a layout decision for the owner. This
   spec leaves the default layout as it is.
2. Lower pot VOICES, the amount knob. 0 is dry. The four voices fade in one at a time, then the
   lead fades out, so the top of the knob is the gang alone. Owner: "if the effect is dimed, we
   don't hear Adam, we hear the gang."
3. Upper pot WIDTH. It spreads the copies' pitch offsets, so the bottom is four copies a hair off the
   lead and the top is the gang at its full offsets (owner 2026-10-31, "nix the distance knob and
   replace it with a 'width knob'. the width knob will spread the voices.").
4. Autotune is not expected on with this card. The tracker runs whenever the Gang card is on.
5. The voices read the pitch front end's ring and add Autotune's correction to their ratio, as
   Octave does. Octave's wet is not doubled.
6. Four voices with linear grain reads, benched first. If over budget, three voices before any
   CPU pass elsewhere.
7. Made in this spec, for the owner to confirm: the lead leaves from 80 % of the knob, not 85 %,
   so each voice owns a round fifth of the travel and the percent readout tells the count
   (20 % one voice, 40 % two, 60 % three, 80 % four).
8. The card is named Gang (owner 2026-10-31).

## Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | WIDTH | continuous, 0..100 % | `WIDTH 35 %` |
| lower (knob B) | VOICES | continuous, 0..100 % | `VOICES 60 %` |

Card id `gang`, name `Gang`, Group Pitch, Engine Gang. Both knobs are the registry's
percent knob with no decimals, so the readout sweep test holds as it is.

### VOICES law, knob position k in 0..1

1. Span A, k in [0, 0.8]. u = k / 0.2 runs 0..4. Voice v (0..3, fade order below) has weight
   w_v = sin(π/2 · clamp01((u − v) / holdFrom)). Each voice fades in over the first three
   quarters of its fifth and holds for the last quarter.
2. Span B, k in (0.8, 1]. s = (k − 0.8) / 0.2. Every w_v = 1. Gang power rises from P_4 to 1
   and the lead falls to 0.
3. Level law at every k. Each voice carries the fixed gain g = 10^(voiceDb / 20); with voiceDb
   −9 dB four voices sum to the power P_4 = 4 g² ≈ 0.50. Gang power is P = Σ (g w_v)² in span A
   and P = P_4 + (1 − P_4) · sin²(π/2 · s) in span B. The lead gain is sqrt(1 − P). The gang
   multiplier is sqrt(P / Σ (g w_v)²), which is 1 in span A and reaches sqrt(1 / P_4) at the top,
   so each voice goes from −9 dB to −6 dB there. Total power is unity at every k, so the level
   rule holds by construction; trimDb in tuning takes up the residue level_test measures on the
   180 Hz program.
4. Fade order, low, high, low, high, by voice index (full pitch, throat, delay before the lag scale):
   0 (−1.75 st, −1.75 st, 40 ms), 1 (+2.75 st, −1 st, 100 ms), 2 (−3 st, −1 st, 70 ms),
   3 (+1 st, −1.5 st, 75 ms). One voice is a second singer, two a pair, four the gang.
5. Gains are computed per block and ramped per sample across the block, as Octave and the reverb
   MIX do. A voice whose ramped gain sits at 0 is reset and does not tick, so one voice costs a
   quarter of four.

### WIDTH law, knob w in 0..1

WIDTH is pitch only. Per copy and per block, copy v's pitch offset in semitones is
sign(pitchSt[v]) × (0.01 + (|pitchSt[v]| − 0.01) × w). At w = 0 every copy sits 0.01 st (1 cent)
off the lead on its own side, and at w = 1 it sits at pitchSt[v]. An offset already inside 0.01 st
stays as it is, so a pitchSt of 0 stays on the lead. Formants, lags and tone shelves do not follow
the knob. The offset moves with the knob continuously, so a knob move glides the copies.

### Lag law

Copy v's lag target is delayMs[v] × `lagScale` (1.2), capped at `maxLagMs` (200 ms) and floored at
the voice's own minimum (1.5 periods plus 4 samples). No knob reads it. At the defaults the four
copies lag 48, 120, 84 and 90 ms.

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
[−driftCents, +driftCents]. A new random target comes every `driftHoldS` (2 s) and the drift slews
toward it with a time constant of 1 s, updated per block. The pitch ratio gains the term
2^(drift / 1200) through exp2Fast. With `driftCents` at 0 the term is exactly 0 and the ratio is
the same as without the code.

Filter coefficients update per block only, since the setters call libm.

## Engine `engine/gang.h`

### Interface

```
namespace cv {
struct GangTuning {
  std::array<float, 4> pitchSt     = {-1.75f, 2.75f, -3.0f, 1.0f};   // full pitch offset 2^(st / 12) at WIDTH 1
  std::array<float, 4> formantSt   = {-1.75f, -1.0f, -1.0f, -1.5f}; // grain playback rate 2^(st / 12)
  std::array<float, 4> tiltDb      = {-3.0f, -4.0f, 3.0f, 5.0f};     // high shelf gain above tiltHz
  std::array<float, 4> delayMs     = {40.0f, 100.0f, 70.0f, 75.0f};  // lag behind the lead before lagScale
  float lagScale     = 1.2f;      // a copy's lag is delayMs times this
  float tiltHz       = 2500.0f;
  float voiceDb      = -9.0f;     // one voice against the lead at unity
  float trimDb       = 2.1f;      // level rule residue
  float leadFadeFrom = 0.8f;      // VOICES position where the lead starts to leave
  float holdFrom     = 0.75f;     // part of each voice's fifth spent fading; the rest holds
  float hpHz         = 120.0f;    // low cut on the gang sum
  float maxLagMs     = 200.0f;    // ceiling on a copy's lag
  float gapFloorDb   = -50.0f;    // a read point under this level counts as a gap
  float scatterMs    = 0.0f;      // peak random lag offset taken at each gap, 0..30
  float driftCents   = 0.0f;      // peak of each copy's slow random pitch walk, 0..50
  float driftHoldS   = 2.0f;      // time between new drift targets
};
struct GangParams {
  bool on = false;
  float width    = 1.0f;   // knob A, 0..1
  float voices   = 0.5f;   // knob B, 0..1
  GangTuning tuning;
};
class Gang {
 public:
  void reset();
  // in is this slot's input, the lead. pitch has already run this block, so the block's
  // samples sit at ring index head() - n + i.
  void process(const float* in, float* out, int n, const GangParams& p, const PitchFx& pitch);
};
}
```

Off: out equals in bit for bit.

### Per-voice chain

ring → voice (PSOLA grains at playback rate f_v, pitch ratio r_v, read delay D_v) → tone shelf →
× g w_v → gang sum.

1. Voice. A new class in gang.h, a PSOLA voice with a grain playback rate. It is built from the
   deleted `EpochPsolaVoice` (`git show 9b715ec^:engine/shifters.h`, launch at its lines 143-152,
   play at 182-193) and the period-grid mark scheduling of today's `PsolaVoice`
   (engine/psola.h `advanceMarks`). Grain length 2p, Hann from `HannRotor`, launch interval
   p / r_v, grain duration 2p / f_v, a per-sample read at a fractional delay that moves by
   (1 − f_v) per sample, grain gain f_v · 2 / (len / p) / r_v. Reads are linear, two taps, not
   Catmull-Rom (decision 6). No epoch search in the first cut: grains sit on the period grid as
   today's PsolaVoice does. If the gang sounds rough in the emulator, the fallback is one shared
   low-passed ring and the old peak search, with the marks shared across the four voices.
2. Read delay D_v is the copy's adopted lag in samples (lag law above), a per-voice member
   set per block, not `PsolaVoice::kGrainDelay`. The voice clamps it to at least 1.5 × the clamped
   period + 4 (the floor kGrainDelay encodes for the lowest pitch) and at most
   kVoiceRingLen − 2 × kMaxPeriod − 8, which is 9,660 samples (201 ms) on the 230 ms ring. The
   defaults keep every copy above the 21.7 ms floor and the 200 ms ceiling leaves the longest grain
   (686-sample period, rate 0.5) inside the ring.
3. Ratio r_v = 2^(widthSt(pitchSt[v], w) / 12) · 2^(drift_v / 1200) · 2^(pitch.correctionSemis() / 12).
   The PSOLA voice clamps the ratio to 0.5..2.0.
4. Period from `pitch.pitch().period`. Period 0 gives silence from the voice, as PsolaVoice does
   today. Unvoiced frames keep the last period, as Autotune's voice does.
5. Biquads from dsp.h. Each voice has one tone shelf, `setHighShelf(tiltHz, tiltDb[v])`, set at
   reset and when the tuning changes, never per sample. The gang sum has one `setHighPass(hpHz,
   kButterworthQ)`, set at reset and when the tuning changes.

### Output

out[i] = lead(i) · in[i] + gangMul(i) · dc(gang[i]) · 10^(trimDb / 20), with lead and gangMul
ramped per sample from the previous block's values to this block's. A 5 Hz DcBlocker sits on the
wet sum before it meets the lead, per the output-safety spec.

## Wiring

1. `Card::Gang` appended before `Count`; `Engine::Gang` appended before `Count`.
   kEngineCount 7 → 8 and kFadeHoldBlocks follows (core/layout_fade.h). Six switch statements gain
   a case: registry.cpp applyKnob and applyToggle, chain_run.cpp resetEngine and runStage,
   test/knob_step_test.cpp runCard, test/firmware/core_registry.cpp engineValue.
2. Registry row: card Gang, id "gang", name "Gang", Engine Gang, two knobs
   kPercent("WIDTH") and kPercent("VOICES"), Group Pitch, costs::kGang. applyGangKnob maps
   knob 0 to width and knob 1 to voices. applyToggle sets `params.gang.on` and
   `params.pitch.gangOn`.
3. `PitchFxParams::gangOn` (new, default false). pitch_fx.h line 49 becomes
   `tracker_.push(in, n, p.autotune.on || p.gangOn || activeAt_ > 0.0f)`. PitchFx gains
   `const VoiceRing& ring() const` and `int head() const` (the next write index, already wrapped).
4. chain_run: PitchFx runs at the first slot whose card is in Group Pitch (Autotune, Octave,
   Gang), with that slot's input, when any Group Pitch toggle is on. Today's rule keys on
   Engine Pitch; it becomes the group, so a Gang card placed ahead of both pitch cards, or alone,
   still gets a written ring and a running tracker. runStage's Gang case calls
   `engines.gang.process(in, out, n, params.gang, engines.pitch)`. The Gang card's own slot
   is where the gang joins the chain, so Slapback, Distortion, Gate and the reverb act on it.
5. ChainEngines gains `Gang gang` as a plain global, not DTCM. chain_params gains `gang`
   with on = false. layout.cpp gets no new rule and defaultLayout is unchanged.
   core_registry.cpp amountCards gains Gang (knob B is the amount) and the pitch group count
   becomes 3.
6. costs.h: `kGang{19, 21, false}` as the host estimate until a board bench, with the comment
   "estimate: four voices at ~28k, EQ ~6k, room ~14k". test/firmware/core_layout.cpp requires
   `measured` only for cards in `defaultLayout()`; a card outside it may carry an estimate, which
   the app already shows as "est.". layoutValid still counts the estimate, so a layout over the
   lines is refused until the bench lowers the row or the engine is cut to fit.
7. The record stores the id string, so flash layouts are unaffected. app/fixtures/hello.json is
   refreshed by `hello_fixture_refresh`; app/internal/server/server_test.go card counts move
   10 → 11.

## Emulator and render tool

1. cubevox-proto: a GANG block (checkbox, WIDTH and VOICES percent sliders, a Tuning header
   with tuning nodes) in whichever column the layout probe accepts; state keys
   `gang.on`, `gang.width`, `gang.voices`; a `gGang` global reset with the others;
   chain position after the pitch stage and before Unison, with gPitchFx passed in. Pedal mode is
   unchanged by this spec; the owner listens to the card in the module view.
   `--layout` must print ok. The tuning nodes are Formants (4 sliders, −4..+4 st), Pitch (4,
   −12..+12 st, the full values), Delay (4, 5..200 ms, and lagScale 0.5..2), Tone (4 tiltDb and
   tiltHz), Scatter (scatterMs 0..30 ms, driftCents 0..50) and Solo (4 checkboxes). Print tuning lists every GangTuning field. The Gang
   tuning has no state keys.
2. cubevox-render: `--gang`, `--width <0..1>`, `--voices <0..1>`; tuning keys prefixed `gang`
   (`gangVoiceDb`, `gangTrimDb`, `gangFormantSt0..3`, `gangPitchSt0..3`, `gangTiltDb0..3`,
   `gangTiltHz`, `gangDelayMs0..3`, `gangLagScale`, `gangScatterMs`, `gangDriftCents`, `gangHpHz`,
   `gangMaxLagMs`). `pitchOn` becomes true when the gang card is on. tools/engine_ab.sh gains a
   `gang` case and a gang term in the chain case.

## Tests

1. `test/gang_test.cpp` (CMake line `cubevox_engine_test(gang)`), on a PitchFx fed the test
   signal through its pass-through path so the ring and tracker are live:
   1. off is bit exact (`==`, after 100 ms).
   2. no allocation inside process (the operator new abort pattern of octave_test.cpp).
   3. pitch moves by pitchSt: one voice alone (VOICES 0.2, WIDTH 1): output f0 within 10 cents
      of 220 × 2^(pitchSt / 12) on a 220 Hz tone, for each of the four voices (Goertzel helpers
      copied from octave_test.cpp).
   4. formant moves: centroid over harmonics 1..10 at +1 st (no default copy has a shorter throat;
      the probe holds the lag at 90 ms) at least 1.03 × the formant-0 centroid, and at voice 0's
      (−1.75 st) at most 0.94 ×.
   5. level law: total RMS at VOICES 0.2, 0.4, 0.6, 0.8 and 1.0 within −1.5..+1.0 dB of the input
      on the 180 Hz program.
   6. the lead leaves: at VOICES 1.0 a probe accessor reports lead gain 0 after the ramp.
   7. voice count: at VOICES 0.2 only voice 0 ticks and at 0.8 all four (probe accessor).
   8. the lag follows lagScale across a gap: on 220 Hz bursts of an irregular envelope with 120 ms
      of silence between them, one copy alone (formant 0, pitchSt 0) has its cross-correlation peak
      at delayMs × 1.2 (90 ms for copy 4, 120 ms for copy 2) and, after lagScale goes to 2, at
      delayMs × 2 capped at 200 ms (150 ms and 200 ms). The move comes mid-burst, the lag stays
      unchanged to the end of that burst, and the next burst sounds at the new lag (±0.5 ms).
   9. tracker runs without Autotune: PitchFxParams with autotune.on false and gangOn true gives
      period > 0 on the tone within 60 ms.
   10. voices differ: voice 0 alone and voice 1 alone on the 220 Hz tone differ in spectral
      centroid (harmonics 1..20 of each voice's own f0) by at least 5 % (8.9 % at the defaults).
   11. scatter: scatterMs 15 on copy 4 over ten bursts, each burst's lag within ±15 ms of 75 ms
      and varying from burst to burst; f0 inside each burst within 10 cents of the copy's offset.
   12. drift: driftCents 20 on a 6 s tone, the copy's f0 in 0.5 s windows within ±20 cents of its
      offset (3 cents of measurement slack) and not the same in every window. driftCents 0 leaves
      the drift at exactly 0 and the output unchanged by driftHoldS.
   13. WIDTH law: voice 0 alone at WIDTH 0 has its f0 within 2 cents of the lead's 220 Hz, at 0.5
      within 10 cents of half its offset, and at 1 within 10 cents of −1.75 st. Its lag reads the
      same at all three.
   14. fade order: at VOICES 0.2, 0.4, 0.6 and 0.8 the output has a spectral line at the f0 of each
      voice that has come in (at least 0.4 of the loudest line) and none at the f0 of the voices
      still to come (at most 0.1), so the order is −1.75 st, +2.75 st, −3 st, +1 st.
2. level_test: a Gang card row at VOICES 0.5 and WIDTH 0.5 in the 0..+0.5 dB band, as a lambda
   row driving PitchFx then Gang (the two-stage row pattern at level_test.cpp:235-243).
3. test/firmware: core_registry (amountCards, engineValue, knobs move, pitch group count 3),
   core_chain_run (a Gang card rig; PitchFx runs with the Gang card on and both pitch toggles off; the
   Gang ahead of Autotune in a layout still gets a ring; bit-exact pass when off), core_layout
   (a layout with the Gang card in slot 4 and Unison out sums against the lines with the estimate;
   the measured rule change), parity_test sweepGang, knob_step runCard case.
4. Gates: ctest `100% tests passed`; `cubevox-proto --layout` ok; `cd firmware && ./build.sh`
   ends `[warn-gate] ... ok`; `BENCH=1 ./build.sh`.

## CPU and memory

| Piece | Estimate | Basis |
|---|---|---|
| one voice, linear reads | ~28k cycles, 4.4 % | PsolaVoice 22k with launch-time weights; per-sample cubic reads were 36k |
| four voices | ~112k, 17.5 % | |
| five biquads | ~4k, 0.6 % | POLISH EQ's three biquads at 9.2k on the host |
| card | ~17 % mean, ~19 % worst | the first estimate with the room (19 / 21) less the room's ~14k and three biquads; costs.h keeps 19 / 21 until a board bench |
| headroom under the lines in today's default layout | 4 % mean, 12 % worst | costs.h sums 71 / 83 against 75 / 95; a layout with the Gang card gives up whatever cards the owner chooses |

The bench decides, on the board per the costs.h method with the Gang card placed in slot 4 by
`set_layout`. Fit order if over: three voices (−4.4 %); then the voices at half rate, reading
every other ring sample and upsampling through a half-band, which halves the voice cost; then a
CPU pass on Spring (15 %). Memory: four voices' grain state and five biquads sit in the plain
global, not DTCM. The shared input ring grows from 100 ms to 230 ms (11,040 floats, 44,160 B) so
a 200 ms lag plus the longest grain fits. It sits in PitchFx, which is in DTCM, so the ring adds
24,960 B there and the firmware build reports DTCM at 76,928 B of 130,816 B (58.8 %).

## Steps

1. The agent working the engine files: gang.h, the pitch_fx.h accessors and gangOn,
   gang_test.cpp, the CMake line, the level_test row. Gate: ctest.
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

1. The one highpass runs on the gang sum after the per-voice gains, not one per voice. The four
   voices share one setting, so the sound is the same and three biquad runs go.
2. `trimDb` defaults to +2.1 dB; the level_test row at VOICES 0.5 and WIDTH 0.5 reads +0.26 dB,
   and the level law reads +0.07 to +0.92 dB across VOICES 20 % to 100 % at WIDTH 0.5. At VOICES
   100 % the gang reads −0.74 dB at WIDTH 0 and +0.72 dB at WIDTH 1.
3. `PitchFx::correctionSemis()` follows Autotune's fade down and reads 0 once it has settled off, so
   the voices return to the sung pitch within 200 ms of the switch; before, it held the last value.
4. The read delay is the output lag; the correlation peaks sit at the lag the copy holds, 89.98 ms
   and 149.98 ms for copy 4 and 119.98 ms and 199.98 ms for copy 2.
5. Host cost at -O2, same machine and run: Gang at VOICES 0.8 reads 3.3 us per block against Unison's
   0.91 us, a ratio of 3.64 (3.85 for the build with the room). Scaled by Unison's 46.9k board
   cycles, four voices land near 171k cycles. The cost row stays the 19 / 21 estimate until a board
   bench; layoutValid refuses the card in today's default layout with Unison out at 82 / 96 against
   75 / 95, so the bench needs a bench op or a lighter layout.
6. Solo (owner 2026-10-31): GangParams::solo, a bit per voice, emulator tuning hook; the firmware never sets it.

### Simplified (owner 2026-10-31)

After tuning in the emulator the owner said: "the room doesn't really help. distance doesn't have
much change except to add an eq. wobble is kind of subtle to the point that i can't tell what it
does. let's simplify things. remove the room reverb. ... distance should add more time between each
voice and also add delay (not an echo but a lag) with a ceiling of 200 ms. ... the goal is to
emulate the effect of several other people saying the same lines as adam. adam has a sing-speaking
vocal delivery so harmony is not in the question and pitch tracking is not important."

1. Gone: the room and its send, the DISTANCE-driven highpass and high shelf, the sine wobble (lfoHz,
   lfoCents, the phase accumulators) and the DISTANCE smoothing. The gang sum keeps one fixed
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
   50}, tiltDb unchanged, voiceDb −9, trimDb +2.1. The Gang card's cost row in costs.h is not
   re-benched; its comment still names the room.
6. The emulator's Gang tuning nodes are Formants, Pitch, Delay, Tone, Scatter and Solo. The Levels
   node (voice level, trim, lead leaves from, fade share) went with the others; those four fields
   keep their defaults and print with the tuning.

## Rule (owner 2026-10-31)

"They need to sound like actual people though. Not an alien, not a cartoon." Every move a copy
makes stays inside what a human voice does. Copies read as people inside about ±5 semitones of
pitch and ±3 semitones of formant, paired the natural way, the lower pitch with the longer throat.
Timing scatter stays under about 25 ms, pitch drift under about 30 cents, loudness scatter under
4 dB, breath mixed under a copy and never over it. The cutting of the copies must not add buzz or
doubling that reads as a machine; epoch alignment exists for that. The pinned sound is the tag
`gang-pin-2026-10-31` on main at v2.00.63.

## Enhancements (owner 2026-10-31)

Four controls make a copy read as a person and not as Adam shifted. The sound the owner pinned is the
tag `gang-pin-2026-10-31` (v2.00.63). Every control defaults to off or neutral, and with all four at
their defaults the output is bit for bit the pinned engine's. Each range stays inside the Rule above,
and the engine clamps a value to its range.

### Controls

1. Loudness scatter, `levelScatterDb`, default 0, range 0..4 dB. At each copy's quiet gap, where the
   timing scatter already re-rolls, the copy also draws a gain offset uniform in
   [-levelScatterDb, +levelScatterDb] from its own xorshift stream and holds it to the next gap. The
   offset multiplies the copy's gain through `dbToGainFast`. The gap rule runs while level scatter is
   on even when timing scatter is 0, and a gap that draws no new lag still draws a new loudness. At 0
   no draw is taken, so the stream and the timing scatter match the pinned engine.
2. Contour, `contour[4]`, default {1, 1, 1, 1}, range 0.6..1.4. A running mean of log2(period) follows
   the tracker's period with a time constant of about 1 s, updated while the tracker reports a voiced
   frame and held otherwise. Adam's deviation is mean - log2(period), in octaves, positive when he is
   sharp of his mean. A copy's ratio gains 2^((contour - 1) x deviation), so the copy sits `contour`
   times as far from its own mean as Adam sits from his. 1 follows him exactly, 0.7 flattens the
   copy's rises and falls to 70 % of his, 1.3 exaggerates them. The term is added to the exponent
   that `exp2Fast` takes once per block, and a contour of exactly 1 adds exactly 0.
3. Breath, `breath[4]`, default {0, 0, 0, 0}, range 0..0.15, and `breathHz` 2000 Hz. For a copy with
   breath above 0, white noise from the copy's own xorshift goes through one Butterworth high-pass at
   `breathHz`, then is multiplied by an envelope follower on the copy's own output (one-pole on the
   absolute value, 10 ms attack and 60 ms release) and by the breath amount, and is added to the copy
   after its tone shelf and before its gain. The follower reads the copy before the noise joins it, so
   the noise sits under the copy and falls silent with it. At full breath the noise reads 20 dB under
   the copy. The amount ramps across a chunk, and a copy at breath 0 runs the pinned code.
4. Epoch align, `epochAlign`, default false, with `epochSearch` 0.25 (range 0..0.3 periods). The cut
   of every grain moves from the period grid to the glottal pulse nearest the grid mark.
   1. PitchFx writes a shadow ring beside the input ring on every sample. It holds the input through
      a one-pole at 800 Hz at the same index and is exposed as `shadow()`. It costs one multiply-add
      per sample and moves no other output, so the toggle is bit exact whether or not the ring is read.
   2. A shared `EpochTrack` (`engine/gang_epoch.h`) finds the pulses once per period for all four
      copies. It expects the next pulse one tracker period after the last, takes the tallest sample of
      the track's sign within +-`epochSearch` periods of that spot, and places the pulse at the
      centroid of the lobe above half its height. The first search of a track takes the largest
      absolute sample of a whole period and keeps its sign. A ringing voice has positive and negative
      lobes nearly as tall, and following one sign keeps the track from flipping between them.
   3. A copy's launch asks the track for the pulse nearest its grid mark and cuts the grain there. A
      pulse more than half a period from the mark leaves the grain on the grid. The grid still decides
      when a grain is laid down, so the copy's pitch is unchanged.
   4. A copy cut on the pulses sits at most half a period (measured 0.7 ms at 220 Hz) from the lag it
      has on the grid, since it takes the nearest pulse.

### Tuning

```
levelScatterDb = 0        // 0..4 dB
contour        = {1, 1, 1, 1}   // 0.6..1.4
breath         = {0, 0, 0, 0}   // 0..0.15
breathHz       = 2000
epochAlign     = false
epochSearch    = 0.25     // 0..0.3 periods
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
2. Breath draws from its own xorshift per copy, not the copy's lag or drift stream, so the scatter and
   drift sequences stay put when breath turns on.
3. The tests range over the Rule's ranges (contour 0.6 and 1.4, breath 0.15, loudness scatter 3 dB),
   and the breath test measures above 4 kHz, since the tone's tenth harmonic sits at 2.2 kHz.
4. The pulse search follows one sign and takes a centroid. A first cut that took the largest absolute
   sample and a parabola flipped between the positive and negative lobe of the ring, 30 samples apart,
   and read 26 dB worse than the grid on the pulse train.

### Measured

Host, -O2, 48 kHz, `test/gang_test.cpp` unless noted.

1. Bit exactness. The 180 Hz program at VOICES 70 % and WIDTH 50 % hashes to b5704f6967ce7680 before
   and after; the burst program at 80 % and 50 % to e8ec16a472153926; the burst program at 60 % and
   80 % with timing scatter 15 ms and drift 20 cents to d6b873e27ac6a7d8. The test prints these three.
   A scratch set of seven runs (those three, the program at 100 % and 100 %, at 20 % and 100 %, with
   scatter and drift, and with a solo) hashes to 02532a5f1bdc5ad1 combined on the tag and on the edited
   engine. Inactive settings (breathHz, epochSearch, negative values that clamp to 0) move no sample.
2. Loudness scatter 3 dB over ten bursts: per-burst gain -2.97, +2.90, +2.88, +2.79, -0.23, -1.09,
   +0.10, +2.47, -1.41, +1.93 dB against the same run with no scatter.
3. Contour on a 220 Hz tone swinging 1 st either side: copy swing over input swing 0.624 at 0.6 and
   1.000 at 1.0 and 1.385 at 1.4 at 1 Hz; 0.627, 1.000 and 1.383 at 3 Hz.
4. Breath 0.15: +20.3 dB above 4 kHz over the no-breath run in the bursts, -13.5 dB at the end of a
   120 ms gap re the bursts (the follower releases over 60 ms), and the noise alone -20.0 dB re the
   copy.
5. Epoch align on a 150 Hz Rosenberg pulse train through two formants with 0.4 % period jitter,
   harmonics 2..10: formant +2 st copy 31.5 dB harmonic to interharmonic cut on the grid, 34.7 dB cut
   on the pulses (+3.2 dB). A copy at -3 st and formant -1.5 st reads 31.0 dB cut on the pulses and
   23.7 dB on the grid against 32.1 dB for the input, so the pulse cut stays 1.2 dB under the input.
   The marks sit within 0.16 samples (spread) of the pulses, which jitter 1.3 samples themselves.
6. Epoch align on Adam's three recordings (`samples/`, scratch measurement outside the test), the
   four default copies alone, harmonics 2..4 in steady 120 ms windows. Each row gives the harmonic to
   interharmonic ratio on the three recordings, grid cut then pulse cut. Copy 1 (-1.75 st, -1.75 st)
   33.3, 36.6, 33.1 then 36.2, 38.8, 35.1 dB. Copy 2 (+2.75 st, -1 st) 19.4, 17.3, 13.5 then 31.3,
   28.9, 27.6 dB. Copy 3 (-3 st, -1 st) 28.0, 26.4, 23.5 then 35.3, 36.1, 32.5 dB. Copy 4 (+1 st,
   -1.5 st) 27.5, 26.8, 25.4 then 34.7, 34.9, 32.9 dB. The pulse cut is better in all twelve cases, by
   2.0 to 14.2 dB, and reads 0.0 to +1.2 dB louder.
7. Host timing, Gang alone at VOICES 80 %, WIDTH 50 %: 3.36 us per block on the pinned engine, 3.44 us
   with every control at its default, 3.92 us with all four on. PitchFx pass-through 1.38 us per
   block on the pinned engine and 1.60 us with the shadow ring.

### Memory and cost

1. The shadow ring adds 44,160 B to `gPitchFx`. The firmware build prints DTCM at 121,088 of
   130,816 B (92.6 %), 9,728 B free; `gPitchFx` is 121,080 B. The Gang card's plain global grows by
   about 2.3 KB (the pulse track and the breath state).
2. The shadow write moves every pitch card's cycles by a few per sample, and the pulse track and the
   breath add cost only while they are on. `costs.h` keeps its estimate until a board bench.

### Emulator and render

1. The Scatter node gains Level scatter. New nodes Contour (four sliders, 0.6..1.4) and Breath (four
   sliders, 0..0.15, and Breath above in Hz). The Pitch node gains an Epoch align checkbox and an
   Epoch search slider. Print tuning lists every new field.
2. cubevox-render keys `gangLevelScatterDb`, `gangContour0..3`, `gangBreath0..3`, `gangBreathHz`,
   `gangEpochAlign` (0 or 1) and `gangEpochSearch`. A value outside its range is refused.
3. The firmware never sets any of them.
