# cubevox — Doubler card (four formant-shifted voices, DISTANCE and VOICES)

Date 2026-10-31. Owner design in chat 2026-10-31; this file is the implementation reference.
The wiring map with file and line references is `build/audit/2026-10-31-doubler-map.md` (gitignored).

## Purpose

Adam sings in a speaky, near-monotone style, so effects that need a key do not serve him. The
Doubler makes a gang out of his own voice. It adds up to four copies, each with its own throat (a
formant move), its own slight detune, a slow wobble on that detune, its own timing behind the
lead, less proximity bass and top, and a shared small room, as if four other people stood a step
back from the mic singing the same line. It never reads the key. It reads the pitch period from
the tracker Autotune uses.

## Decisions (owner 2026-10-31)

1. A new library card, not a change to Unison. It goes head to head with Unison in slot 4 by ear
   before the default layout changes. This spec leaves the default layout as it is.
2. Lower pot VOICES, the amount knob. 0 is dry. The four voices fade in one at a time, then the
   lead fades out, so the top of the knob is the gang alone. Owner: "if the effect is dimed, we
   don't hear Adam, we hear the gang."
3. Upper pot DISTANCE. The bottom is four close doubles, the top a far gang.
4. Autotune is not expected on with this card. The tracker runs whenever the Doubler is on.
5. The voices read the pitch front end's ring and add Autotune's correction to their ratio, as
   Octave does. Octave's wet is not doubled.
6. Four voices with linear grain reads, benched first. If over budget, three voices before any
   CPU pass elsewhere.
7. Made in this spec, for the owner to confirm: the lead leaves from 80 % of the knob, not 85 %,
   so each voice owns a round fifth of the travel and the percent readout tells the count
   (20 % one voice, 40 % two, 60 % three, 80 % four).

## Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | DISTANCE | continuous, 0..100 % | `DISTANCE 35 %` |
| lower (knob B) | VOICES | continuous, 0..100 % | `VOICES 60 %` |

Card id `doubler`, name `Doubler`, Group Pitch, Engine Doubler. Both knobs are the registry's
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
4. Fade order, nearest throat first, by voice index: 0 (+0.6 st), 1 (−0.6 st), 2 (+1.5 st),
   3 (−1.5 st). One voice is a tight double, two a pair, four the gang.
5. Gains are computed per block and ramped per sample across the block, as Octave and the reverb
   MIX do. A voice whose ramped gain sits at 0 is reset and does not tick, so one voice costs a
   quarter of four.

### DISTANCE law, knob d in 0..1

| Term | at d = 0 | at d = 1 | law |
|---|---|---|---|
| timing scale on delayMs | 0.8 | 1.3 | 0.8 + 0.5 d |
| per-voice highpass | 120 Hz | 250 Hz | linear in Hz |
| per-voice high shelf at 6 kHz | −1 dB | −5 dB | linear in dB |
| room send | −18 dB | −6 dB | linear in dB |

Filter coefficients and the send update per block only, since the setters call libm.

## Engine `engine/doubler.h`

### Interface

```
namespace cv {
struct DoublerTuning {
  std::array<float, 4> formantSt   = {0.6f, -0.6f, 1.5f, -1.5f};   // grain playback rate 2^(st / 12)
  std::array<float, 4> detuneCents = {7.0f, -6.0f, 12.0f, -14.0f};
  std::array<float, 4> delayMs     = {28.0f, 36.0f, 44.0f, 52.0f};  // behind the lead, before the DISTANCE scale
  std::array<float, 4> lfoHz       = {0.11f, 0.17f, 0.23f, 0.29f};
  float lfoCents     = 6.0f;      // peak wobble on each voice's detune
  float voiceDb      = -9.0f;     // one voice against the lead at unity
  float trimDb       = 0.0f;      // level rule residue
  float leadFadeFrom = 0.8f;      // VOICES position where the lead starts to leave
  float holdFrom     = 0.75f;     // part of each voice's fifth spent fading; the rest holds
  float hpNearHz = 120.0f, hpFarHz = 250.0f;
  float shelfHz = 6000.0f, shelfNearDb = -1.0f, shelfFarDb = -5.0f;
  float roomNearDb = -18.0f, roomFarDb = -6.0f;
  float roomLpHz = 4000.0f;       // tail darkness
  float roomFeedback = 0.6f;      // 35 ms loop, about 0.45 s to -60 dB
};
struct DoublerParams {
  bool on = false;
  float distance = 0.5f;   // knob A, 0..1
  float voices   = 0.5f;   // knob B, 0..1
  DoublerTuning tuning;
};
class Doubler {
 public:
  void reset();
  // in is this slot's input, the lead. pitch has already run this block, so the block's
  // samples sit at ring index head() - n + i.
  void process(const float* in, float* out, int n, const DoublerParams& p, const PitchFx& pitch);
};
}
```

