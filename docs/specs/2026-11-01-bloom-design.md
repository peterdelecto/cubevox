# Bloom card: each word restates itself as it dies

Owner 2026-11-01: "let's also adapt the 'bloom' effect from drumsynth as a possible effect for slot
7", with DrumSynthV3's BLOOM technical spec (D2, R10, code at a4149ed) pasted in. Offered for slot 7
in place of Gate, through the slot 7 row (`2026-11-01-slot-7-choices-design.md`).

Status: the card is CHATTER (display name Chatter; card id and engine stay `bloom`). Sections 14
to 17 are current. Sections 1 to 13 record the first build (v2.00.95) and the rotation build
(v2.00.99); their knob names (TAIL, BLOOM, LINGER), slice timings, rotation, memory and cost
figures are history wherever a later section differs.

DrumSynthV3 is a reference to read, never to edit, and nothing in cubevox is copied from it (project
rule). The engine here is written new from the laws below; the Bleep Labs granular code is not
vendored.

## 1. Sound

1. While Adam sings a word, the card cuts short slices of the voice and loops them under it, taking
   a fresh slice every 55 to 180 ms, so the word shimmers and stutters with its own sound.
2. When the word ends, the last good slice keeps looping and fades over the TAIL time, gritty in the
   middle of the fade and clean at its end. The word blooms out instead of stopping.
3. Slot 8's reverb follows slot 7, so the bloom lands in the room.

## 2. From the drum to the voice

| Drum (D2) | Voice (this card) | Why |
|---|---|---|
| a hit starts the sequence | a word start starts it | a voice has no hits; words are its events |
| D, the drum's decay, 15 to 1000 ms | TAIL, the upper pot, 60 to 1000 ms | the decay was the drum's own knob; Adam sets it here |
| life e^(−t/τ) from the hit | life 1 through the word, e^(−t/τ) from the word's end | a sung note does not decay until it ends |
| re-capture until 0.8·D | re-capture through the word, never onto a quieter slice | a slice from after the word would be silence or an echo tail |
| first capture 10 ms after the hit | first capture 40 ms after the word start | lands on the vowel, past the consonant |
| R10, one knob, OFF zone at the bottom | BLOOM, the lower pot; the slot toggle is the off | every card has two pots and a toggle |
| ambience (freeverb, gate, tremolo) | dropped | slot 8's reverb is the room; saves CPU |
| drum output feeds the engine | slot 7's input feeds the engine; the word key comes from the Input gate | Echo repeats and distortion ahead of slot 7 must not hold a word open |

## 3. Panel

| Pot | Name | Kind | Readout |
|---|---|---|---|
| upper (knob A) | TAIL | continuous, 60..1000 ms, log | `TAIL 300 ms` |
| lower (knob B) | BLOOM | continuous, 0..100 % | `BLOOM 50 %` |

1. Card id `bloom`, display name `Bloom`, Group None, Engine Bloom (new), cost row `costs::kBloom`.
2. Defaults: TAIL 300 ms, BLOOM 50 %.

## 4. Laws

BLOOM position `k` (0..1). Below 5 % the wet level ramps from 0 to the law's floor, so the bottom
of the knob is dry, as MIX 0 is on the other cards. From 5 % up, `x = (k − 0.05) / 0.95` and every
law is linear in `x`, the drum's R10 laws on its kept 40 % of travel:

| x | Wet level | Grain | Re-capture interval |
|---|---|---|---|
| 0 | 0.25 | 80 ms | 180 ms |
| 0.5 | 0.48 | 63 ms | 118 ms |
| 1 | 0.70 | 45 ms | 55 ms |

TAIL `D` (60..1000 ms):

| Law | Formula |
|---|---|
| life time constant | τ = max(D, clamp(1.5·D, 120, 300)) ms |
| grain length | the law's grain, capped at 0.8·D, floored at 35 ms |
| static fade start | life = 0.20, at τ·ln 5 after the word's end |
| static fade | 80 ms exponential from that level |
| close | gain under 0.001 writes exactly 0 and stops the loop |
| saturation drive | 1.4 through the word; 1.4 + 1.6·4·life·(1 − life) over the tail |

