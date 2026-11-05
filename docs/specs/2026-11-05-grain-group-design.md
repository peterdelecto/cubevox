# cubevox — One grain pass for Autotune and the card behind it

Owner 2026-11-05: "15ms is a long time" ... "let's fix it properly". Follows `2026-11-05-pitch-split-design.md`.

## Problem

1. Autotune, Formant and Apraxia each cut grains two pitch cycles long, centred on one cycle. A grain cannot
   be cut until the cycle after its centre has arrived, so each of these cards trails its input by about
   1.5 cycles (`formant_detail::kFloorPeriods`), 15 ms on Adam's G2 and longer on lower notes.
2. Since the split, Formant and Apraxia behind Autotune cut their grains from Autotune's output, so their
   wait sits on top of Autotune's 23 to 36 ms. The voice reaches the output 47 ms behind the mic on G2
   where the shared stage gave 32 ms.
3. Octave is not part of this. It is the asynchronous granular shifter the owner chose on 2026-10-01
   (`2026-10-01-octave-c-design.md`, "formants move with pitch"). Its 20 ms is its own window, and it sat
   20 ms behind the raw voice before the split as it sits 20 ms behind Autotune's output now.

## Rule

Grain cards that sit next to each other share one grain pass. Retuning a grain and stretching a grain are
two changes to the same slice of the voice, and doing both on one cut gives what doing them one after the
other gives. The group waits once, for Autotune's grain delay, however many of its cards are on. A card
between two grain cards breaks the group, and only then do the waits stack. The chain-order rule holds:
no card sits between the two for the shift to skip, and Formant's and Apraxia's output is still the
formant-shifted version of what Autotune hands them.

## Behaviour

| Layout | Formant or Apraxia cuts from | Its voice lands | Its tracker |
|---|---|---|---|
| Directly behind Autotune (Empty between allowed), Autotune on or fading | Autotune's ring, on Autotune's grain grid: marks at the tracked period, laid at Autotune's corrected period, played at Formant's rate | with Autotune's voice, at Autotune's latency, 22.7 to 36.0 ms | none of its own; Autotune's |
| Directly behind Autotune, Autotune settled off | its own ring, its own grid | 1.5 cycles behind its input, about 15 ms on G2 | its own (as the split has it) |
| Anywhere else | its own ring, its own grid | 1.5 cycles behind its input | its own |

1. Formant's base is its input as it arrives when grouped (no lag read), because Autotune's output and the
   grouped voice already line up. Alone, the base is the lagged read the split built.
2. During Autotune's own crossfade (toggle, or the fade a reader starts), Autotune's output is a mix of its
   dry and its tuned voice, so the grouped Formant voice is the same mix of two renders on Autotune's grid:
   rate `r` at pitch ratio 1 (the dry share) and rate `r` at Autotune's ratio (the tuned share), weighted by
   Autotune's gains. Settled on, one render.
3. The group forms and dissolves on Autotune's toggle, and that change must make no click and bend no
   pitch. Formant's voice moves from Autotune's latency to its own 1.5 cycles (or back) over Autotune's
   crossfade, as a crossfade between the grouped render and the alone render, never a jump in the read
   position. The builder measures a 440 Hz tone and Adam's clips through the toggle both ways: no step
   above the toggle fade's own, and the pitch within 5 cents throughout.
4. Formant's own ring keeps filling while grouped, so the alone voice can start at the dissolve.
5. Apraxia is Formant with the follower. The follower keeps reading its own input's level, as the split
   built. Grouped, its sweep drives the rate of the grouped render.
6. Autotune settled off and nothing else needing it: its slot passes its input bit for bit, as today.
7. Octave, Mob and Deepthroat are unchanged. `UpstreamPitch` adds Formant's lag only when Formant runs
   alone; grouped, it adds 0.

## What moves

| Measure | Split (v2.01.59) | After |
|---|---|---|
| Formant or Apraxia behind Autotune on, voice behind the mic, G2 | 47 ms (36.0 + 1.5 cycles) | 22.7 to 36.0 ms |
| Formant or Apraxia with Autotune off | 15 ms | 15 ms |
| Autotune alone | 22.7 to 36.0 ms | unchanged |
| CPU, Formant behind Autotune on | Autotune voice + Formant voice + Formant base read | Autotune voice + one grouped render (two during Autotune's crossfade); no base read, no own tracker. Host bench before and after by the costs.h method; `kFormant` / `kFormantFollower` rows re-stated |
| DTCM, D1 | 36.2 %, 53.3 % | about the same; the grouped render reads Autotune's ring, Formant's ring stays |

## Tests

1. `formant_test`: a new latency check. A pitch step through Autotune on, Formant behind at SHIFT +3,
   MIX 100 %: the shifted voice arrives within one block of Autotune's own voice, not 1.5 cycles later.
   The same with an Empty slot between. The same with Echo between: 1.5 cycles later (alone).
2. `formant_test`: toggle Autotune off and on under Formant: largest step at the output no more than the
   toggle fade's own, pitch within 5 cents, measured on the 440 Hz tone.
3. `level_test`: Formant and Apraxia rows stay inside 0 to +0.5 dB at every setting the test holds.
4. `core_chain_run`: the group forms with Empty between, not with a card between; `UpstreamPitch`
   latency for Deepthroat behind a grouped Formant equals Autotune's alone.
5. `autotune_test` "toggle level, Formant −3 st at MIX 50 %" (re-pinned in the split at 6.44 dB dip): with
   the group, the dip is expected to fall back toward the pre-split value; re-pin with the measured number
   and say why.
6. Outputs with Autotune plus Formant or Apraxia change by design. Nothing else moves; the split's other
   hashes hold.

## Build order

1. `AutotuneFx` exposes what a grouped render needs: its ring, its grid (marks and the corrected period for
   the block), its gains, and a render of one extra voice at a given rate on that grid, so the grain cut
   stays in one place (`PsolaVoiceT`, `FormantPsolaVoiceT`).
2. `FormantFx` gains the grouped mode: `process(in, out, n, p, up)` takes the group from the chain
   (`UpstreamPitch` carries whether Autotune is directly ahead and live, as it carries the tracker now),
   renders on Autotune's grid when grouped, and keeps the alone path as built.
3. The transition (behaviour 3), with its test.
4. `chain_run`: `runFormant` hands the group; `handOnFormant` adds lag 0 when grouped. `layoutCost`
   unchanged in shape.
5. Docs: this spec's INDEX row; the split spec's latency table and section 2 row for Formant marked
   superseded by this; `engine/CLAUDE.md` one paragraph.

## Ear checks after the build

1. Formant +3 st at MIX 100 % behind Autotune: the voice should sit where Autotune's does.
2. Apraxia behind Autotune on a held note.
3. Autotune toggled off and on while Formant is on and singing.
