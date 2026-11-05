# cubevox — Pitch stage split into per-slot cards

Date 2026-11-05. Owner picked option A for chain-order audit entries 4 and 5
(`build/audit/2026-11-05-chain-order-audit.md`). Autotune, Octave, Formant and Yawn each run in their
own slot on that slot's input, in slot order. Mob keeps its exemption (root `CLAUDE.md`, "Chain order").
Status: built 2026-11-05 (v2.01.59). Ringer (`RingMod`) was removed 2026-11-05. Code state read: main at 7b0bf92.

## Goal

1. Each of Autotune, Octave, Formant and Yawn runs at its own slot on the block the slot ahead hands it.
2. Octave behind Autotune shifts Autotune's output. Formant behind Octave shifts Octave's output.
3. A toggle fades only its own card's slot.
4. Fixes audit entry 4 (a to d), entry 5, the Yawn envelope flag (control-only item 2) and the side bug
   (Yawn behind a Deepthroat-first layout never sweeps).
5. Mob keeps building its copies from a ring of Autotune's input with Autotune's tracker and correction,
   as today in every menu layout.

## Audit picture on main today

1. Entry 1 (Deepthroat) is fixed. `Growl` keeps a 3,852-float ring of its own input in D1. It still reads
   three control values from `PitchFx` (`pitch().period`, `outputShiftSemis()`, `outputLatency()`).
2. Entry 3 (Chatter) is fixed. `runBloom` passes only `in`.
3. Octave is the granular shifter and reads no period. It takes Autotune's correction as an added
   interval, which the split removes.
4. At Formant MIX 100 % the stage skips Autotune's voice (`skipAutotune`). Yawn pins MIX at 1, so in every
   Yawn layout today Autotune's voice never renders and its correction reaches the output only as the
   Formant voice's pitch ratio.
5. Formant with Autotune off mixes its voice, 22.7 to 36.0 ms late, with the undelayed dry at MIX under
   100 %. With Autotune on the two line up. The split lines them up in every case.

## 1. Engines, rings and memory

### Engine per card

| Card | Engine (new) | Ring holds | Ring floats | Ring bytes | Tracker | Memory |
|---|---|---|---|---|---|---|
| Autotune | `AutotuneFx` | its own input | 2,176 | 8,704 | own, fine pass, 8,472 B | DTCM |
| Octave | `OctaveFx` | its own input | 3,920 | 15,680 | none | DTCM |
| Formant, Yawn | `FormantFx` (one engine, as Spring and Chasm share `Reverb`) | its own input | 3,852 | 15,408 | own, 8,472 B, runs only when not directly behind Autotune | D1 |
| Mob | `Crowd` | Autotune's input when Autotune sits ahead, else Mob's own input | 12,756 | 51,024 | Autotune's when ahead, else own, 8,472 B | D1 |
| Deepthroat | `Growl`, unchanged ring | its own input | 3,852 | 15,408 | Autotune's through the chain (section 2), else own, 8,472 B | D1 |

1. `Engine::Pitch` splits into `Engine::Autotune`, `Engine::Octave` and `Engine::Formant`. `PitchFx` goes.
2. `Group::Pitch` stays only as the protocol's group label (`protocol.cpp:586`). The chain stops reading it.
3. `PsolaVoice`, `AutotuneVoice` and `OctaveVoice` take the ring length as a template argument, as
   `FormantPsolaVoiceT` already does. `FormantFx` uses Growl's `FormantPsolaVoiceT<3852>`.
4. Each engine runs a call in `kBlock` chunks (write the chunk, push the tracker, render), so a ring sized
   for one block of calls holds any `n`.
5. Each engine with a ring has `rest()`, which zeroes 512 floats a block while its slot is stopped, and
   `reset()` clears the rest with `fill`.

### Ring lengths