Off: out equals in bit for bit.

### Per-voice chain

ring → voice (PSOLA grains at playback rate f_v, pitch ratio r_v, read delay D_v) → highpass →
high shelf → × g w_v → gang sum.

1. Voice. A new class in doubler.h, a PSOLA voice with a grain playback rate. It is built from the
   deleted `EpochPsolaVoice` (`git show 9b715ec^:engine/shifters.h`, launch at its lines 143-152,
   play at 182-193) and the period-grid mark scheduling of today's `PsolaVoice`
   (engine/psola.h `advanceMarks`). Grain length 2p, Hann from `HannRotor`, launch interval
   p / r_v, grain duration 2p / f_v, a per-sample read at a fractional delay that moves by
   (1 − f_v) per sample, grain gain f_v · 2 / (len / p) / r_v. Reads are linear, two taps, not
   Catmull-Rom (decision 6). No epoch search in the first cut: grains sit on the period grid as
   today's PsolaVoice does. If the gang sounds rough in the emulator, the fallback is one shared
   low-passed ring and the old peak search, with the marks shared across the four voices.
2. Read delay D_v = delayMs[v] · (0.8 + 0.5 d) · 48 samples, a per-voice member set per block, not
   `PsolaVoice::kGrainDelay`. It is clamped to at least 1.5 × the clamped period + 4 (the floor
   kGrainDelay encodes for the lowest pitch) and at most kVoiceRingLen − 2 × kMaxPeriod − 8. The
   defaults keep every voice above the 21.7 ms floor at d = 0 and the farthest voice's oldest
   grain sample inside the 100 ms ring at d = 1.
3. Ratio r_v = 2^((detuneCents[v] + lfoCents · sin(lfo_v)) / 1200) · 2^(pitch.correctionSemis() / 12).
   The LFO is the 32-bit phase accumulator of unison.h, advanced per sample, sine from `sinTurns`,
   rate from lfoHz[v], evaluated once per block for the ratio.
4. Period from `pitch.pitch().period`. Period 0 gives silence from the voice, as PsolaVoice does
   today. Unvoiced frames keep the last period, as Autotune's voice does.
5. Biquads from dsp.h, `setHighPass(hz, kButterworthQ)` and `setHighShelf(hz, db)`, set per block.

### Room

One room on the gang sum, not one per voice; they stand in the same room, and their timing offsets
already give each a different pre-delay into it. Early reflections are six taps on one delay line
at 11, 17, 23, 31, 37 and 43 ms with gains 0.9, 0.75, 0.6, 0.5, 0.4 and 0.3. The tail is one
35 ms feedback loop with a one-pole lowpass at roomLpHz inside the loop and feedback roomFeedback,
fed by the reflections through two `chasm_detail::Allpass` stages of about 5 and 7 ms (include
chasm.h). Room output × room send (DISTANCE law) adds to the gang. A 5 Hz DcBlocker sits on the
wet sum before it meets the lead, per the output-safety spec.

### Output

out[i] = lead(i) · in[i] + gangMul(i) · (gang[i] + room[i]) · 10^(trimDb / 20), with lead and
gangMul ramped per sample from the previous block's values to this block's.

