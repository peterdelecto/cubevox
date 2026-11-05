# cubevox — Crowd card, Mob on the box (eight copies of the lead, DIVERGE and VOICES)

Date 2026-10-31. Owner design in chat 2026-10-31 and 2026-11-01; this file is the implementation
reference. The first part states the card as it is at v2.00.79. The log at the end records how it got
there, with the commit that holds each thing that was built and then removed.

Since 2026-11-05 Mob has a ring and a tracker of its own (`2026-11-05-pitch-split-design.md`). With an
Autotune slot ahead, that slot writes Autotune's input into Mob's ring each block, keeps its tracker running
for Mob, and Mob reads the tracker and Autotune's correction through `CrowdPitch`. With none ahead, Mob writes
its own input and runs its own tracker. `PitchFx`, `PitchFxParams::crowdOn` and the pitch stage's `ring()` and
`head()` named below are gone. Mob is the one card exempt from the chain-order rule.

Name (owner 2026-11-01): the card is **Mob**; the menu and readout show MOB, in capitals like every menu value (owner 2026-11-02). The registry's display name is Mob from
v2.00.82; the card id `crowd`, the engine, the cost row and the file names keep the old name until
a rename pass. The menu row after REVERB swaps the slot that holds Unison or Mob between the two
(`2026-10-24-polish-design.md`, menu section). Board bench 2026-11-01 (bench image with Adam's
5.5 s clip, knobs at top, all slots on, two runs of 24 s): Mob in slot 3 of the crowd bench layout
78.7k cycles mean (12.3 %), lowest worst block 94.97k (14.8 %), second run worst 101.0k; Octave's
slot carried the tracker at 47.5k; zero late, overrun or dry blocks. The stress run of the same
day (`2026-11-01-stress-bench-design.md`, all slots forced on, knobs at top then WIDTH and VOICES
swept) put the lowest run worst at 15.6 % in the default layout with Mob for Unison and 15.9 %
with Chasm as well, so the row is `kCrowd{13, 16, true}` from v2.00.83: mean from knobs at top,
worst from the sweeps. That layout sums to 72 % mean and 84 % worst against the 75 / 95 lines,
and the menu's swap to Mob goes through.

## Purpose

Adam sings in a speaky, near-monotone style, so effects that need a key do not serve him. The Crowd
card makes a crowd out of his own voice: up to eight copies, each another person, saying his lines a
little after him. Four are the tuned copies the tuning lists; each has a raised copy that shares its
throat, tone shelf and contour, sits 1.75 st higher and comes 75 ms later, with its own scatter and
drift (owner 2026-11-01). Owner 2026-10-31: "we are supposed to be offsetting by a few semitones and
formants to give the effect of the voice belonging to a totally different person. then we use a
little lfo or delay to make it sound like it's not the same performance." Each copy has a pitch
offset of a few semitones, a formant offset (a throat), its own tone shelf, and its own lag behind
the lead, so it is another performance. The crowd as a whole has a fixed low cut. The card never
reads the key. It reads the pitch period from the tracker Autotune uses.

The card cuts on one pitch, between about 70 Hz and 1 kHz, so a single melodic line works through it
and a chord does not. The bottom tenth of WIDTH keeps that line in tune (WIDTH law below). Outside
the tracker's range the copies fall back to plain delayed doubles (Per-voice chain, item 4).

## Rule (owner 2026-10-31)

"They need to sound like actual people though. Not an alien, not a cartoon." Every move a copy makes
stays inside what a human voice does. Copies read as people inside about ±5 semitones of pitch and
±3 semitones of formant, paired the natural way, the lower pitch with the longer throat. Timing
scatter stays under about 25 ms, pitch drift under about 30 cents and loudness scatter under 4 dB.
The engine clamps every tuning value to its range, and the render tool refuses a value outside it.

## Decisions (owner 2026-10-31)

1. A new library card and its own effect, id `crowd`, name Crowd. It is not a replacement for
   Unison and is not compared against it. Which slot it takes is a layout decision for the owner;
   the default layout does not hold it. Owner 2026-11-01: a layout would hold Crowd or Unison, not
   both; whether Crowd takes Unison's place in the default layout waits on the board bench (CPU and
   memory below).
2. Lower pot VOICES, the amount knob. 0 is dry. The copies fade in one pair at a time, a tuned copy
   with its raised copy, then the lead fades out, so the top of the knob is the crowd alone. Owner:
   "if the effect is dimed, we don't hear Adam, we hear the crowd." The lead leaves from 80 % of the
   knob, so each pair owns a round fifth of the travel and the percent readout tells the count (20 %
   one pair, 40 % two, 60 % three, 80 % all eight). The owner tuned on 80 % all day.
   Superseded 2026-11-01 (owner: "0 - 8 - MOB"): VOICES is a stepped knob of ten positions, 0 to 8
   copies one at a time, then MOB with the lead gone; the readout shows the count, or MOB. The
   stepped law is under "VOICES law" below.