| Ring | Deepest read | Plus a block and a pad | Length |
|---|---|---|---|
| Autotune | grain delay 1,040 + 1.5 × 686 + 1 tap = 2,070 | + 64 + 4 = 2,138; tracker replay needs 1,708 | 2,176 |
| Octave | 80 ms window 3,840 + 4 + 3 taps = 3,847 (`octave.h` static_assert) | + 64 + 4 = 3,915 | 3,920 |
| Formant | lag 1,040 + 686, a period past it, two periods of half-speed drift = 3,784 | + 64 + 4 | 3,852 (Growl's formula) |
| Mob | 225 ms lag + 12 ms scatter + a grain (`psola.h`) | already in | 12,756 |

### Memory before and after

Sizes are host `sizeof` (g++ 15), within a few bytes of the board. Confirm with `arm-none-eabi-nm -S` at build
step 6. DTCM is 130,816 B. D1 is 524,288 B.

| Region | Before | Moves | After |
|---|---|---|---|
| DTCM | 88,224 B, 67.4 % | − `PitchFx` 61,420; + `AutotuneFx` 18,132; + `OctaveFx` 16,048 | 60,984 B, 46.6 % |
| D1 | 186,647 B, 35.6 % | + Mob ring 51,024; + Mob tracker 8,472; + `FormantFx` 24,952; + Deepthroat tracker 8,472 | 279,567 B, 53.3 % |

1. Autotune and Octave sit in DTCM because the default layout runs them every block.
2. `FormantFx` sits in D1 beside `Growl`, which runs the same voice type from D1 and measured 5 / 7 on the box.
   If the stress run reads it slower than its row, it moves to DTCM (65.7 %).
3. Fallback if Mob reads slower with its ring in D1 than its measured 14 / 17: Mob's ring returns to DTCM
   and `AutotuneFx` and `OctaveFx` move to D1. DTCM is then 77,828 B, 59.5 %.

## 2. Trackers and who reads them

### Readers

| Reader | Today | After |
|---|---|---|
| Autotune | stage tracker on the first pitch-group slot's input | its own tracker on its own input, fine pass |
| Octave | none; adds Autotune's correction to its interval | none; drops the correction, which Autotune's output already carries |
| Formant, Yawn | stage tracker, Autotune's correction as pitch ratio, Autotune's grain delay | directly behind Autotune: Autotune's tracker. Anywhere else: its own tracker on its own input. Pitch ratio 1 in both. **Superseded by `2026-11-05-grain-group-design.md`: directly behind Autotune the card also cuts from Autotune's ring, at Autotune's pitch ratio and delay** |
| Mob (exempt) | stage ring, tracker and correction | Autotune ahead: Autotune's tracker and correction, and a ring Autotune's slot writes from Autotune's input. No Autotune ahead: Mob writes its own input into its ring and runs its own tracker |
| Deepthroat | stage period, `outputShiftSemis()`, `outputLatency()` | Autotune ahead: Autotune's tracker with the shift and latency the chain sums (below). No Autotune ahead: its own tracker on its own ring, shift 0, latency 0 |

### When a card reuses Autotune's tracker

1. "Directly behind" means the slots between hold Empty only.
2. Formant reuses only directly behind Autotune. Autotune's PSOLA lays its grains at the tracked period
   divided by 2^(correction / 12), so its output carries exactly that period, one pitch. Formant takes
   that period and nothing else from Autotune. Its grains and its base come from its own ring, so the rule
   holds. While Autotune's crossfade moves, the period is the one the larger share carries, as
   `octaveLeads()` picks today.
3. Formant anywhere else runs its own tracker. Octave at mixed MIX, Unison's detuned copy, Echo's repeats,
   a reverb tail each give the input two pitches at once. Formant plays at pitch
   ratio 1 and rebuilds its whole input on one period grid, so a period that fits only one of the pitches
   drags the other onto it. Its own tracker reads the mixture's common period (Octave −12 and the voice
   share the lower note's period), which carries both.
4. Deepthroat keeps today's reads. Its audio is its own ring, and only the period, shift and latency are
   controls. The values come from the separate engines instead of `PitchFx`. The cards Deepthroat treats as
   pitch-neutral (Unison, Echo, Mob, Formant) are the ones it treats so today.
5. Mob is exempt. With Autotune ahead it reads the same samples, tracker and correction as today.

### What the chain hands Deepthroat

`runChain` keeps one small struct per block (code name `UpstreamPitch`: the tracker's result, a shift in
semitones and a latency in samples). It replaces `PitchFx::outputShiftSemis()` and `outputLatency()`.

| Slot holds | Sets |
|---|---|
| Autotune | result = its tracker; shift = its correction (scaled by its fade as `correctionSemis()` is); latency = its corrected voice's delay weighted by its crossfade's power |
| Octave | when MIX × fade ≥ 0.5: shift += the glided SEMITONES. Latency += its wet power share × its window latency |
| Formant, Yawn | latency += its lag, weighted by its slot fade |
| any other card | unchanged |

No Autotune ahead leaves the struct empty, and Deepthroat then uses its own tracker.

### Tracker gating

1. Autotune's tracker runs while Autotune is on or fading, or while a reader behind it is on: Formant or
   Yawn directly behind, Mob, or Deepthroat.
2. Autotune's slot runs its engine with its toggle off while a reader needs the tracker, as the Input gate
   keeps its detector. Settled off, its output is its input bit for bit.
3. The fine pass runs while Autotune is on or fading, or while Formant or Yawn reuses the tracker.
4. `readOutOfRange` is on while Mob or a reusing Formant or Yawn is on.
5. Autotune's slot writes Mob's ring every block it runs while Mob sits behind it. Both stopped, Mob's
   ring rests and clears, as the stage ring does today.

## 3. The shared grid

1. Today Autotune and Formant read one ring at one delay and add in one mix, so a voice that starts beside
   a running one adopts its grain grid (`adoptGrid`) to keep the sum from combing.
2. After the split the two voices never add. Formant's base is its own input read from its own ring at
   the voice's lag, and its voice is cut from that same ring. At SHIFT 0 the voice rebuilds the base, so
   the two line up with no grid to share.
3. The base read uses Growl's dry read (`readRing` at the lag, slewed so a new period slides the lag).
4. Removed: `VoiceGrid`, every `grid()` and `adoptGrid()`, `formantStart_`, `autotuneIdle_`, `skipAutotune`.
5. `kFormantCoherence` was measured with Autotune on. It is re-measured with the aligned base on Adam's
   three clips. The level gain (`levelGainAt`) stays.

## 4. Latency

Range is 1 kHz to 70 Hz. Adam's G2 (98 Hz, period 490 samples) in brackets.

| Path | Before | After |
|---|---|---|
| Autotune, corrected voice | 22.7 to 36.0 ms (31.9) | same |
| Default layout, the tuned voice (Octave MIX 10 %) | 22.7 to 36.0 ms (31.9) | same |
| Octave wet behind its own input | 20.1 ms | 20.1 ms |
| Octave wet behind the mic, Autotune on | 20.1 ms, 2.6 to 15.9 ms ahead of the tuned voice | 42.8 to 56.1 ms (52.0), 20.1 ms behind the tuned voice |
| Formant or Yawn behind Autotune (menu layouts) | 22.7 to 36.0 ms (31.9) | 24.3 to 57.5 ms (47.3). **Superseded by `2026-11-05-grain-group-design.md`: 22.7 to 36.0 ms (31.9), Autotune's own** |
| Formant or Yawn on its own tracker | voice 22.7 to 36.0 ms, base 0 | voice and base 22.7 to 36.0 ms |

1. Formant behind Autotune adds its lag floor, 1.5 periods and 4 samples: 1.6 to 21.5 ms (15.4 ms at G2).
   PSOLA needs that lookahead. Today Formant gets it free inside Autotune's delay. (Superseded: the grain
   group removes this addition.)
