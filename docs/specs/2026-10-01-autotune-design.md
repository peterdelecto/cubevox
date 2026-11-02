# cubevox — Autotune card and the pitch front end

Date 2026-10-01. Owner: "add a simple autotune module with key selection and response time
parameter." Adam on the V3's: "U can set the general key and the sensitivity ... I mean how
corrective." This file is the implementation reference. The first part states the card and the
shared pitch front end as they are at v2.01.13, after the Autotune fix pass of 2026-11-02. The log at
the end records how it got there and what went.

## Decisions

1. One card, id `autotune`, name Autotune, Group Pitch, two knobs. KEY is the upper pot, twelve
   positions round the circle of fifths from C (`kKeyName`, "C / Am" to "F / Dm"; the relative minor
   shares the scale). RESPONSE is the lower pot, log and reversed: NATURAL (500 ms) on the left to
   MECHANICAL (1 ms) on the right, as on the V3 (owner 2026-10-02); default 1 ms, the hard tune. No
   MIX knob; the slot's bypass toggle turns the card on and off, and it starts off.
2. The correction runs inside `PitchFx` and replaces the dry path: the corrected voice crossfades in
   over the dry input once the tracker has a period. Octave adds the correction to its interval, so
   its voice is built on the corrected note.
3. Target note: the nearest note of the key's major scale to the sung pitch, by circular distance,
   with one hold. After the first pick (after reset or a KEY change) the last note stands until the
   sung pitch sits 0.15 st (`kHoldSemis`) past the halfway point to the next one, so a pitch near the
   halfway point holds one note instead of trilling between two (trills on Adam's clips 8 → 1,
   correctness pass). A vibrato wider than ±15 cents centred on the halfway point still flips
   between the two notes, about 12 times a second at MECHANICAL. Owner 2026-11-02 left it: the
   cure picks the note from a slower pitch and makes every new note land about 50 ms late.
   Correction c = target − sung in semitones, continuous, through a one-pole with time constant
   RESPONSE. It starts from 0 after a reset and always glides at RESPONSE, so the first note after a
   switch-on glides in instead of landing corrected.
4. KEY glide. A KEY change while a note sounds moves the target by up to a semitone. The part of the
   correction that follows the sung pitch (`corr_`) moves to the new note at once and keeps following
   the sung pitch at RESPONSE, so hard tune holds through the change (a +30 cent step is followed in
   21.5 ms with or without a KEY change). The old note's side of the change (`noteOffset_`) runs down
   along 50 ms (`kKeyGlideSec`), never faster than RESPONSE. A grain is two periods, 28 ms at 70 Hz,
   so a low voice needs the long glide: moved at once, the grains laid at the old and new spacing sum
   over the steady level. A KEY change made after 16 ms without a voiced frame (`kRestSamples`) or
   after a reset has no note to protect, and the next note takes the new key in 1.3 ms; mid-note the
   move takes 116 ms.
5. The pitch the target follows is `PitchResult::rawHz`, the newest frame, while it sits within a
   fifth of the tracker's median-of-three (`kFifthRatio`, 2^(7/12)); past that the median stands in.
   The newest frame is one hop fresher than the median, which cost about 7 cents on vibrato
   (owner 2026-10-02); the fifth keeps a sung leap on its own hop (0 of 2,635 targets on Adam's
   clips delayed) while a frame read at two or three times the period never reaches the target
   (bleed on A3 under pink noise 28 → 16 pulls in the test, 60 → 40 in the audit's run). A band of
   one semitone was tried and delayed 18 real leap targets by a hop, so the fifth stands.
6. A clamped frame, where the tracker's period sits on the edge of its search (68 Hz or 1,030 Hz
   reads the edge, not the note), has target 0, so the note passes uncorrected and the correction
   eases there at RESPONSE (68 Hz −0.3 cents against +84.5 before; 1,030 Hz +0.3 against −20.5).
   Unvoiced frames hold the target and the voice keeps its last period. There is no pull range: the
   clamped rule and the fifth guard the correction, and `maxCorrectSemis` never acted at its default
   (clean-up pass).
