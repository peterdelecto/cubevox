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
| Directly behind Autotune (Empty between allowed), Autotune on or fading | Autotune's input (held in its own ring), on Autotune's grain grid: marks at the tracked period, laid at Autotune's corrected period, played at Formant's rate | with Autotune's voice, at Autotune's latency, 22.7 to 36.0 ms | none of its own; Autotune's |
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
| DTCM, D1 | 36.2 %, 53.3 % | 36.2 %, 53.4 % (Formant's ring holds Autotune's input; Autotune's ring is unchanged) |

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

All three passed by the owner in the emulator, 2026-11-05, at v2.01.66.

## As built (2026-11-05)

Steps 1 to 5 are built. The build differs from the design in these places.

1. The Formant card alone lags its input by the grain delay plus a period, 31.9 ms on G2, not 1.5 cycles. The
   lag is the tracker's alignment (`kGrainDelay + period`) less the delay already on the audio, held over the
   1.5-cycle floor. With Autotune off nothing is on the audio, so the lag is the full alignment. Only with
   Autotune on did the floor bind, and that is where the extra 15 ms came from. The alone voice and the grouped
   ratio 1 voice therefore land at one delay, and Autotune's toggle moves no delay.
2. So there is no alone voice to crossfade to. Directly behind a running Autotune the card cuts from Autotune's
   input in every state: `voice_` at ratio 1 for Autotune's input and `tuned_` at Autotune's ratio for its
   corrected voice, each running only while its share is up. They start on one grid (the corrected voice takes
   the running ratio 1 voice's marks at its enable edge, the one that starts takes the other's), mixed by
   Autotune's linear fade. Settled off, `voice_` alone runs and equals the card on its own ring and own tracker
   bit for bit (Adam's three clips, SHIFT -7 to +4, MIX 50 and 100 %). The card's own ring holds the audio it
   receives, and runs the voice when a card sits between or the call is longer than kBlock.
3. The grouped render cannot read Autotune's 2,176-float ring. A grain that plays at half speed drifts two periods
   back, and that ring cannot hold the read on a low voice. Growing it to 3,852 floats cost 5.1 points of DTCM
   (41.3 %) and moved Autotune's own output at 1e-6, so it stays at 2,176. The Autotune slot instead hands its
   input to the card (`FormantFx::feed`), and the card writes it into its own 3,852-float ring in D1. Autotune
   alone is bit for bit what it was. DTCM is 47,328 B, 36.2 % (32 B more than before, the feed record); D1 is
   279,936 B, 53.4 % (672 B more, the second voice and its buffer).
4. The corrected share carries Autotune's trim and a group trim (`kFormantGroupTrimDb`, -0.3 dB, ramped in over the
   first semitone of SHIFT), since one cut from Autotune's input reads about 0.3 dB over the two cuts it replaces.
   `level_test` holds the card behind Autotune at every SHIFT, MIX 100 %: the mean of Adam's clips reads +0.10 to
   +0.38 dB over Autotune's output.
5. The base is the card's input with Autotune's input share moved to the voice's delay, read from the card's ring;
   settled on it is the input itself, settled off it is the ring read the card makes on its own.
6. `UpstreamPitch` carries the Autotune engine and the slot's engine gain at the block's edges, so the shares
   follow the slot's own fade as well as Autotune's. The latency the card hands on is its lag less Autotune's own
   latency, so Deepthroat behind a grouped Formant reads Autotune's alone.

Latency, where a pitch step lands (a sine stepping from G2 to A2, with a line fitted through the output's zero
crossings before the step and another after it; every figure carries about 2 ms of the fit, so Autotune alone reads
34.0 ms against 31.9 ms nominal):

| Path | Split | After |
|---|---|---|
| Formant or Apraxia behind Autotune on, G2 | 46.7 ms | 33.7 ms |
| Formant or Apraxia behind Autotune on, 196 Hz | 32.6 ms | 25.4 ms |
| Formant with Autotune off, G2 | 32.4 ms | 32.4 ms |
| Autotune alone, G2 | 34.0 ms | 34.0 ms |

Toggle (behaviour 3), Autotune on and off under Formant, 440 Hz sine, SHIFT -3 and 0, MIX 100 %: the largest step
is x1.000 of the settled outputs' and the pitch strays 0.01 to 0.03 cents (the split: x1.015 and 172 cents). On
Adam's clips (SHIFT -3 and 0, MIX 100 %, a toggle on and off each second) the largest step is x0.93 to x1.02 of the
settled outputs' at the voiced toggles, and the tracker reads the two-pitch mix within 12 to 35 cents of the band
the settled voices span, where Autotune's own toggle reads 9 to 48 and Formant's 5 to 53.

CPU, host bench of 2026-11-05 (Adam's clips, 64-sample blocks, -3 st, Apple clang -O2, minimum of 9 passes, us a
block, % at 50.4k board cycles per host us): default layout 1.378 us (10.9 %) before, 1.356 (10.7 %) after; Autotune
2 and Formant 3 at MIX 100 % 1.579 (12.4 %) before, 1.653 (13.0 %) after; at MIX 50 % 1.775 (14.0 %) before, 1.690
(13.3 %) after; Autotune off with Formant 3 at MIX 100 % 1.102 before, 1.125 after. The card's own share is 0.43 us
before and 0.48 us after at MIX 100 % (3.4 % and 3.8 %) and 0.62 before, 0.51 after at MIX 50 %. With Autotune
toggled every 300 ms the card's worst 1 % of blocks add 1.9 us (15 %) before and 1.9 us after, the blocks of the
fade and the restart. `kFormant` is 4 / 7 and `kFormantFollower` 5 / 8.

## Fix pass (2026-11-10)

1. Switch-on from rest. A stopped Formant slot rests and the next call clears the card's ring, while Autotune's
   tracker stays warm. The card used to come on at once and read the cleared ring, so Formant or Apraxia behind
   a running Autotune played 17 to 24 ms of exact zeros (MIX 100 %) or dipped 4 to 20 dB (MIX 50 %). The card now
   stays off until its ring holds its lag, two periods and the chunk since the clear (`InputRing::filled()`),
   passing its input meanwhile, then fades in over the 20 ms one-pole. No block of exact zeros and no dip of
   0.5 dB in the 40 ms after the toggle (`core_chain_run` `checkPitchCardSwitchOnFromRest`).
2. Formant at MIX 0 behind Autotune passes its input and no longer keeps Autotune's tracker and fine pass running
   (`FormantFx::needsTracker`). Its slot still feeds the card Autotune's input, so MIX leaving 0 finds the ring
   full. Host bench, default layout with Formant in slot 3, Autotune off, MIX 0: 4.76 us a block before,
   4.09 after.
3. A card that steps aside for a range flag and returns starts its voices clean when its fade leaves 0, as the
   toggle's edge does. It used to fade back in on grains frozen before the dropout, reading up to 3,840 samples
   back where the lag is about 1,100 (`formant_test` "range return").
4. As built item 2's settled-off claim is pinned: `pitch_split_test` compares the grouped card behind a
   settled-off Autotune with the card alone on Adam's three clips, SHIFT -7, 0 and +4, MIX 50 and 100 %, with `==`.
5. The card's own fade from its input to its delayed output still dips on a steady tone where the two partly
   cancel: up to 9.2 dB at 98 Hz, SHIFT +7, MIX 100 %, 60 to 80 ms after the toggle, with Autotune off or
   absent (9.3 dB before the pass). Formant behind a running Autotune reads 6.9 dB there.
6. Host cost, default layout (Adam's clips, all slots on, Apple clang -O2, minimum of 9 passes over several
   interleaved runs, whole chain): 5.36 to 5.46 us a block before, 5.39 to 5.45 after at knobs 0.5; with
   Formant in slot 3 at MIX 100 %, 5.70 to 5.79 before, 5.73 to 5.81 after. The gate adds no cost a host bench
   can see.