2. Formant on its own tracker reads at the grain delay plus a period, where the tracker's frame stands, as
   Autotune does.

## 5. CPU

### Method

Host bench of 2026-11-05 (`/tmp/cubevox-pitchsplit-a880/bench.cpp`): Adam's three clips, 64-sample blocks,
g++ 15 -O2, minimum of 9 interleaved passes, two runs within 0.02 us. The split is approximated by two
`PitchFx` calls, an Autotune-only stage and then a second stage on its output. Percent is at 50.4k board
cycles per host us (7.875 % per us). The pitch code runs nearer 27k on the board (`kTracker`), so these
read high.

| Case | Host us a block | % at 50.4k |
|---|---|---|
| Autotune alone, today | 1.437 | 11.3 |
| Autotune + Octave 10 %, today | 1.683 to 1.690 | 13.3 |
| Autotune, then Octave 10 % (split) | 1.687 to 1.688 | 13.3 |
| Octave 100 % alone, today | 0.251 | 2.0 |
| Autotune + Formant 100 %, today (Autotune's voice skipped) | 1.447 to 1.453 | 11.4 |
| Autotune, then Formant 100 % on its own tracker (split) | 2.660 to 2.672 | 21.0 |
| Autotune, then Formant 50 % on its own tracker (split) | 2.729 to 2.742 | 21.5 |
| Tracker alone, fine pass | 0.993 to 0.998 | 7.8 |
| Tracker alone, coarse | 0.913 to 0.924 | 7.2 |

1. Default layout: the split costs the same, within 0.01 us.
2. Formant reusing Autotune's tracker: about 2.666 − 0.996 = 1.67 us, 0.22 us (1.7 %) over today. That is
   Autotune's voice, which today sits out at MIX 100 %. `kAutotune` already charges it.
3. Formant on its own tracker adds about 1.0 us, which `kTracker` holds at 4 / 5.

### Rows

| Row | Before | After |
|---|---|---|
| `kAutotune` | 3 / 5, carries the stage's mix | 3 / 5, carries its own crossfade |
| `kOctave` | 2 / 2, voice only | 2 / 2, the whole engine (0.251 us, 2.0 %) |
| `kFormant`, `kFormantFollower` | 3 / 4, 4 / 5 | same; base read and mix were inside the stage |
| `kPitchStage` | 4 / 4 when Octave or Formant sits without Autotune | removed; each engine carries its own mix |
| `kTracker` | 4 / 5 once when Autotune, Mob, Formant, Yawn or Deepthroat is placed | 4 / 5 once for Autotune's; once more for each of Formant or Yawn not directly behind Autotune, Mob with no Autotune ahead, Deepthroat with no Autotune ahead |

### Layouts

| Layout | Before (mean / worst) | After |
|---|---|---|
| Default | 33 / 47 | 33 / 47 |
| Worst menu layout (Yawn, Mob, Deepthroat, Lo-fi or Gate, Spring) | 48 / 67 | 48 / 67 |
| Formant alone, no Autotune (set_layout) | rows + `kTracker` + `kPitchStage` | rows + `kTracker`; 4 / 4 less |

Every menu layout keeps Formant and Yawn directly behind Autotune and Mob and Deepthroat behind Autotune, so
no menu layout pays a second tracker. A set_layout layout that pays several stays under `layoutValid`'s
75 % / 95 % lines or the box will not load it, as today.

## 6. Tests

| Test | Change | Expected |
|---|---|---|
| `pitch_onset_test` | none; it drives the tracker through `RingedTracker` | passes unchanged |
| `pitch_spikes_test` | one row per ring (Autotune, Octave, Formant, Mob) in place of the `PitchFx` row | every switch-on block below its line |
| `parity_test` | `ChainParams::pitchFx` keeps its three card params; `crowdOn` and `growlOn` go | passes; no mapping moves |
| `crowd_test` pinned hashes | ring and tracker plumbed through the new path | hold: same samples, tracker and correction |
| `growl_test` `kFullMixHash` | nothing ahead, so Deepthroat runs its own tracker on the same input | holds |
| `growl_test` accessor checks | measure `UpstreamPitch` against Octave and Formant engines | re-pinned figures; the pulse-landing check passes |
| `level_test` | rows build each engine alone | Autotune, Octave, Formant MIX 1 and Yawn rows hold (Formant within −80 dB, ring length moves the grains' rounding); Formant MIX 0.5 moves with the aligned base and comes back inside 0 to +0.5 dB after the `kFormantCoherence` re-fit |
| `core_chain_run` | shared-stage checks replaced | new checks below |
| `core_layout_fade` | `enginesToReset` bits per new engine | rewritten rows |
| `core_layout` | `layoutCost` tracker terms | rows for each reuse and own-tracker case |
| `autotune_test`, `octave_test`, `formant_test`, `formant_follower_test`, `knob_step_test`, `fastmath_test`, `core_block_tail` | port from `PitchFx` to the new engines | curvature and level gates hold |

### New `core_chain_run` checks

1. Each card's slot output equals its engine run by hand on that slot's input, for audit layouts 4a
   (Autotune 2, Distortion 3, Octave 4), 4b (Mob 2, Octave 3), 4c (Octave 2, Autotune 3) and 4d.
2. A toggle fades only its own slot; the other pitch cards' slots stay settled.
3. Yawn sweeps behind a Deepthroat-first layout.
4. Autotune's tracker stays live with Autotune's toggle off while a reader behind is on, and its slot passes
   bit for bit.
5. Formant reuses Autotune's tracker directly behind it and runs its own elsewhere.

### Outputs that move, and why that is expected

1. Any run with Autotune and Octave both on: Octave now shifts the tuned voice.
2. Any run with Autotune and Formant or Yawn both on: Formant cuts the tuned voice, adds its lag, and
   Autotune's voice runs at Formant MIX 100 %.
3. Formant at MIX under 100 % with Autotune off: the base is delayed to the voice.
4. No pinned hash moves.

## 7. Build order

Each step ends with `cmake --build build -j && ctest --test-dir build` at 100 % passed.

1. Template the ring length in `PsolaVoice`, `AutotuneVoice` and `OctaveVoice`, defaulting to
   `kVoiceRingLen`. No output moves.
2. Add `AutotuneFx`, `OctaveFx` and `FormantFx` (with the follower call and the aligned base) beside
   `PitchFx`, unused by the chain, each with a test against `PitchFx` running that card alone. Octave
   matches bit for bit (Q18 positions); Autotune and Formant match within −80 dB.
3. Move Mob's ring and its fallback tracker into `Crowd` (D1), with a write call the chain makes. `PitchFx`
   still serves the rest. Crowd hashes hold.
4. Switch the chain: one engine per slot, `UpstreamPitch`, Autotune's tracker gating, the follower at the
   Formant engine's slot, `layoutCost`, board placement (section 1), `audio_block.cpp`, the emulator and
   the render tool. `core_chain_run` and `core_layout_fade` rewritten.
5. Delete `PitchFx`, `kPitchStage`, `crowdOn`, `growlOn`, the grid sharing and Octave's correction input.
   Re-fit `kFormantCoherence`. Update `engine/CLAUDE.md` (DTCM line, the Growl and ring paragraphs), the
   autotune, octave-c, formant, yawn and crowd specs, and the INDEX rows.
6. Board: build, `arm-none-eabi-nm -S` for DTCM under 70 %, `stress.py` reading Octave and Formant in their
   own slots, a stress run of the default, crowd and formant layouts, and the rows it measures.

## 8. Risks and owner questions

### Risks

1. Mob's ring in D1 reads through the cache. The stress run decides; the fallback is in section 1.
2. Formant reusing Autotune's tracker gets a period that describes Autotune's newest output while it cuts
   1.5 periods back. Behind hard tune the period holds still, so the gap shows only at note changes.
3. Formant at MIX under 100 % and Autotune off loses today's 23 to 36 ms doubling, because the base now
   lines up with the voice.

### Ear checks after the build

1. Formant and Yawn behind Autotune gain 1.5 periods of delay, 15 ms on Adam's G2: 47 ms from the mic in
   place of 32 ms. They take it, because Mob is the only exemption from the chain-order rule (owner
   2026-11-05).
2. Octave's wet now follows the tuned voice, carries Autotune's grain texture and sits 20 ms behind it
   (today it sits 3 to 16 ms ahead). An ear check at MIX 10 % and 100 % in the emulator settles it.
   Passed by the owner in the emulator, 2026-11-05, at v2.01.66.