7. The shifter is the grid PSOLA voice in `psola.h`, one voice, no formant option. It runs its two
   overlapping grains in one loop, with marks set per sample, a window in difference form and the
   inverse ratio from a polynomial (efficiency N1, 2026-11-02: 92 to 43 M7 instructions a sample). Its grain delay
   `AutotuneTuning::grainDelay` defaults from `PsolaVoice::kGrainDelay`, 1,040 samples, which lines
   the voice up with the tracker's frame for the hard tune (commit ee527fb). Latency is the grain
   delay plus one period: 22.7 ms at 1 kHz to 36.0 ms at 70 Hz. The dry path is replaced, so the box
   gains that much when the card is on; this is inherent to pitch correction and matches the V3.
8. The crossfade between the dry input and the corrected voice is equal-power: each gain is the sine
   of its own share of the quarter turn, exactly 0 and 1 at the ends, because the corrected voice lags
   the dry by 21 to 35 ms and may sit a semitone away, so the two add in power (dips at a toggle
   6.9 → 3.9 dB, rises at most 2.5 to 3.2 dB; the toggle test pins 4.4 dB dip and 3.0 dB rise). The
   fade waits for the tracker's first period so the grains start on the sung pitch, takes about
   280 ms to settle after the card goes off, and the voice resets on its enable edge only once the
   fade sits at exactly 0, so a quick off and on keeps the running voice.
9. Hard tune. A 5 Hz, ±30 cent vibrato on A3 at the defaults reads 0.84 to 1.07 cents RMS at call
   sizes 17, 40, 64 and 200, and the test gates it at 2 cents. In Adam's register the same vibrato
   reads at worst 2.40 cents at 110 Hz, 2.59 at 98 Hz and 3.00 at 82.41 Hz, gated at 5, 5 and 6.
   Each gate sits at about twice its reading, so a change that costs pitch accuracy fails a test.
   The 4 cents RMS of 2026-10-02 stays as the most a future CPU change may spend on A3, and only
   with that trade recorded here (lead 2026-11-02). On Adam's clips at v2.00.80 the median distance
   from the note was 2.53 cents and the 90th percentile 16.1 (his own box: 2.1 to 2.3 and 6.5 to 9.6).
10. Level rule: `AutotuneTuning::trimDb` is +0.35 dB, folded into the grain tap weights. Adam's clips
   set it: without it the hard tune at KEY C reads −0.01, −0.35 and −0.33 dB (mean −0.23); with it
   the `level_test` rows at KEY C read a mean of +0.11 dB at RESPONSE 1 ms, +0.32 at 100 ms and
   +0.35 at 500 ms, and every key at 1, 20, 100 and 500 ms reads +0.08 to +0.37. The synthetic row
   "autotune key C" reads +0.27.

## The pitch front end, `engine/pitch_fx.h`

1. Ring. One input ring of `kVoiceRingLen` samples, 12,756 (265.75 ms, 51,024 B), sized in `psola.h`
   for the Crowd card's 225 ms lag ceiling, 12 ms of scatter and the longest grain. The stage writes
   it every block it runs; `head()` is the index the next sample writes to. Autotune reads under
   2,100 samples back, Octave under 3,850, Crowd up to the ceiling.