## 5. One word, step by step

1. Word start. The wet gain heads to the wet level over 5 ms. Until the first capture the loop is
   empty and the wet adds nothing.
2. 40 ms in, the first capture. It waits for the input's next zero crossing, up to 10 ms, then
   writes one grain of the law's length into the free buffer.
3. Loop. When the write completes, the grain loops at unity speed, each pass faded in and out over
   5.8 ms, and each new grain spliced in over a 5.8 ms crossfade. Grains are never under 35 ms, so
   the fades always fit.
4. Re-capture. Every re-capture interval, ±5 % from a fixed-seed LCG so renders repeat, a fresh
   slice is cut while the word lasts. A slice whose peak sits more than 12 dB under the looping
   grain's is skipped, so a good grain is never traded for a quieter one.
5. Word end. Re-capture stops and the last grain keeps looping. Life runs e^(−t/τ) from here; the
   wet gain rides wet × life; the drive follows its bell.
6. Static fade and close as in the laws. A new word before the close restarts at step 1, with the
   wet gain gliding up from where it sits.

## 6. Word key

1. A word is the Input gate's detector open: above its THRESHOLD, with its 3 dB hysteresis and
   50 ms hold. Adam already sets that threshold to his room, so Bloom needs no threshold of its own.
   The detector runs while the Input gate's slot is off (it feeds `floor_dbfs` today).
2. `cv::Gate` exposes `bool open() const`; `runChain` hands the Input gate's state to the Bloom
   engine for the block.
3. A layout without the Input gate keys Bloom from its own input: open while the block peak sits
   above −45 dBFS, with the same 3 dB hysteresis and 50 ms hold.

## 7. Engine (`engine/bloom.h`)

1. Two float grain buffers of 80 ms at 48 kHz, 3,840 samples each, 30,720 B, in D1 SRAM with the
   other slot engines. One loops while the other takes the next capture.