3. Upper pot DIVERGE, named WIDTH until 2026-11-01 (owner: "width should possibly be called
   DIVERGE"; WIDTH suggested stereo, and the knob moves the copies away from the lead). It spreads the copies' pitch and formant offsets and stretches their lags, so
   the bottom is eight plain delayed doubles of the lead on its own pitch and the top is the crowd at
   its full offsets and longest lags. Owner: "nix the distance knob and replace it with a 'width
   knob'. the width knob will spread the voices." The bottom tenth is for instruments and melodic
   lines. At the top the pitch offsets reach two thirds of their listed values: "width shouldn't
   cause so much variation in pitch. cut its extremity by 1/3 at its max."
4. The tracker runs whenever the Crowd card is on. Autotune is not expected on with this card, and
   the card needs neither Autotune's tracker precision nor exact timing (owner 2026-10-31), so the
   tracker skips its fine pass while Autotune is off.
5. The copies read the pitch front end's ring, the voice as Autotune left it, and add Autotune's
   correction to their ratio, as Octave does. Octave's wet is not doubled.
6. Eight copies (owner 2026-11-01: "go but we need to make sure that it is cpu efficient"; "for the
   4 new voices, lag them all by an additional 75 ms"). Copy v + 4 is tuned copy v raised 1.75 st
   (`kRaisedSt`) and lagged 75 ms more (`kRaisedLagMs`), with its own scatter and drift seeds; every
   copy carries the four-copy gain over √2 (`kCopyGain`), so the crowd's power at each VOICES
   position is the four-copy card's. With the listed offsets the raised copy of copy 0 (−1.75 st)
   sits on the lead's pitch and the raised copy of copy 3 (+1 st) sits on copy 1's (+2.75 st); the
   owner heard both and kept 1.75 st. The ring grows to 225 ms (`kCrowdMaxLagMs`), 51,024 B, for
   a raised copy 75 ms behind a tuned copy at the 150 ms ceiling.
6. The defaults are the owner's working set, tuned by ear in the emulator on 2026-10-31 (Interface
   below). The firmware never sets the tuning; the box runs the struct's defaults.

## Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | DIVERGE | continuous, 0..100 % | `DIVERGE 35 %` |
| lower (knob B) | VOICES | stepped, 10 positions | `VOICES 3`, `VOICES MOB` |

Card id `crowd`, name `Mob` on the box, Group Pitch, Engine Crowd. DIVERGE is the registry's percent
knob with no decimals. VOICES is a stepped knob with named positions, as Autotune's KEY is: `0` to
`8`, then `MOB` (owner 2026-11-01).

### VOICES law, position k in 0..9 (2026-11-01)

1. Position k in 0..8 has exactly k copies in and the lead playing; position 9, MOB, has all eight
   in and the lead gone. 0 is Adam alone, the same sound as the toggle off, with the tracker
   running so the first copy enters clean.
2. Copies enter one at a time, each tuned copy followed by its own raised partner: tuned 0, raised
   4, tuned 1, raised 5, tuned 2, raised 6, tuned 3, raised 7. A copy not in is silent and does
   not tick.
3. Total power is unity at every position. The copies keep the fixed gain g of the earlier law; the
   lead gain is sqrt(1 − Σ g²) over the copies in, never below 0. At MOB the lead is 0 and the
   eight copies are scaled together to unity power, as span B's top was.
4. A step change glides on the engine's existing per-sample gain ramps, so a copy entering or
   leaving and the lead moving take the fade they took before; nothing clicks.
5. The engine takes the position as `voicesStep` (0..9), not a float. `solo` is unchanged.

### VOICES law until 2026-11-01, knob position k in 0..1 (superseded)

1. Span A, k in [0, 0.8]. u = k / 0.2 runs 0..4. Pair v (0..3, fade order below), tuned copy v and
   raised copy v + 4, has weight w_v = sin(π/2 · clamp01((u − v) / holdFrom)). Each pair fades in
   over the first three quarters of its fifth and holds for the last quarter.
2. Span B, k in (0.8, 1]. s = (k − 0.8) / 0.2. Every w_v = 1. Crowd power rises from P_8 to 1 and
   the lead falls to 0.
3. Level law at every k. Each copy carries the fixed gain g = 10^(voiceDb / 20) / √2 (`kCopyGain`);
   with voiceDb −9 dB eight copies sum to the power P_8 = 8 g² ≈ 0.50, the four-copy card's. Crowd
   power is P = Σ (g w_v)² over the eight in span A and P = P_8 + (1 − P_8) · sin²(π/2 · s) in
   span B. The lead gain is sqrt(1 − P). The crowd multiplier is sqrt(P / Σ (g w_v)²), which is 1 in
   span A and reaches sqrt(1 / P_8) at the top, so each copy goes from −12 dB to −9 dB there. Total
   power is unity at every k. The trim (Output below) takes up the residue level_test measures on
   the program.
4. Fade order, low, high, low, high, by tuned copy index (listed pitch, throat, delay before the lag
   law): 0 (−1.75 st, −1.75 st, 40 ms), 1 (+2.75 st, −1 st, 100 ms), 2 (−2.5 st, −1 st, 70 ms),
   3 (+1 st, −1.5 st, 75 ms); each brings its raised copy (its pitch + 1.75 st, its delay + 75 ms).
   One pair is a second singer with a higher shadow, two pairs a group, four pairs the crowd.
5. The law runs when VOICES, the solo or its tuning moves. Gains ramp per sample across the block, as
   Octave and the reverb MIX do. A copy whose ramped gain sits at 0 is reset and does not tick, so one
   pair costs a quarter of eight plus the fixed part. The live copies render first and mix four at
   a time, the crowd sum and both shelf sums held in registers.
6. Solo, `CrowdParams::solo`, a bit per copy, is an emulator tuning hook. A soloed copy sits fully
   faded in and the others are silent whatever VOICES says; the lead follows from the soloed power.
   The firmware never sets it.

### WIDTH law, knob w in 0..1

Owner 2026-10-31: "from about 0 to 10 percent, we need much less detuning. for a melodic line, should
the user want to pipe an instrument through that effect, we don't want it to be out of tune." After a
piano loop through the bottom tenth sounded discordant: "we might want to start with a baseline of
0 st per voice and maybe even 0 formant at 0 on the width slider." On the top: "at max width, add a
little bit more time spread between voices, maybe adding 20 ms to the max."

WIDTH scales the pitch offset, the formant offset, the drift and the contour of every copy, and it
stretches the lags. One shape f(w) is linear between three points. The formant offsets take f(w)
whole. The pitch offsets, the drift and the contour deviation take fp(w) = f(w) × 2/3, the constant
`kWidthPitchTop` in `engine/crowd.h`. Per copy and per block, copy v's pitch offset in semitones is
pitchSt[v] × fp(w) and its formant offset is formantSt[v] × f(w).

| Point | f (formant factor) | fp (pitch, drift and contour factor) |
|---|---|---|
| w = 0 | 0 | 0 |
| w = `widthKnee` (0.1) | `widthKneeFrac` (0.012) | 0.008 |
| w = 1 | 1 | 2/3 (0.667) |

Between the points f is linear in w, so f(0.5) = 0.451 and fp(0.5) = 0.301. At w = 0 f is exactly
0, so every copy sits at pitch ratio exactly 1 with a grain playback rate of exactly 1, and the
cutting returns the input delayed by the copy's lag (the WIDTH 0 is a delay test). A melodic line or
an instrument gets eight plain delayed doubles on its own pitch. At w = 0.1 the −2.5 st copy sits
2.0 cents off the lead and carries 1.2 % of its formant move. At w = 1 every copy sits at two thirds
of pitchSt[v] (−1.167, +1.833, −1.667 and +0.667 st) and at the whole of formantSt[v]. A pitchSt or
formantSt of 0 stays 0. The w the factor reads follows the knob through the 20 ms one-pole of
`engine/smooth.h`, updated per block, so a knob step glides the copies over a few blocks. It takes
the knob value outright on the first block after a reset, and a settled w equals the knob bit for
bit. `widthKnee` is clamped to 0.02..0.9 and `widthKneeFrac` to 0..1.

1. Drift follows fp. The drift range in cents is driftCents × fp(w): 0 at w = 0, 0.12 cents at the
   knee and two thirds of driftCents (10 cents at the default 15) at w = 1.
2. Contour follows fp. The contour a copy uses is 1 + (contour[v] − 1) × fp(w): exactly 1.0 at
   w = 0, within 0.5 % of 1.0 at the knee and 1 + (contour[v] − 1) × 2/3 at w = 1.
3. Timing scatter, loudness scatter, the tone shelves, VOICES and solo do not follow the knob.
4. The trim follows f (Output below), so the level holds at every WIDTH position.

Lag multiplier. WIDTH also moves every lag by one multiplier m(w), linear in w. m(0) is
`widthLagBottom` (0.5). m(1) is 1 + `widthLagMs` / L, where L is the longest delayMs[v] × `lagScale`
(120 ms at the defaults) and `widthLagMs` is 20 ms, so the longest copy sits 20 ms further back at
w = 1 than `lagScale` alone puts it and the others sit further back in proportion. At the defaults
m(1) = 1.1667 and m reaches 1 at w = 0.75. The lags are 24, 60, 42 and 45 ms at WIDTH 0 and 56, 140,
98 and 105 ms at WIDTH 1. m reads the knob value, not the smoothed w, so the target steps with the
knob and each copy takes the new lag at its next gap (Lag law below). `widthLagBottom` is clamped to
0.2..1 and `widthLagMs` to 0..60.

### Lag law

Tuned copy v's lag target is delayMs[v] × `lagScale` (1.2) × m(w), capped at `maxLagMs` (150 ms)
and floored at the voice's own minimum (1.5 periods plus 4 samples, 21.5 ms at the lowest period).
Raised copy v + 4's target is tuned copy v's target plus 75 ms (`kRaisedLagMs`), before scatter,
capped at `kCrowdMaxLagMs` (225 ms), the lag the ring was sized for. Only WIDTH moves them.

A copy changes its lag only in a gap. Each block it measures the mean square of the ring at its read
point (the block's samples `lag` behind the head). While a new target is pending (a tuning move, a
WIDTH move, or a fresh scatter draw), the copy counts the blocks in which the ring is under the gap
floor both at the lag it plays and at the lag it would move to. After two such blocks it takes the
new lag at once by resetting its grain marks there, and the grains in flight finish. The jump then
replays or skips only quiet material, so a copy never bends in pitch mid-word. The first lag after a
reset or a fade-in lands at once. A gap shorter than the move (the distance between the two read
points) never admits the move, so a large lag step waits for a long pause.

The gap floor follows the signal. It sits 20 dB (`kGapUnderPhraseDb`) under the recent phrase level
and never under `gapFloorDb` (−50 dBFS), so lags land between phrases over a stage's noise floor as
well as over silence. The phrase level is the loudest mean square the copies read at their lags for
the gap test, through a one-pole that rises in 50 ms and falls in 1 s, so it holds a phrase's level
through the gap after it and costs no reading of its own. A gap ends once the copy's read point
sounds 6 dB over the floor (`kGapRearm`, a power ratio of 4), so noise wandering about the floor
inside one pause counts as one gap and draws once. On Adam's clips each copy lands about three or
four times a second; on the test program, which has two gaps a second over a −40 dBFS floor, once
per gap.

Scatter. With `scatterMs` above 0, each gap gives the copy a fresh uniform random offset in
[−scatterMs, +scatterMs] on top of its target, drawn from a per-copy xorshift32 seeded at reset, so
a run repeats. The copy takes one offset per gap.

Pitch drift. With `driftCents` above 0, each copy holds a drift in cents that walks inside
[−driftCents × fp(w), +driftCents × fp(w)]. A new random target comes every `driftHoldS` (2 s) and
the drift slews toward it with a time constant of 1 s, updated per block. The pitch ratio gains the
term 2^(drift / 1200) through exp2Fast. With `driftCents` at 0 the term is exactly 0.

Filter coefficients update per block at most, and only when the tuning moves.

## Engine `engine/crowd.h`

### Interface

```
namespace cv {
struct CrowdTuning {
  std::array<float, 4> pitchSt   = {-1.75f, 2.75f, -2.5f, 1.0f};    // offset before the WIDTH pitch top (2/3 sounds at WIDTH 1)
  std::array<float, 4> formantSt = {-1.75f, -1.0f, -1.0f, -1.5f};   // grain playback rate 2^(st / 12) at WIDTH 1
  std::array<float, 4> shelfDb   = {-3.0f, -4.0f, 3.0f, 5.0f};      // high shelf gain above shelfHz
  std::array<float, 4> delayMs   = {40.0f, 100.0f, 70.0f, 75.0f};   // lag behind the lead before lagScale
  float lagScale     = 1.2f;      // a copy's lag is delayMs times this times the WIDTH multiplier
  float shelfHz      = 2500.0f;
  float voiceDb      = -9.0f;     // a tuned copy and its raised copy together against the lead at unity
  float trimDb       = 2.1f;      // level rule residue at WIDTH 1
  float trimBottomDb = 0.7f;      // the same at WIDTH 0; between, the trim follows f(w)
  float leadFadeFrom = 0.8f;      // VOICES position where the lead starts to leave
  float holdFrom     = 0.75f;     // part of each copy's fifth spent fading; the rest holds
  float hpHz         = 120.0f;    // low cut on the crowd sum
  float maxLagMs     = 150.0f;    // ceiling on a copy's lag before scatter, up to kCrowdMaxLagMs
  float gapFloorDb   = -50.0f;    // the gap floor never sits under this
  float scatterMs    = 12.0f;     // peak random lag offset taken at each gap, 0..30
  float driftCents   = 15.0f;     // peak of each copy's slow random pitch walk, 0..50
  float driftHoldS   = 2.0f;      // time between new drift targets
  float widthKnee    = 0.1f;      // WIDTH where f reaches widthKneeFrac, 0.02..0.9
  float widthKneeFrac = 0.012f;   // f at the knee, 0..1 (f is 0 at WIDTH 0)
  float widthLagBottom = 0.5f;    // lag multiplier at WIDTH 0, 0.2..1
  float widthLagMs   = 20.0f;     // the longest lag grows by this at WIDTH 1, 0..60 ms
  float levelScatterDb = 2.0f;    // each copy takes a new loudness at every gap, uniform in +- this, 0..4 dB
  std::array<float, 4> contour = {0.8f, 1.2f, 0.7f, 1.1f};  // share of Adam's pitch deviation a copy follows, 0.6..1.4
};
struct CrowdParams {
  bool on = false;
  float width  = 1.0f;   // knob A, 0..1
  float voices = 0.5f;   // knob B, 0..1
  CrowdTuning tuning;
  uint8_t solo = 0;      // a bit per copy; an emulator tuning hook, never set by the firmware
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

Off: out equals in bit for bit. The engine's clamps and the render tool's checks share one set of
constants in `crowd_detail`: offsets ±12 st (`kMaxOffsetSt`), shelf ±12 dB (`kMaxShelfDb`), delays
5..150 ms, scatter 0..12 ms (`kMaxScatterMs`, the ring's allowance `kCrowdScatterMs`, so the longest
lag never shortens at the ring's ceiling; a `static_assert` holds the shipped scatter inside it),
drift 0..50 cents (`kMaxDriftCents`), voiceDb −24..0,
both trims ±12 dB, lagScale 0.5..2, shelfHz and hpHz 20..21,600 Hz.

Constants in `crowd_detail`: `kVoices` 4 (the tuned copies, one per entry of the tuning's arrays),
`kCopies` 8 (the tuned copies, then the raised copies; copy v + 4 reads tuned copy v's entries),
`kRaisedSt` 1.75 (added to pitchSt[v] before the WIDTH pitch scaling), `kRaisedLagMs` 75 (added to
the tuned copy's lag target before scatter), `kCopyGain` 1/√2.

### Per-voice chain

ring → voice (PSOLA grains at playback rate f_v, pitch ratio r_v, read delay D_v) → × g w_v → crowd
sum, with the four tone shelves as one shared filter on weighted sums.

1. Voice. A PSOLA voice with a grain playback rate, in crowd.h. Grain length 2p, Hann from a
   1,025-entry table of 1 − cos(2πk/1024) built at compile time and read through a 32-bit turn
   counter with interpolation between entries, launch interval p / r_v, grain duration 2p / f_v, a
   per-sample read at a fractional delay that moves by (1 − f_v) per sample, grain gain
   f_v · 2 / (len / p) / r_v. Reads are linear, two taps. Grains sit on the period grid, as
   Autotune's `PsolaVoice` does. The launch search jumps from launch to launch rather than stepping
   every sample, and each grain's run splits once at the ring seam, so the per-sample loop carries
   no wrap test. A voice that starts with no grain in flight lays its first grain one output period
   older, so its overlap sums to one from the first sample.
2. Read delay D_v is the copy's adopted lag in samples (Lag law), a per-voice member set per block.
   The voice clamps it to at least 1.5 × the clamped period + 4 and at most the ring less the
   longest grain less the call's n (kBlock when the call is shorter), so a grain never reads the
   samples the pitch stage wrote this call in place of the oldest. The ring is 12,756 samples,
   265.75 ms (`kVoiceRingLen` in `psola.h`): the 225 ms lag ceiling of a raised copy, 12 ms of
   scatter and the longest grain. Autotune reads under 2,100 samples back and Octave under 3,850.
3. Ratio r_v = 2^(pitchSt[v] · fp(w) / 12) · 2^(drift_v / 1200) · 2^((contour_v − 1) · dev)
   · 2^(pitch.correctionSemis() / 12), where dev is Adam's deviation from his running mean pitch in
   octaves (Contour below), and the grain playback rate is 2^(formantSt[v] · f(w) / 12). The voice
   clamps the ratio to 0.5..2.0.
4. Period from `pitch.pitch().period`. The card holds the tracker's last nonzero period through its
   own ramp out, so switching the card off never drops a grain at the block edge. Until the tracker
   has given a period the plan holds the lead at 1 and the copies at 0. Unvoiced frames keep the
   last period. After an unvoiced run as long as the copy's own reach (its lag ceiling plus scatter:
   162 ms for a tuned copy, 237 ms for a raised one, so the copy has read past the last voiced
   sample), or while the tracker flags the note outside its 70 Hz to 1 kHz range
   (`PitchResult::outOfRange`: a dip below its lowest lag or off its slow end), the copy's pitch and
   formant moves glide to 0 through the 20 ms one-pole. Ratio and rate are then exactly 1,
   the WIDTH 0 path, exact at any period; they glide back when the note returns.
5. Tone shelves. Each copy's shelf is 1 + wh_v HP + wb_v BP, where HP and BP share one Butterworth
   pole pair at 1.05 × shelfHz. A copy adds into the plain crowd sum and, weighted, into two sums
   that one two-input biquad filters for all eight; a raised copy uses its tuned copy's weights. The
   weights come from a least-squares fit over
   0.2 to 4.8 × shelfHz to the copy's RBJ high shelf `setHighShelf(shelfHz, shelfDb[v])` when the
   tuning moves, and are exactly 0 at 0 dB; the fit sits within 0.141 dB and 0.94° of the four
   default shelves. With no copy live the shared filter's state is set to 0 once under 1e-20 and the
   filter stops, 3 blocks after the last copy leaves.
6. The crowd sum has one `setHighPass(hpHz, kButterworthQ)`, set at reset and when the tuning
   changes. Its numerator sums to exactly 0, a double zero at DC, so the wet carries no DC and needs
   no DC blocker.

### Loudness scatter and contour

1. Loudness scatter, `levelScatterDb`, default 2, range 0..4 dB. At each copy's gap, where the timing
   scatter re-rolls, the copy also draws a gain offset uniform in [−levelScatterDb, +levelScatterDb]
   from its own xorshift stream and holds it to the next gap. The gap rule runs while level scatter
   is on even when timing scatter is 0, and a gap that draws no new lag still draws a new loudness.
   The gain is worked out at the landing, not per block.
2. Contour, `contour[4]`, default {0.8, 1.2, 0.7, 1.1}, range 0.6..1.4. A running mean of
   log2(period) follows the tracker's period with a time constant of about 1 s, updated while the
   tracker reports a voiced frame and held otherwise. Adam's deviation is mean − log2(period), in
   octaves, positive when he is sharp of his mean. A copy's ratio gains 2^((contour − 1) × dev), so
   the copy sits `contour` times as far from its own mean as Adam sits from his: 1 follows him
   exactly, 0.7 flattens the copy's rises and falls to 70 % of his, 1.3 exaggerates them. The term
   is added to the exponent `exp2Fast` takes once per block, and a contour of exactly 1 adds exactly
   0. The contour reads the deviation of the current block, not of the moment a copy's lag ago,
   since every copy's grains are laid down at the tracker's current period. The contour a copy uses
   follows WIDTH (WIDTH law, item 2).

### Output

out[i] = lead(i) · in[i] + crowdMul(i) · crowd[i] · 10^(trim(w) / 20), with lead and crowdMul ramped
per sample from the previous block's values to this block's. trim(w) = trimDb · f(w) +
trimBottomDb · (1 − f(w)), exactly trimDb at WIDTH 1 and trimBottomDb at WIDTH 0. The two trims are
1.4 dB apart because a copy loses level as WIDTH moves its pitch and formant (0.3 to 1.5 dB alone on
the program, 0.5 to 2.3 dB on Adam's clips); the eight-copy card re-set both from level_test's rows
(Tests, item 2). At the bottom of WIDTH the eight exact doubles comb with
the lead, so a steady tone can read ±3 dB there; the gap landings move each copy's timing offset
(±12 ms, about ±2 periods at 180 Hz) and the comb averages out over a phrase or two. Over 30 s of
the program at VOICES 50 % every WIDTH position sits within 0.34 dB of WIDTH 1; over 3 s the bottom
fifth can still swing by up to 2 dB.

## Wiring

1. `Card::Crowd` and `Engine::Crowd` sit before `Count`; kEngineCount is 8 and kFadeHoldBlocks
   follows (core/layout_fade.h). The switch statements with a Crowd case: registry.cpp applyKnob and
   applyToggle, chain_run.cpp resetEngine and runStage, test/knob_step_test.cpp runCard,
   test/firmware/core_registry.cpp engineValue.
2. Registry row: card Crowd, id "crowd", name "Crowd", Engine Crowd, two knobs kPercent("WIDTH") and
   kPercent("VOICES"), Group Pitch, costs::kCrowd. applyCrowdKnob maps knob 0 to width and knob 1 to
   voices. applyToggle sets `params.crowd.on` and `params.pitch.crowdOn` together; the card holds its
   last period through the ramp, so the coupling does not click (Per-voice chain, item 4).
3. `PitchFxParams::crowdOn` (default false). The tracker runs when Autotune is on, when crowdOn is
   set, or while Autotune's fade is still up. Its fine pass (the 48 kHz refine, nine lags over 1,024
   samples) runs only while Autotune is on or fading; otherwise it publishes the coarse lag × 4. If
   Autotune switches on under a running Crowd card the period stays coarse for at most one hop,
   256 samples. PitchFx exposes `const VoiceRing& ring() const` and `int head() const`.
   `correctionSemis()` follows Autotune's fade down and reads 0 once it has settled off.
4. chain_run: PitchFx runs at the first slot whose card is in Group Pitch (Autotune, Octave, Crowd),
   with that slot's input, when any Group Pitch toggle is on, so a Crowd card placed ahead of both
   pitch cards, or alone, still gets a written ring and a running tracker. runStage's Crowd case
   calls `engines.crowd.process(in, out, n, params.crowd, engines.pitch)`. The Crowd card's own slot
   is where the crowd joins the chain, so Slapback, Distortion, Gate and the reverb act on it.
5. ChainEngines holds `Crowd crowd` as a plain global in D1 SRAM, not DTCM; only PitchFx and its ring
   sit in DTCM. chain_params holds `crowd` with on = false. defaultLayout is unchanged.
   core_registry.cpp amountCards holds Crowd (knob B is the amount) and the pitch group count is 3.
6. costs.h: `kCrowd{19, 21, false}` from the eight-copy host figure (2.40 µs at WIDTH 50 %, 2.47 µs
   at WIDTH 100 %) at 50.4k board cycles per host µs, a board bench still owed; `kTracker` for the
   tracker's history fill and coarse analysis, added once to a layout's sum by `layoutCost()` when
   Autotune or Crowd is placed, and `kAutotune` for the rest of Autotune, both set by the pitch
   slot's passes (`2026-10-01-autotune-design.md`). test/firmware/core_layout.cpp requires
   `measured` only for cards in `defaultLayout()`, and exempts Autotune while its row is an
   estimate. With the pitch slot's clean-up rows (`kTracker{4, 5}`, `kAutotune{5, 6}`) `layoutValid`
   says ok for the default layout (67 % mean, 74 % worst against 75 / 95) and refuses the crowdtest
   layout (81 / 89) and a default layout with Crowd in Unison's place (78 / 87, over on the mean).
   The crowdtest image bypasses the budget check.
7. The crowdtest image (`VARIANT=crowdtest ./build.sh`, label `-crowd`) puts Crowd in Autotune's
   slot. On that image the record saves the layout it held at boot, or the default when it held
   none, so the test layout plays from RAM only and a stored custom layout survives the test.
8. The record stores the id string, so flash layouts are unaffected. app/fixtures/hello.json is
   refreshed by `hello_fixture_refresh`; app/internal/server/server_test.go counts 11 cards.

## Emulator and render tool

1. cubevox-proto: a CROWD block (checkbox, WIDTH and VOICES percent sliders, a Tuning header with
   tuning nodes) in whichever column the layout probe accepts; state keys `crowd.on`, `crowd.width`,
   `crowd.voices`; a `gCrowd` global reset with the others; chain position after the pitch stage and
   before Unison, with gPitchFx passed in. `--layout` must print ok. The tuning nodes are Formants
   (4 sliders, −4..+4 st), Pitch (4, −12..+12 st, Width knee, Offsets at knee), Delay (4, 5..150 ms,
   Lag scale 0.5..2, Lag at bottom 0.2..1, Lag at top 0..60 ms), Tone (4 shelfDb, Tone shelf at
   500..8,000 Hz), Scatter (Timing scatter 0..30 ms, Pitch drift 0..50 cents, Level scatter 0..4 dB),
   Contour (4, 0.6..1.4) and Solo (4 checkboxes). Every slider but Formant and Tone shelf at is
   drawn from the engine's constants; those two keep a finer stretch inside the clamps. Print tuning
   lists every CrowdTuning field in struct order. The Crowd tuning has no state keys.
2. cubevox-render: `--crowd`, `--width <0..1>`, `--voices <0..1>`; tuning keys prefixed `crowd`
   (`crowdVoiceDb`, `crowdTrimDb`, `crowdTrimBottomDb`, `crowdFormantSt0..3`, `crowdPitchSt0..3`,
   `crowdShelfDb0..3`, `crowdShelfHz`, `crowdDelayMs0..3`, `crowdLagScale`, `crowdScatterMs`,
   `crowdDriftCents`, `crowdHpHz`, `crowdMaxLagMs`, `crowdWidthKnee`, `crowdWidthKneeFrac`,
   `crowdWidthLagBottom`, `crowdWidthLagMs`, `crowdLevelScatterDb`, `crowdContour0..3`). Every value
   must be finite and inside the engine's range, or the tool exits 2. `pitchOn` becomes true when the
   crowd card is on. tools/engine_ab.sh has a `crowd` case and a `full_chain` case that includes it.

## Tests

1. `test/crowd_test.cpp` (CMake line `cubevox_engine_test(crowd)`), on a PitchFx fed the test signal
   through its pass-through path so the ring and tracker are live. Every test but the working-set
   and pinned ones starts from a neutral tuning (no scatter, no drift, no level scatter, contour 1)
   and turns on only what it measures.
   1. off is bit exact (`==`, after 100 ms); reset; no allocation inside process.
   2. pitch moves: one copy alone (VOICES 0.2, WIDTH 1) has its f0 within 10 cents of
      220 × 2^(pitchSt × 2/3 / 12) on a 220 Hz tone, for each of the four tuned copies; each raised
      copy alone sits 116.7 ± 10 cents (1.75 st × 2/3) over its tuned copy.
   3. WIDTH law: the −2.5 st copy (formant −1 st) alone at WIDTH 0, 0.1, 0.5 and 1 has its f0 at
      0, −2.0, −75 and −167 cents from the 220 Hz lead (within 1, 1.5, 10 and 10 cents) and its grain
      playback rate at 0, 1.2 %, 45.1 % and 100 % of its full formant move (within 0.5 %). At WIDTH 0
      the output equals that of a copy with no offsets sample for sample. Its lag follows m(w).
   4. fade order: at WIDTH 1 and 0.8, at VOICES 0.2, 0.4, 0.6 and 0.8 the output has a spectral line
      at the f0 of each copy that has come in (at least 0.4 of the loudest) and none at the f0 of the
      copies still to come (at most 0.1), so the order is −1.75, +2.75, −2.5, +1 st times fp(w).
   5. formant moves: centroid over harmonics 1..10 at +1 st at least 1.03 × the formant-0 centroid,
      and at −1.75 st at most 0.94 ×.
   6. copies differ: copies 0 and 2 alone, both pitch offsets at 0, differ in spectral centroid by at
      least 5 % (6.1 % at the defaults), so the throat and the shelf alone tell them apart.
   7. level law: total RMS at VOICES 0.2, 0.4, 0.6, 0.8 and 1.0 within −1.5..+1.0 dB of the input on
      the 180 Hz program; the lead leaves at VOICES 1.0 (lead gain 0 after the ramp); at VOICES 0.2
      only copy 0 and its raised copy tick and at 0.8 all eight; solo.
   8. the lag follows lagScale across a gap: on 220 Hz bursts with 120 ms of silence between them,
      one copy alone at WIDTH 1 has its cross-correlation peak at delayMs × 1.2 × m(1) and, after
      lagScale goes to 2, at delayMs × 2 × m(1) capped at the 150 ms ceiling. The move comes while
      the copy's output sounds the burst, the lag stays unchanged to the end of that burst, and the
      next burst sounds at the new lag (±0.5 ms).
   9. scatter: scatterMs 12 on copy 4 over ten bursts at WIDTH 1, each burst's lag within ±12 ms of
      105 ms and varying from burst to burst; f0 inside each burst within 10 cents of its offset.
   10. drift: driftCents 20 on a 6 s tone at WIDTH 1, the copy's f0 in 0.5 s windows within
       ±20 × 2/3 cents of its offset (3 cents of slack) and not the same in every window;
       driftCents 0 leaves the drift at exactly 0.
   11. tracker runs without Autotune (period > 0 on the tone within 60 ms); Autotune's correction
       releases to 0 within 200 ms of its switch-off.
   12. pinned: four runs hash to fixed values, three from the neutral tuning (the 180 Hz program at
       VOICES 70 %, WIDTH 50 %; the burst program at 80 %, 50 %; the burst program at 60 %, 80 % with
       scatter 12 and drift 20) and one on `CrowdTuning{}` unchanged. Pinned at v2.00.79 for the
       eight-copy card: 9d220333937cb62b, d50d29e677f4651f, 1b6fd85bd0ca88e1, bd5aac8433058c41. Run 3
       was re-pinned at 8d7fc1ce41837fb8 on 2026-11-01 (v2.00.81) because its scatter moved from 15
       to 12 ms when the tuning range became the ring's allowance, not for an engine change. They
       move only when the sound changes on purpose; a pass that must keep them re-pins only after a
       measured comparison under −80 dB against the previous build.
   13. working set: the defaults hold the working set, and a run on them stays finite and within
       2.5 dB of the neutral card's level; inactive settings move no sample.
   14. level scatter 3 dB over ten bursts gives per-burst gains that vary inside ±3 dB.
   15. contour on a 220 Hz tone swinging 1 st either side at 1 Hz and 3 Hz: copy swing over input
       swing 0.62 at 0.6, 1.00 at 1.0 and 1.38 at 1.4.
   16. melodic bottom: the −2.5 st copy alone on the working set is within 1 cent of the lead at
       WIDTH 0, 2.0 ± 1.5 cents flat at 0.1 and 166.7 ± 10 cents flat at 1. With driftCents 15 at
       WIDTH 0.05 the largest drift is at most 0.06 cents; with contour 0.6 or 1.4 at WIDTH 0.05 a
       ±1 st sweep comes out within 2 % of the input's swing, and at WIDTH 1 within 15 % of
       1 + (contour − 1) × 2/3.
   17. width glides: 512 samples after a WIDTH step from 0.1 to 1 the copy's grain playback rate sits
       strictly between its two settled values, and a second later it equals a run that held WIDTH 1.
   18. width lag: each copy alone has its lag at 24, 60, 42 and 45 ms at WIDTH 0 and 56, 140, 98 and
       105 ms at WIDTH 1 (±0.5 ms); a WIDTH move from 0 to 1 while a burst sounds leaves copy 2's lag
       at 60 ms to the end of that burst and puts the next burst at 140 ms.
   19. WIDTH 0 is a delay: at WIDTH 0 each copy alone at VOICES 1.0 on the working set, with the shelf
       flat and the trim off, turns a steady 220 Hz tone into the gain-scaled input at the lag it
       took, through the same low cut, within 1e-4 (5.6e-5, the float rounding of the low cut). The
       same copy at WIDTH 0.1 is more than 100 times further from the delayed input.
   20. shared shelf: the shared filter matches each copy's RBJ shelf within 0.15 dB and 1°.
   21. switch with the tracker: `crowd.on` and `crowdOn` switch together on the program at VOICES
       100 % and 50 %, WIDTH 1, 50 ms after a phrase peak. The largest output step at the switch stays
       under the largest step in the 100 ms before it, both ways, and after the on switch every 6 ms
       window of the next 30 ms stays within 6 dB of the input.
   22. gap over a noise floor: on the program (−40 dBFS floor), WIDTH 1 to 0 at 5 s, every copy lands
       within 3 s of the move, copy 2 ends within 12.5 ms of 60 ms, and no copy lands more than 3
       times a second. On bursts with silent gaps every copy lands in each of the last 9 gaps.
   23. WIDTH holds the level: at VOICES 50 % on 30 s of the program, WIDTH 0, 2, 5, 10, 20, 30, 50
       and 70 % each within 0.5 dB of WIDTH 100 %.
   24. notes outside the tracker's range: copy 2 alone at WIDTH 1, 1 s at 400 Hz, 1 s of the note,
       0.5 s at 400 Hz. At 1300 Hz and at 60 Hz the copy correlates above 0.95 with itself at the
       note's period over the note's last 0.5 s, and its move share glides to exactly 0 and back to
       exactly 1. At 220 Hz the share stays exactly 1.
   25. read ceiling: one voice at rate 0.5 (formant −12 st), 72 Hz and the lag at the ceiling,
       through calls of 64, of 17/64/40 and of 200 samples; each call's own samples hold NaN while
       the voice renders and no output sample is NaN.
   26. tuning ranges: a value past its range plays exactly as the end of the range.
   27. raised copies differ: each raised copy alone sits 116.7 ± 10 cents over its tuned copy alone
       on the 220 Hz tone at WIDTH 1.
   28. raised copies lag: on the burst program each raised copy's lag is its tuned copy's plus 75 ms
       and matches the law, worst error 0.5 ms or less.
   29. plain after, per copy: a tuned copy goes plain 162 ± 1.5 ms into an unvoiced run and a raised
       copy 237 ± 1.5 ms, no voiced block is reported inside the noise, and the largest step in the
       glide is no larger than the largest step in the plain noise.
2. level_test: Crowd rows at VOICES 0.5 and WIDTH 0, 0.5 and 1 on the default tuning, in the
   0..+0.5 dB band, over 30 s of the program (the comb at the bottom of WIDTH averages out over the
   gaps). They read +0.07, +0.16 and +0.15 dB.
3. test/firmware: core_registry (amountCards, engineValue, knobs move, pitch group count 3),
   core_chain_run (a Crowd card rig; PitchFx runs with the Crowd card on and both pitch toggles off;
   a Crowd ahead of Autotune still gets a ring; bit-exact pass when off; a Crowd card with no tracker
   period passes at unity), core_layout (the tracker cost added once for Autotune, Crowd and both;
   the measured rule), parity_test sweepCrowd with a `static_assert` that the firmware uses
   `CrowdTuning{}`, knob_step runCard case, fastmath_test pinning `sinCosFast`.
4. Gates: ctest `100% tests passed`; `cubevox-proto --layout` ok with `default macros match engine
   defaults: ok`; `cd firmware && ./build.sh` ends `[warn-gate] ... ok`; `VARIANT=crowdtest` builds.

## CPU and memory

Host figures are per 64-sample block at VOICES 80 %, WIDTH 50 %, default tuning, Autotune off, on the
crowd_test program, medians of 33 runs of 10 s with binaries interleaved. The board converts at 50.4k
cycles per host µs in costs.h, which is 7.9 % of the 640k-cycle block per µs; the CPU audit's
comparison against board-measured pitch slots gave 28k for code like Crowd's, 4.4 % per µs. Only a
run of the crowdtest image on the box settles the ratio, and that bench is owed.

| Item | Host | Board at 50.4k / 28k |
|---|---|---|
| `Crowd::process`, four tuned copies alone | 1.57 µs | 12 % / 6.9 % |
| `Crowd::process`, eight copies, WIDTH 50 % | 2.40 µs | 19 % / 10.6 % |
| `Crowd::process`, eight copies, WIDTH 100 % | 2.47 µs | 19.5 % / 10.9 % |
| tracker history fill and coarse analysis, Crowd on, Autotune off, after the pitch slot's CPU pass | 0.70 µs | 5.5 % / 3.1 % |
| input ring, shared, in DTCM, 265.75 ms | 12,756 floats, 51,024 B | |
| DTCM, firmware build v2.00.79 | 75,648 of 130,816 B, 57.8 %, 55,168 B free | |
| cost row | `kCrowd{19, 21, false}` | |

Eight copies cost half again as much as four, not twice, because the live copies render first and
the mix runs four copies at a time with the crowd sum and both shelf sums in registers (bit-exact,
−0.29 µs). Ruled out for the eight copies: half-rate copies, a shared grain analysis, and sharing
marks, launches or gap sums between a raised copy and its tuned copy (their lags differ, so nothing
lines up). A bench-only lever that is not in the card: two raised voices each heard at two lags
(2.40 → 2.12 µs) needs a listen first, because copies 5 and 6 would lose their own pitch and drift.
With Crowd in Unison's place in the default layout the sums were 83 / 97 on the rows before the pitch
slot's clean-up pass, and are 78 / 87 on its rows (`kTracker{4, 5}`, `kAutotune{5, 6}`), refused by
3 on the mean; the default layout itself is 67 / 74. The pair's scaling implies 29.4k board cycles
per host µs, at which Crowd's 2.40 µs is 11 points and the swap fits at about 70 / 77. The box bench
decides.

What costs more on the M7 than on the host, from the audits' listings: the shared shelf, the low cut
and the grain launch are issue-bound rather than latency-bound, so their shares may land differently.
Left for the board: the engines' hot code in ITCM (unused today, every card would gain an estimated
0.4 to 1 %) and Crowd's hot state in DTCM, now that the pass freed the room.

## Log, 2026-10-31

Each entry says what changed and where the removed code lives. Measured figures are as recorded on
the day; the sections above hold the present values.

1. **First cut** (v2.00.55 as Doubler, v2.00.56 as Gang, v2.00.66 as Crowd). Four PSOLA copies with
   a DISTANCE knob (lag and an EQ), a room reverb send, a sine wobble, one high-pass per copy.
   Retune 1 the same day: the copies sounded too similar to the input, so each gained a pitch offset
   of a few semitones, a formant offset and its own tone shelf. Defaults 3: copy 4 to formant −1.5
   and pitch +1; lags 40, 70, 100, 75 ms. The one low cut moved to the crowd sum.
2. **Simplified** (owner: "the room doesn't really help. distance doesn't have much change except to
   add an eq. wobble is kind of subtle to the point that i can't tell what it does. let's simplify
   things ... the goal is to emulate the effect of several other people saying the same lines as
   adam"). Gone: the room and its send, the DISTANCE EQ, the wobble and its phase accumulators, the
   DISTANCE smoothing. DISTANCE became time only with a 200 ms ceiling, the ring grew to 230 ms, lag
   changes began waiting for a gap, and `scatterMs` and `driftCents` arrived at 0. Defaults then:
   formantSt {−1.75, −1, −1, +1}, pitchSt {−1.75, −3, +3, −1}, delayMs {90, 60, 100, 50}, trimDb 2.1.
3. **WIDTH** replaced DISTANCE and a LOOSE knob (owner: "nix the distance knob and replace it with a
   'width knob'"). First as a spread from ±0.01 st to the full offsets with the lag multiplier pegged
   at 1.2; then the melodic bottom tenth with a knee at 0.025 and drift and contour scaling with it;
   then the clean bottom and wider top: exactly 0 at WIDTH 0, knee 0.012, the lag multiplier from 0.5
   to a top 20 ms longer. The 20 ms one-pole on w came in when a knob-step scan found pitch spikes at
   13 start points without it and 2 with it. Measured then on the −3 st copy: 0.00, −3.53 and
   −294.86 cents at WIDTH 0, 0.1 and 1, and each copy at WIDTH 0 equal to the delayed input to 4.2e-5.
4. **Rule and enhancements.** The owner's rule, "actual people, not an alien, not a cartoon". Three
   controls, each neutral at its default: loudness scatter at each gap, per-copy contour, and
   epoch-aligned cutting on an 800 Hz shadow ring (grains cut at the glottal pulse nearest the grid
   mark, a shared pulse track with centroid placement). Breath was tried and removed by the owner.
   Epoch align measured 2 to 14 dB better harmonic-to-interharmonic ratio than the grid cut on
   Adam's three recordings, and the working set shipped with it on. The pinned sound before the
   enhancements is the tag `crowd-pin-2026-10-31` at v2.00.63. Working set: contour 0.8/1.2/0.7/1.1,
   level scatter 2 dB, timing scatter 12 ms, drift 15 cents, trimDb 1.0.
5. **Voice 5 trial** (v2.00.70). A fifth copy outside the VOICES law, meant to sound like a person of
   another sex, age or body size: its own pitch, throat, contour, tilt, breath, vocal fry, jitter and
   shimmer, three presets (Woman, Older man, Big man) and a solo. Owner: "peak and notch didn't seem
   to make a ton of difference". Commits `33be645` and `cb28d41` hold it.
6. **Voice 2 as the owner's higher voice, per-copy tilt and shimmer** (v2.00.71). Owner: "adding a
   higher pitched voice with these attributes is convincing: pitch +3 st, formant +1.65 st, contour
   1.2, tilt +2, shimmer 1 ... we can sub this for voice 2." Copy 1 took those values, every copy
   gained a tilt (−6..+6 dB) and a shimmer (0..1 dB) scaled by f(w). Reversed the same day
   (v2.00.72): "put the original voice 2 back", "get rid of voice 5", and the per-copy tilt and
   shimmer with them. Commit `cb28d41` holds the code and measurements; the defaults reproduced the
   v2.00.68 output hashes bit for bit after the reversal.
7. **Copy 3 pitch −2.5 st** instead of −3 (v2.00.73). **Epoch align off by default and the WIDTH pitch
   top at 2/3** (v2.00.74, owner: "turn off epoch align"; "width shouldn't cause so much variation in
   pitch. cut its extremity by 1/3 at its max"). The third copy then measured −166.6 cents at WIDTH 1.
8. **Audit** (four read-only reports in `build/audit/2026-10-31-crowd-audit-*.md` and the per-part
   cost in `2026-10-31-crowd-cost.md`): the card measured 3.9 µs per block on the host, 30.8 % of
   the block at the Unison ratio, against a 19 % cost row; the grain launch search alone was a
   quarter of it. Owner: "ok lets address all of those", and that the card needs neither Autotune's
   tracker precision nor exact timing.
9. **CPU pass** (v2.00.75). Host cost per step, `Crowd::process` / `PitchFx`, µs per block at load 4
   to 6: before 3.94 / 1.75; launches found per launch 2.92; Hann table and `sinCosFast` 2.68; grain
   loop split at the ring seam 2.53; DC blocker out 2.34; one shared shelf 1.75; gap detector on
   four accumulators 1.64; per-block libm cached 1.62; epoch align and the shadow ring out 1.61 /
   1.53; tracker fine pass off with Autotune off 1.62 / 1.04; ring to 190.75 ms 1.62 / 1.04. Steps
   1, 3, 6 and 7 are bit-exact (0 of 78 audit runs differ); the table sits 90 to 98 dB under; the DC
   blocker moves no band above 50 Hz by more than 0.033 dB; the shelf fit is within 0.141 dB and
   0.94°; the coarse period puts the copies within 2.9 cents p95 of the fine one on the program and
   5.8 cents on Adam's clips, voicing identical. DTCM 121,088 to 69,376 B. Removed: epoch align
   (`epochAlign`, `epochSearch`, `engine/crowd_epoch.h`, the snap, the shadow ring in `PitchFx`, the
   emulator's two controls, the render keys `crowdEpochAlign` and `crowdEpochSearch`, five tests);
   commit `7804587` (v2.00.74) is the reference. The box could never reach the control.
10. **Correctness pass** (v2.00.76). Switching off clicked (the tracker's period went to 0 in the
    ramp block; the step at the switch was 0.350 against 0.043 in the 100 ms before, now 0.012) and
    switching on left a −10.9 dB hole (now −3.7 dB): the card holds its last period and lays a
    start-up grain. The gap floor at a fixed −50 dBFS never fired over a −40 dBFS floor (0 landings in
    10 s, copy 2 frozen at 134 ms; now 21 to 28 landings and copy 2 at 56 ms): the floor follows the
    phrase level. WIDTH 0 read 1.8 dB under WIDTH 1 (one frozen draw of the comb; now 0.34 dB over
    30 s): the trim follows WIDTH and the gaps redraw the comb; `trimDb` 1.0 to 1.7 and
    `trimBottomDb` 1.0 new. Notes outside the tracker's range buzzed (1300 Hz read an octave down,
    correlation 0.52; now 1.000): the plain path on `outOfRange` or a 162 ms unvoiced run. A grain
    could read up to n − 4 samples of the current call (1,054 NaN reads; now 0). The render tool took
    nan and inf and left most keys unchecked. The crowdtest image wrote its test layout into the
    record. Cost +0.036 µs. Hashes re-pinned.
11. **Clean-up pass** (v2.00.77). The tone shelf is `shelf` everywhere (tilt was the removed per-copy
    control); Gang- and Doubler-era aliases gone; stale comments corrected; `kTracker` split out of
    `kAutotune` and `kCrowd` set from the measured host figure; the tracker's period bounds and the
    ratio and read-delay bounds live once; the copies-differ test compares throat and shelf at equal
    pitch; the tuning parity compare is a `static_assert`; test functions under 40 lines; the INDEX
    row rewritten without tuning numbers. No behaviour change: the four hashes pass unchanged.
12. **Eight copies** (v2.00.79, commit `291e968`; owner 2026-11-01: "go but we need to make sure that
    it is cpu efficient", then "for the 4 new voices, lag them all by an additional 75 ms"). A trial
    with four raised copies at +1.75 st and no extra lag was heard first; the lag came from the
    owner's ear. Each raised copy mirrors a tuned copy (throat, shelf, contour), sits 1.75 st higher
    before WIDTH, lags 75 ms further before scatter, and draws its own scatter and drift; every copy's
    gain is the four-copy gain over √2, so the level law's power is unchanged. The ring grew to 225 ms
    (51,024 B); each copy goes plain after its own reach. The live copies render first and mix four
    at a time (bit-exact, −0.29 µs). Host at VOICES 80 %, WIDTH 50 %: 1.57 µs for four copies,
    2.40 µs for eight. `trimDb` 1.7 → 2.1, `trimBottomDb` 1.0 → 0.7; the level rows read +0.07,
    +0.16, +0.15 dB. `kCrowd{13, 15}` → `{19, 21}`, still an estimate. The merged tree matched the
    card's own build on the four pinned runs at −91.7 / −96.1 / −98.0 / −93.7 dB; hashes re-pinned.
    The raised copy of copy 0 sits on the lead's pitch and the raised copy of copy 3 on copy 1's; the
    owner heard both and kept 1.75 st. On a steady tone at low VOICES the level reads −0.74 dB.