2. Tracker, `engine/pitch.h`. Two-stage YIN, hop 256 samples (5.3 ms). The coarse search runs on a
   12 kHz copy (`kDecim` 4) over a 256-sample window and lags 12 to 171, so 70 Hz to 1 kHz
   (`kMinHz`, `kMaxHz`; periods `kMinPeriod` 48 to `kMaxPeriod` 686 at 48 kHz). Its per-lag sums are
   kept per hop, four hop sums to a frame, so a hop adds one and drops one, and it compares lags by
   cross-multiplying, with no divide per lag. The tracker reads its 48 kHz history from the stage's
   input ring, so the ring holds the whole call before `PitchTracker::push` runs; the tests give it
   a ring of its own (`test/ringed_tracker.h`). The fine pass compares
   the 512 samples that end `kMaxPeriod` before the newest with a copy up to `kMaxPeriod` newer, and
   walks downhill from the coarse centre within `kFineReach` 4 lags each side. A level gate (−50 dBFS
   over the newest 1,024 samples) and the YIN threshold 0.15 decide voicing. The result carries the
   median-of-three `hz`, the newest frame `rawHz`, `voiced`, `clamped` (the period on an edge of the
   search) and `outOfRange` (a dip below the lowest lag or off the slow end).
3. Gating. The tracker analyses while Autotune is on or still fading, and while the Crowd card is on
   (`PitchFxParams::crowdOn`). The fine pass runs only for Autotune; the Crowd card reads the coarse
   lag × 4, within 2.9 cents p95 of the fine one on the program. With neither reader the tracker
   idles: the hop count keeps time, the rings rest and the result reads unvoiced. On the next start
   it replays its history from the ring, its first analysis at the first hop at least eight blocks
   ahead, the rebuild spread over those blocks so no block costs more than a voiced hop block; the
   first pitch then comes 8 to 11 blocks after the start against 6 from a warm frame, and only when
   Octave alone was running.
4. Enable behaviour. The chain resets the pitch stage when every pitch card is off, so the frame is
   warm only while the stage runs. The first voiced frame after such a switch-on comes about 32 ms
   in (24 blocks), against 8 ms (6 blocks) from a running stage. The owner keeps the chain as it is:
   keeping the stage warm from all-off would cost about 1 % of the block everywhere for 24 ms once.
5. Crossfades. Three one-pole smoothers of 20 ms, stepped per sample with their gains evaluated at
   the block edges and interpolated between: Autotune's fade, Octave's fade and Octave's MIX. All
   three are equal-power. Settled smoothers hold their targets bit for bit and are skipped. With
   every stage settled off the dry gain is exactly 1 and the stage only writes the ring, so a stage
   switched off ends bit-exact on its input. `correctionSemis()` is Autotune's live correction,
   follows the fade down after the card goes off, and is exactly 0 once the fade has settled.
6. Reset rule. A voice resets on its enable edge only when its crossfade sits at exactly 0; while the
   fade is still up the running voice carries on, and the period stays set through the fade so the
   grains never lose their pitch.
7. Memory. `PitchFx` lives in DTCM, 61,420 B of the 130,816 B region after the Autotune fix pass
   dropped the tracker's own 16,384 B history copy (the ring 51,024 B of it). The firmware build
   prints `[budget] DTCM 61440 of 130816 B 47.0 %`; `engine/CLAUDE.md` and
   `firmware/cubevox/src/board/audio.cpp` carry the figure.
8. CPU, host µs per 64-sample block on Adam's clips, pooled medians of nine passes, g++ 15 (the
   firmware's compiler family; the M7 has no SIMD, so g++ stands for the board), before and after the
   Autotune fix pass. Autotune on, mean 1.794 → 1.408, voiced hop block 2.618 → 1.910; the tracker
   alone 1.018 → 0.867 and 1.589 → 1.136; Autotune with Octave 2.036 → 1.668 and 2.865 → 2.172. The
   board measured the tracker and Autotune together at 83.2k cycles mean (13.0 %) and 131.9k worst
   (20.6 %) before the 2026-11-01 CPU pass. Both passes' host ratios scale that pair to 38.3k mean
   (6.0 %) and 48.8k worst (7.6 %), split by the tracker's g++ share (62 % of the mean, 59 % of the
   hop block) and rounded up: `kTracker{4, 5, false}` and `kAutotune{3, 5, false}` in
   `firmware/cubevox/src/core/costs.h`, Autotune's worst keeping the extra point the stress run of
   2026-11-01 read. The pair is 7 / 10, pinned in `test/firmware/core_layout.cpp`. With the board
   rows of the 2026-11-02 stress run the default layout sums to 43 % mean and 57 % worst against the
   lines of 75 / 95. Both rows stay estimates until the stress run reads the pitch slot again.