2. Tone on the wet only, in this order: low-pass 11 kHz Q 0.70 (RBJ biquad), the soft-clip
   `y = x − x³/3` after the drive gain and a hard limit at |x| = 1, high shelf 3 kHz +2.2 dB slope
   0.7 after the clipper (the drum measured darker with the shelf first; +2.2 dB is the ear-approved
   value, per the drum spec's discrepancy note).
3. The dry passes untouched at unity; the slot's output is dry + wet × gain × trim.
4. Toggle off, settled: a plain copy, bit-exact, and the loop stops.
5. Level. The wet adds to the dry, as Echo's repeats do. `trimDb` in the tuning scales the whole
   output so the default setting lands 0 to +0.5 dB RMS on program, the way Echo's trim does.

## 8. Firmware

1. `registry.cpp`: the card entry (TAIL `Curve::Log`, 60..1000 ms; BLOOM percent), `applyKnob`,
   `applyToggle`.
2. `chain_params.h`, `chain_run.h` and `.cpp`: `cv::BloomParams bloom`, `cv::Bloom bloom` in
   `ChainEngines`, the `Engine::Bloom` cases, and the word key from the Input gate.
3. `costs.h`: `kBloom{5, 6, false}`, an estimate from the host bench (2026-11-01, word key open):
   Bloom 9.4 ns a sample against Echo's 8.1 and Gate's 7.8, so 1.17 and 1.21 times rows of 5 / 5
   and 4 / 5. The first estimate here, 40 cycles a sample and {1, 2}, did not hold.

## 9. Checks

1. `bloom_test`: on a scripted word (a 300 ms vowel-like tone, then silence), the first capture
   lands 40 to 50 ms after the key opens and on a zero crossing; re-captures land at the interval
   ±5 % while the key is open and stop when it closes; a slice 12 dB quieter is skipped; after the
   word the wet gain follows wet × e^(−t/τ), switches to the 80 ms fade at life 0.2 and ends at
   exactly 0; a word during the tail restarts the sequence; two renders are byte-identical.
2. Settled off is a plain copy; `knob_step_test` holds TAIL and BLOOM steps and the toggle.
3. `level_test`: the default setting lands 0 to +0.5 dB RMS on program.
4. `core_registry`, `core_chain_run`: the card, its knobs, the key handed from the Input gate.
5. Stress run with Bloom in slot 7 on the default and MOB + Chasm layouts.

## 10. Open for the owner's ear

1. Whether the loop plays under the word (as the drum's did under its hit) or only after the word
   ends. This spec plays it under the word; the tail-only version is one flag.
2. The 40 ms first capture and the 12 dB skip.

## 11. CPU

| Layout | Mean % | Worst % |
|---|---|---|
| default, Bloom in slot 7 | 68 | 77 |
| MOB and Chasm, Bloom in slot 7 | 75 | 87 |
| lines | 75 | 95 |

## 12. Built 2026-11-01 (v2.00.95)

1. The trim scales the whole output with the wet level, as Echo's does, so BLOOM 0 is bit-exact dry.
2. The 80 ms static fade is a time constant; a 300 ms tail ends about 2 s after the word.
3. Level at the default: +0.05 to +0.34 dB on the clips with either key.
4. The host bench put the card at 1.2 times Echo's cost; the row is {5, 6} (section 8).
5. Owner 2026-11-01, after auditioning in the emulator: "it's cool. it's not the effect i intended
   but we should keep it and refine it." The card stays; refinement waits for the owner's direction.

## 13. Refinement (owner 2026-11-01)

Owner, after the first audition: more time for the stutters, the lower knob named AMOUNT, then:
"rotate yes", the tail may finish under the next word ("ok"), "it should take its input from
post-octave at latest (assuming its activated). not mob. mob would just be muddle.", and the upper
knob named LINGER.

1. Knobs. Upper LINGER, 60 ms to 2 s, log (was TAIL, 60 ms to 1 s; same laws). Lower AMOUNT (was
   BLOOM).
2. Rotation. Bloom keeps the word's last four slices. Through the word the newest slice loops, as
   now. After the word ends the tail plays the kept slices in the order they were sung, each for
   one re-capture interval (±5 %), spliced over 5.8 ms, round and round until the tail closes. A
   word that left fewer slices rotates the ones it has.
3. Overlap. A new word no longer cuts the old tail. The old tail keeps its slices and its own fade
   and closes on its own, while the new word starts its own loop. At most two run at once: a third
   word fades the oldest tail out over 20 ms.
4. Source. Bloom cuts its slices from the chain after the pitch stage: the signal leaving the last
   slot that holds Autotune, Octave or Formant, before MOB, Unison, Echo and Distortion. A layout
   with none of those cuts after slot 1. The dry path is still slot 7's own input; the loops add to
   it. The word key stays the Input gate's.
5. Memory. Eight float slices of 80 ms, 122,880 B, in D1 SRAM (two tails of four).
6. Age limit (owner 2026-11-01: "bloom should keep replaying grains for a maximum of 2.5 seconds
   because if it gets stuck on a grain and nothing ever triggers a new grain, it will continue
   forever under new material"). No slice plays more than 2.5 s after it was cut. The way it could
   stick today: through a held word whose later slices sit 12 dB under the looping one, every
   re-capture is skipped and the loud slice loops for as long as the Input gate stays open.
   - During a word, a slice that reaches 2.5 s is replaced at the next re-capture even when the new
     slice is quieter; if no re-capture comes, it fades out over the 80 ms close.
   - In a tail, a slice that reaches 2.5 s leaves the rotation; with none left, the tail closes over
     80 ms.
   - So LINGER's top end rings for at most 2.5 s after the word's last slice was cut, short of the
     law's natural fade at 2 s.
7. AMOUNT sets loudness only (owner 2026-11-01, option A). Grains are a fixed 62.5 ms, 3,000 samples
   (owner: "62.5"), which falls exactly on the beat at 120 and 160 BPM (8 and 6 a beat) and within
   25 ms of it from 120 to 180 BPM; no one length fits every tempo. A fresh grain is cut every
   125 ms, two grains, with no jitter, and splices land on that grid, so each grain plays exactly
   twice. LINGER still caps a grain at 0.8 of its setting below 78 ms, never under 35 ms. The wet
   level runs from 0 at AMOUNT 0 to 0.70 at 100 % as 0.70 · AMOUNT^0.6, which puts 50 % at 0.46,
   the default loudness the owner auditioned. Eight slices of 3,000 samples are 96,000 B, which
   replaces item 5's figure.
8. Built in v2.00.98: TAIL to 2 s (the tail at 2 s ends 3.58 s after the word, today's law) and the
   lower knob named AMOUNT. Items 2 to 7 and the LINGER name are the next build.
9. Built in v2.00.99: section 13 items 1 to 7. As built:
   - The first loop sounds about 112 ms after a word starts (40 ms wait, up to 10 ms for a zero
     crossing, then one 62.5 ms slice); each slice plays twice, and splices sit on that grid.
   - After the word the playing slice finishes its turn, then the rotation runs oldest to newest.
     A capture under way at the word's end is dropped, so such a word rotates three slices.
   - A slice plays on only while its age plus one interval and the 80 ms close stays within 2.5 s;
     the longest play measured is 2.32 s, and a LINGER 2 s tail closes 2.22 s after the word.
   - While two loops sound, each plays 3 dB down, so the pair stays at one loop's power.
   - The 11 kHz low-pass runs as a slice is cut, not at playback.
   - Bloom's own word key, used without an Input gate, still listens to slot 7's input.
   - Level at the default, Input gate key: −0.01, −0.05, +0.54 dB; own key +0.30, −0.05, +0.27 dB.
   - Memory: `sizeof(Bloom)` 96,628 B; the firmware's D1 SRAM 292,560 B, 55.8 %.
   - CPU on the host: 1.3 times Echo with one word sounding, 2.0 times with two loops. Echo measured
     2.1 % on the board, so about 2.8 % and 4.2 %; the {5, 6} row covers both until a board bench.

## 14. CHATTER, the middle ground (owner 2026-11-01)

Owner, after hearing v2.00.99 against the first build: keep the clean source; bring back the
variable slice length; the tail repeats one slice; tail length as it is; some dirt back in, from
Bloom's own clipper (option B, not slot 7's input, which carries MOB and Echo); "this effect should
be called CHATTER". This section overrides sections 13 items 2, 3 and 7 where they differ.

1. Name. Display name `Chatter`, menu value `CHATTER`, emulator switch `CHATTER`. The code id
   stays `bloom` so saved layouts load; the engine keeps its file and class names.
2. Source. Unchanged from section 13 item 4: slices from after the pitch stage.
3. Slice length follows AMOUNT again: 80 ms at AMOUNT 0 to 45 ms at 100 %, linear, LINGER's 0.8 cap
   and the 35 ms floor kept. AMOUNT also sets the loudness, 0.70 · AMOUNT^0.6 as now, so the knob
   runs from quiet and long to loud and choppy. The 62.5 ms beat match holds only where the law
   passes 62.5 ms (AMOUNT 50 %).
4. Cut timing. Each slice plays exactly twice, then the next cut splices in at the slice boundary;
   the interval between cuts drifts ±3 % from a fixed-seed LCG, so renders stay byte-identical.
5. Tail. One slice repeats, the last good one, as in the first build; no rotation. The 2.5 s age
   limit (section 13 item 6) and LINGER's 60 ms to 2 s law stand.
6. Next word. A tail still sounding when a new word starts fades out over 150 ms while the new
   word's loop starts; the two never overlap longer than that. Each plays 3 dB down while both sound.
7. Dirt. The clipper drive through the word doubles, 1.4 to 2.8, and over the tail it follows
   2.8 + 2.0 · 4 · life · (1 − life), peaking at 4.8 mid-tail. `trimDb` holds the default (LINGER
   300 ms, AMOUNT 50 %) at 0 to +0.5 dB on the clips. The owner tunes the drive by ear afterwards.
8. Memory. Three slices of 80 ms (the loop, the capture, the fading tail), 11,520 samples, 46,080 B.

## 15. CHATTER as built (v2.01.02)

1. Drift. Each cut's length drifts ±3 % about AMOUNT's length, then LINGER's cap and the 35 ms floor
   apply. Each slice plays exactly twice and splices at its own end, so the interval drifts with it.
   A capped slice does not drift.
2. Tail (owner: "Palms open en en en en open en" should be "Palms open en en en en en en"). At the
   word's end the word's loop moves whole into the tail, after at most one waiting cut, so the tail
   repeats the newest slice. Nothing splices, rotates or swaps after that; the age limit only fades
   and closes it. A cut that lands after the voice stops is near silence and fails the 12 dB test, so
   the tail repeats the last cut that carries the voice. `bloom_test` `tail-on-clips` runs Adam's
   three clips through the Input gate at LINGER 60 ms / 300 ms / 2 s and gate Decay 20 ms / 2 s:
   42 tails, 0 slice changes after the word's end. A cut that straddles the stop can still loop a
   short gap, and a word held past about 2.3 s can still take a quieter cut to beat the age limit.
3. Next word. The old tail falls in a straight line over 150 ms, the pair 3 dB down while both sound
   (36 ms measured). A tail still fading when a third word comes yields in 20 ms.
4. Level. `trimDb` −5.62 scales the whole output, centring the three clips: +0.25, −0.03, +0.53 dB
   with the Input gate key, +0.23, −0.03, +0.13 dB on the engine's own key. The dry voice therefore
   sits about 3.7 dB down at the default and 5.6 dB down at AMOUNT 100 %, because drive 2.8 makes
   the slices hotter.
5. Cost. Host bench 11.7 ns a sample, 1.5 times Echo (v2.00.99 was 14.0). From Echo's board 2.1 %
   that is about 3.2 %; the {5, 6} row stands. D1 SRAM 242,648 B, 46.3 % (v2.00.99 292,560 B).
6. `tools/render.cpp` takes `bloomDriveWord`, `bloomDriveBell` and `bloomDrift` for tuning by ear.
   Renders `build/audition/chatter-{a,b,c}.wav`.

## 16. LINGER 20 ms to 3 s (owner 2026-11-01, option B)

The knob is named LENGTH from v2.01.04 (owner: "linger should be renamed to LENGTH"); the render
tool takes `--length`, with `--linger` and `--tail` as aliases. Earlier sections keep the old name.

Owner: "let's allow it to run from a minimum of 20ms to 3s." The 2.5 s age limit cut every tail
above about 1.5 s, so a longer LINGER only made the cutoff more abrupt. The owner picked B: the
limit stays while a word lasts, where a slice can stick, and a tail runs its whole fade, which
always ends. This section overrides section 13 item 6's tail rule and section 14 item 5's range.

1. Range. LINGER runs 20 ms to 3 s on the log curve. The default stays 300 ms.
2. Low end. τ = max(D, min(1.5·D, 300)) ms, without the 120 ms floor, and the static fade's time
   constant is min(80, D) ms. Both match today at D ≥ 80 ms, so the sound from 80 ms up does not
   change. At 20 ms τ is 30 ms: life reaches 0.2 at 48 ms and the 20 ms fade takes 91 ms more to
   the close, so the tail ends 139 ms after the word. LINGER's 0.8 cap on the slice and the 35 ms
   slice floor stand, so below 44 ms the slices sit at 35 ms.
3. Age limit in a word. Unchanged: a slice that reaches 2.5 s is replaced at the next re-capture
   even when quieter; with no re-capture it closes over 80 ms.
4. Tail. No age limit. The tail repeats its one slice until LINGER's law ends it, or a new word
   fades it over 150 ms. At LINGER 3 s it rings about 5 s.
5. Built (v2.01.03). Measured tail ends after the word's end at AMOUNT 50 %: 20 ms → 139 ms,
   40 ms → 278, 80 ms → 555, 300 ms → 845, 2 s → 3,576, 3 s → 5,170 (law 5,190). Renders at LINGER
   80, 300 and 1,000 ms are byte-identical to v2.01.02; at 2 s they part only past the old age-limit
   close. The knob's mid travel is now 245 ms (was 346). `bloom_test` covers the law at six
   settings, a click-free 20 ms close, 2 s and 3 s tails running past 2.5 s, and `tail-on-clips` at
   LINGER 20 / 60 / 300 / 2,000 / 3,000 ms (72 tails, 0 slice changes after the word's end).

## 17. Audit fix pass (owner 2026-11-01)

Three read-only audits at v2.01.03 (`build/audit/2026-11-01-chatter-audit-{efficiency,cleanup,
correctness}.md`). Owner: "we must address all of these", with B for the key blip and A for the
shelf.

1. Cut with a gap. A cut whose voice stops partway (more than one eighth of it after its last
   sample within 12 dB of its peak) or starts late (the same share before its first such sample)
   counts as quiet. While a loop plays it is skipped under the existing rule, so the tail repeats
   the last full slice. The first cut of a word, which has no loop to keep, re-arms once instead.
   The 2.5 s age limit still overrides the skip. Before: 37 to 52 % of tails over 15 % silence
   with no pitch card on; the audit's patch gave 0 of 120.
2. Key blip (option B). The tail stays where it is when the key opens. It fades over 150 ms only
   when the new word plays its first slice, so a breath or pop that never gives a slice leaves the
   tail alone. On a real word the old tail overlaps it 85 to 130 ms longer than before. Word and
   tail can now sound together, so the four voices stay; a full set of voices never overwrites a
   sounding one with a click.
3. Shelf (option A). The RBJ high shelf stays. Its coefficients come from `dsp.h`'s
   `Biquad::setShelf`, which gains a slope that leaves every other caller bit-exact.
4. CPU, sound unchanged byte for byte: the per-sample helpers inline, an idle path when nothing
   sounds, the trim gain stepped instead of recomputed, the voice loop bounded by the highest
   running voice, the pass window written into the slice as it is cut, the zero-crossing test only
   while a capture waits, the slice data last in the class, the own key scanned as integers, and
   `exp2Fast` / `log2Fast` for the block-rate laws (rounding under 1e-5).
5. Voice-major rewrite (efficiency finding 12) is a second pass after this one lands, held
   byte-identical to this pass's renders.
6. `kBloom` is set by `costs.h`'s own host-to-board method after the CPU work, and its comment
   names the bench. The stress run of v2.00.82 measured Echo at 2.1 % on the board, half its row,
   so the table stays conservative until the board re-bench.
7. Level. All six level rows (three clips, two key sources) sit inside 0 to +0.5 dB without the
   test's slack, or the reason they cannot is found and written here.
8. Names. Code uses the owner's words: `lengthMs` and `amount` for the knobs, `kLengthMinMs` /
   `kLengthMaxMs`, plain names for the end fade and the quick fade, "the Chatter card" in comments
   and test messages (the class, file, `Card::Bloom`, `Engine::Bloom` and the id stay `bloom`).
   The render tool takes `--amount` and `--length`; the old flags go.
9. Tuning. The 27 values nothing sets become constants. The render tool keeps six of its seven
   keys; `tailOnly` goes, because the loop plays under the word.
10. The key reaches `Bloom::process` as an argument, so the chain no longer copies the parameter
    block. One helper writes the pitch-stage source for both chain sites.