## Wiring

1. `Card::Doubler` appended before `Count`; `Engine::Doubler` appended before `Count`.
   kEngineCount 7 → 8 and kFadeHoldBlocks follows (core/layout_fade.h). Six switch statements gain
   a case: registry.cpp applyKnob and applyToggle, chain_run.cpp resetEngine and runStage,
   test/knob_step_test.cpp runCard, test/firmware/core_registry.cpp engineValue.
2. Registry row: card Doubler, id "doubler", name "Doubler", Engine Doubler, two knobs
   kPercent("DISTANCE") and kPercent("VOICES"), Group Pitch, costs::kDoubler. applyDoublerKnob maps
   knob 0 to distance and knob 1 to voices. applyToggle sets `params.doubler.on` and
   `params.pitch.doublerOn`.
3. `PitchFxParams::doublerOn` (new, default false). pitch_fx.h line 49 becomes
   `tracker_.push(in, n, p.autotune.on || p.doublerOn || activeAt_ > 0.0f)`. PitchFx gains
   `const VoiceRing& ring() const` and `int head() const` (the next write index, already wrapped).
4. chain_run: PitchFx runs at the first slot whose card is in Group Pitch (Autotune, Octave,
   Doubler), with that slot's input, when any Group Pitch toggle is on. Today's rule keys on
   Engine Pitch; it becomes the group, so a Doubler placed ahead of both pitch cards, or alone,
   still gets a written ring and a running tracker. runStage's Doubler case calls
   `engines.doubler.process(in, out, n, params.doubler, engines.pitch)`. The Doubler's own slot
   is where the gang joins the chain, so Slapback, Distortion, Gate and the reverb act on it.
5. ChainEngines gains `Doubler doubler` as a plain global, not DTCM. chain_params gains `doubler`
   with on = false. layout.cpp gets no new rule and defaultLayout is unchanged.
   core_registry.cpp amountCards gains Doubler (knob B is the amount) and the pitch group count
   becomes 3.
6. costs.h: `kDoubler{19, 21, false}` as the host estimate until a board bench, with the comment
   "estimate: four voices at ~28k, EQ ~6k, room ~14k". test/firmware/core_layout.cpp requires
   `measured` only for cards in `defaultLayout()`; a card outside it may carry an estimate, which
   the app already shows as "est.". layoutValid still counts the estimate, so a layout over the
   lines is refused until the bench lowers the row or the engine is cut to fit.
7. The record stores the id string, so flash layouts are unaffected. app/fixtures/hello.json is
   refreshed by `hello_fixture_refresh`; app/internal/server/server_test.go card counts move
   10 → 11.

## Emulator and render tool

1. cubevox-proto: a DOUBLER block (checkbox, DISTANCE and VOICES percent sliders, a Tuning header
   with the DoublerTuning fields raw) in whichever column the layout probe accepts; state keys
   `doubler.on`, `doubler.distance`, `doubler.voices`; a `gDoubler` global reset with the others;
   chain position after the pitch stage and before Unison, with gPitchFx passed in. Pedal mode is
   unchanged by this spec; the A/B against Unison is the two checkboxes in the module view.
   `--layout` must print ok.
2. cubevox-render: `--doubler`, `--distance <0..1>`, `--voices <0..1>`; tuning keys prefixed `dbl`
   (`dblVoiceDb`, `dblTrimDb`, `dblFormantSt0..3`, `dblDetuneCents0..3`, `dblDelayMs0..3`,
   `dblLfoHz0..3`, `dblLfoCents`, `dblRoomFeedback`, `dblRoomLpHz`, `dblRoomNearDb`,
   `dblRoomFarDb`). `pitchOn` becomes true when the doubler is on. tools/engine_ab.sh gains a
   `doubler` case and a doubler term in the chain case.

## Tests