## Emulator and render tool

1. AUTOTUNE block: checkbox, KEY combo, RESPONSE slider in percent mechanical with the readout
   NATURAL at 0 and MECHANICAL at 100, `Correction: %+.2f st`. Tuning node: trim and grain delay.
   Reset and Print cover the tuning; `--layout` checks the default macros against the engine's
   defaults.
2. `cubevox-render`: `--autotune`, `--atkey <0..11>`, `--response <ms>`; tuning keys `atTrimDb` and
   `atGrainDelay`. `tools/engine_ab.sh` runs its autotune case at `--response 1`.

## Tests

1. `test/autotune_test.cpp` (ctest `autotune`): passthrough off is bit-exact (`==`) and a plain copy
   300 ms after the card goes off; passthrough in tune (A3 in C, within −40 dB of the input after
   300 ms at the best whole-sample lag; the true delay is not a whole number of samples, so the
   test prints no lag); scale degrees;
   correction (A3 + 50 cents to 220 Hz ± 5 cents); B♭3 in C pulls to the nearest scale note;
   response 200 ms reaches 63 % between 150 and 250 ms after the tracker reports the step; feeds
   Octave (the twelfth lands on the corrected note); toggle level (the pinned dip and rise); halfway
   holds (the hysteresis), on A3 and on F♯2; range ends (68 Hz and 1,030 Hz pass within a cent); first
   note glides from 0; bleed (A3 with pink noise 5 dB under, KEY E: at most 18 voiced hops pulled
   0.2 st or more) at call sizes 17, 40, 64 and 200; hard tune at those call sizes and at 110, 98
   and 82.41 Hz (decision 9); toggle at G2 + 50 cents; the toggle with Formant at SHIFT −3 and MIX
   50 % over 97 to 220 Hz (dip 3.80 dB against a 4.4 dB gate, rise 2.68 against 3.0); every
   `push()` under the allocation guard.
2. `test/pitch_test.cpp` (ctest `pitch`): vibrato tracks and reads voiced; a steady tone reads its
   exact period; silence reads unvoiced; an enable spreads its rebuild over the blocks and reports
   at the end of the block that makes the frame whole; idle replay; clamped at 68 Hz and at
   1,030 Hz, not clamped inside; `outOfRange`; with `fine = false` the coarse period × 4 is
   published; no allocation.
3. `knob_step_test` walks all twelve KEY steps both ways on a 220 Hz note and holds the KEY step at
   98 Hz (1.0086 times steady-state curvature), 82 Hz (0.999) and 74.5 Hz (1.027).
4. `level_test` rows "autotune key C" and Adam's clips in 0..+0.5 dB (decision 10). test/firmware: core_layout holds the tracker row
   added once for Autotune, Crowd and both; parity_test's `AutotuneTuning` comparison is a
   `static_assert` that the firmware uses `AutotuneTuning{}`.
5. Gates: `ctest` prints `100% tests passed`; `cubevox-emulator --check` prints `emulator check:
   ok`; `cd firmware && ./build.sh` ends `[warn-gate] ... ok`.

## Log

1. **2026-10-01, first design.** Correction inside `PitchFx` on the shared tracker and ring,
   replacing the dry path; target the nearest major-scale note or, in a chromatic mode, the nearest
   semitone; RESPONSE 5 to 500 ms, default 60; shifter engine A (grid PSOLA) or B (epoch PSOLA,
   formant preserved), default B; KEY shared with Harmony through a `Link key to Harmony` checkbox;
   the correction added to every Harmony and Octave target; a four-column face with HARMONY in
   column 2; a pull range `maxCorrectSemis` 6 st called a tracker glitch guard. Of this the
   replacement of the dry path, the major scale, the one-pole at RESPONSE and the Octave feed stand.
