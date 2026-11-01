# Bloom card: each word restates itself as it dies

Owner 2026-11-01: "let's also adapt the 'bloom' effect from drumsynth as a possible effect for slot
7", with DrumSynthV3's BLOOM technical spec (D2, R10, code at a4149ed) pasted in. Offered for slot 7
in place of Gate, through the slot 7 row (`2026-11-01-slot-7-choices-design.md`). Spec only, not
built.

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