1. `test/doubler_test.cpp` (CMake line `cubevox_engine_test(doubler)`), on a PitchFx fed the test
   signal through its pass-through path so the ring and tracker are live:
   1. off is bit exact (`==`, after 100 ms).
   2. no allocation inside process (the operator new abort pattern of octave_test.cpp).
   3. pitch stays: one voice (VOICES 0.2), formant +1.5 st, detune 0, lfoCents 0: output f0 within
      10 cents of input on a 220 Hz tone (Goertzel helpers copied from octave_test.cpp).
   4. formant moves: centroid over harmonics 1..10 at formant +1.5 st at least 1.06 × the
      formant-0 centroid, and at −1.5 st at most 0.94 × (the ±9 % move with slack).
   5. level law: total RMS at VOICES 0.2, 0.4, 0.6, 0.8 and 1.0 within −1.5..+1.0 dB of the input
      on the 180 Hz program.
   6. the lead leaves: at VOICES 1.0 a probe accessor reports lead gain 0 after the ramp.
   7. voice count: at VOICES 0.2 only voice 0 ticks and at 0.8 all four (probe accessor).
   8. DISTANCE moves timing: the cross-correlation peak of one voice against the input moves from
      about 22.4 ms at d = 0 to about 36.4 ms at d = 1 (delayMs[0] 28 × 0.8 and × 1.3), formant 0,
      detune 0, lfoCents 0.
   9. tracker runs without Autotune: PitchFxParams with autotune.on false and doublerOn true gives
      period > 0 on the tone within 60 ms.
2. level_test: a Doubler row at VOICES 0.5 and DISTANCE 0.5 in the 0..+0.5 dB band, as a lambda
   row driving PitchFx then Doubler (the two-stage row pattern at level_test.cpp:235-243).
3. test/firmware: core_registry (amountCards, engineValue, knobs move, pitch group count 3),
   core_chain_run (a Doubler rig; PitchFx runs with the Doubler on and both pitch toggles off; the
   Doubler ahead of Autotune in a layout still gets a ring; bit-exact pass when off), core_layout
   (a layout with the Doubler in slot 4 and Unison out sums against the lines with the estimate;
   the measured rule change), parity_test sweepDoubler, knob_step runCard case.
4. Gates: ctest `100% tests passed`; `cubevox-proto --layout` ok; `cd firmware && ./build.sh`
   ends `[warn-gate] ... ok`; `BENCH=1 ./build.sh`.

## CPU and memory

| Piece | Estimate | Basis |
|---|---|---|
| one voice, linear reads | ~28k cycles, 4.4 % | PsolaVoice 22k with launch-time weights; per-sample cubic reads were 36k |
| four voices | ~112k, 17.5 % | |
| eight biquads | ~6k, 1 % | POLISH EQ's three biquads at 9.2k on the host |
| room | ~14k, 2 % | six taps, two allpasses, one loop |
| card | ~19 % mean, ~21 % worst | |
| available in a default layout with Unison out | 12 % mean, 20 % worst | costs.h sums 71 / 83 against 75 / 95, Unison 8 / 8 |

The bench decides, on the board per the costs.h method with the Doubler placed in slot 4 by
`set_layout`. Fit order if over: three voices (−4.4 %); then the voices at half rate, reading
every other ring sample and upsampling through a half-band, which halves the voice cost and suits
the 6 kHz shelf; then a CPU pass on Spring (15 %). Memory: the room is about 4.6k floats (18 KB)
plus four voices' grain state, in the plain global, not DTCM.

## Steps

1. The agent working the engine files: doubler.h, the pitch_fx.h accessors and doublerOn,
   doubler_test.cpp, the CMake line, the level_test row. Gate: ctest.
2. The agent working the wiring files, on top of step 1: registry, chain, costs, firmware tests,
   emulator block, render flags, engine_ab.sh, hello.json, app test counts. Gates: all four.
3. The lead merges per the worktree rule (cpu-pass-3 spec item 6), bumps VERSION.txt in the merge
   commit; the owner listens in the emulator against Unison; a board bench fills the cost row.

## For the owner

1. 80 % against your 85 % for the lead fade point (decision 7).
2. The card name on glass is "Doubler" until you say otherwise.