2. **2026-10-02, matched to Adam's box** (owner). Two clips of Adam through his own autotune, one
   held note each (A♯2 and C♯3); he sings near-monotone and lets the box hold him on the note,
   artifacts included.

   | Source | median off note | 90th pct | wobble on a held note (sd) |
   |---|---|---|---|
   | His box | 2.1 to 2.3 cents | 6.5 to 9.6 | 2.7 to 5.3 |
   | Ours before (B, 100 ms) | 24 | 45 | 10 |
   | Ours then (A, 1 ms) | 2.4 | 10.6 | 1.7 |

   The correction moved to `rawHz` (the median cost one hop, about 7 cents on vibrato); RESPONSE
   became 1 to 500 ms, default 1, NATURAL to MECHANICAL; engine A became the only engine and the
   major scale the only mode (no engine B, no chromatic); KEY went to Autotune alone and Harmony left
   the firmware, the emulator and the render tool (owner 2026-10-08: no Harmony card); the crossfade
   waits for the tracker's first period and the voice resets only from a settled fade; the hard-tune
   gate of 4 cents RMS (3.8 then; 7.3 with the median pitch).
3. **2026-10-08, pitch stage CPU pass** (`2026-10-08-pitch-cpu-pass-design.md`): tracker gating by
   its readers, warm buffers, the spinning window, block-rate gains.
4. **2026-10-31, audits.** Four read-only reports on the pitch slot in `build/audit/
   2026-10-31-autotune-audit-*.md` (correctness, efficiency, clean-up, ear). Owner: "we need to
   address all of those." Three passes followed, each from a brief in `build/audit/`.
5. **2026-11-01, CPU pass** (v2.00.78, commit dc0e0dd). The fine pass walks downhill from the coarse
   centre instead of searching nine lags; the coarse frame keeps hop sums; the tracker idles with no
   reader and replays from the ring on the next start; the fine window is 512 samples ending at
   `kMaxPeriod` (hard tune 3.76 → 0.51 cents on the gate signal, Adam's median 3.93 → 2.53 cents,
   p90 20.5 → 16.1; the 70 Hz note at a −40 dB floor 0.72 → 3.59 cents, 82 Hz 0.57 → 1.60, both
   below Adam's lowest note); one divide fewer per grain (−82 to −110 dB against the previous
   build). Steps 1 to 4 bit-exact; the hop sums moved the Crowd card's coarse period by at most
   0.0002 cents. Host figures in the front end's item 8. Kept the no-wrap grain read: a per-block
   render was slower with Octave on. Ruled out: the fine pass in the next block, a fixed narrower
   reach, a 6 kHz coarse rate (Adam's median 3.93 → 4.75 cents, 7 of 1,449 hops wrong by up to
   42 cents, for about 1 %).
6. **2026-11-01, correctness pass** (v2.00.80, commit 4fbe30f). The halfway hold of 0.15 st; the
   `clamped` flag and the ease at the range ends; the equal-power crossfade; the first note gliding
   from 0; the `rawHz` guard with its fifth-wide band (a one-semitone band delayed 18 real leap
   targets by a hop); call sizes 17, 40, 64 and 200 in the tests. Cost +0.005 µs; PitchFx +8 B; the
   Crowd card's four pinned hashes unchanged. On Adam's clips at the best-fit KEY: 21 jumps and 1
   trill against 32 and 8 before. Kept: the target held through unvoiced frames (the clips were
   inconclusive and the decision stands); the chain's reset from all-off (front end, item 4).
7. **2026-11-01, clean-up pass** (v2.00.81). The grain delay's comments give the alignment reason
   and the latency law; the pull range is gone from the tuning, the emulator, its state key and the
   render tool; the warm-frame comments tell the truth; the cost rows follow the CPU pass; the tests
   cover the switched-off rule and the fine flag; one source for each constant (`grainDelay` from
   `kGrainDelay`, `kKeyCount`, the period bounds from `kMinHz` and `kMaxHz`, the Crowd scatter clamp
   from the ring's allowance); the crossfade and ring-head names are the owner's words
   (`autotuneFade`, `octaveFade`, `octaveMix`, `head`, `finePass`); `OctaveParams::on` defaults to
   false; the parity compare is a `static_assert`; the `--key` alias is gone. No behaviour change:
   the hashes and the printed figures hold.
8. **2026-11-01, KEY step and the stage's own cost** (v2.01.01).
   - KEY 4 to 5 and 10 to 9 read 1.059 times steady-state curvature, the two steps where a 220 Hz
     note gains a semitone of correction. At hard tune the correction moved 99 % of a semitone in
     4 ms, inside one 9 ms grain. A KEY change that moves the target now glides the correction at
     the slower of RESPONSE and the 20 ms knob smoother, until it sits within 2 cents of the target
     (`keyGlide_`). Worst KEY step 1.0167; hard tune on a held note is unchanged. knob_step walks all
     twelve steps both ways.
   - `kPitchStage` (4 % mean and worst, `costs.h`) is charged once when Octave or Formant is placed
     without Autotune, because `kAutotune` and `kTracker` were measured as the whole slot and carry
     the stage's mix. The figure is the board's 8.2 % for Octave alone less Octave's row. The host
     bench does not show the gap (Octave alone 1.26 µs a block, Autotune plus Octave 2.85 against
     Autotune 1.70), so the row is an estimate until a board run of Octave alone.
9. **2026-11-02, Autotune fix pass** (v2.01.13, merge 097cb3e; report `build/audit/
   report-autotune-fix.md`). The owner took every finding of the 2026-11-01 audits and efficiency
   bundle A, and the work ran in three stages.
   - No sound change: 192 of 192 renders byte-identical, 21 of 21 tracker results identical. The
     tracker reads the caller's ring (−16,384 B of DTCM); the cross-multiplied lag scan (N2); the trim
     folded into the grain tap weights; the dead `PsolaVoice::tick` and `play` deleted.
   - Below hearing: the two-grain voice (N1) and `midiOf` from `log2Fast` with one Newton step (N8),
     together −64.5 dBFS peak and −66.0 dB RMS against the output. Plain `log2Fast` flipped held
     notes (peak −15 dBFS) and was not taken.
   - Sound changes: the KEY glide (decision 4) replaces `keyGlide_` of log item 8, and `trimDb` goes
     to +0.35 dB (decision 10). A sweep of KEY steps over 70 to 130 Hz in 984 steps had 28 past
     1.05 times steady-state curvature, the worst 1.375 at 89.5 Hz; now 2 remain, both at 89.5 Hz
     (1.14), and no glide length from 40 to 100 ms clears them.
   - Lead's call on the 98 Hz KEY step (the owner delegated ear checks): ship the 50 ms glide. It
     takes the steps past 1.05 from 28 to 2, the 98 Hz step reads 1.0086, 82 Hz 0.999 and 74.5 Hz
     1.027 (main failed 74.5 Hz), and on vibrato tones the step reads alike before and after, so the
     gain is on steady notes and sung notes lose nothing. Renders:
     `build/audition/autotune-key-98hz.wav` (before, a 1 s gap, after), `autotune-fix-a.wav` and
     `autotune-fix-b.wav`.
   - Autotune toggled with Formant at part MIX: the audit read a 5.5 dB dip; on v2.01.10 it reads
     3.2 to 3.8 dB, the same as Autotune alone, because v2.01.10's seeding fixed it. The rest comes
     from the level gain's 40 ms product lagging the crossfade. A split into dry and corrected parts
     helped sine tones but not Adam's clips (dips −1.5 to −3.3 dB either way), so the engine is
     unchanged and the test pins the dip.
   - Not taken: the exact sum of G21 (it still costs one FMA a sample, about a tenth of the loop),
     bundles B (N3, N6) and C (N4), the voice skip in silence (N10, it moves the noise floor's
     timing), ITCM placement (N5, only with a board measurement).
