# Scope

The voice screen shows the chain's output as an oscilloscope trace in place of the scrolling ribbon
(owner 2026-11-06). In silence it is a thin horizontal line. The trace reads the block that leaves the
last effect slot, before POLISH COMP, the back half of VOX EQ, output trim and the soft ceiling, so it
shows what the effects made and not what the polish pass did to it. The input meter box at the left
edge stays as it is.

Supersedes the Voice section of `2026-10-24-oled-gui-design.md` items 1 (the tap moves), 2 (the scale's
top and floor rules stay; the amplitude map goes), 4, 5 and 7, its Burn-in item 2 (the rim dither), the
Timing row "the ribbon has none", the Modules rows that name the ribbon, and Tests items 2 and 9. Items
3, 6, 8 and 9 of Voice stand.

UX audit 2026-11-06 (`build/audit/scope-ux-audit.md`, 7 findings, 3 holds) is folded in below.

## 1. Adam's walk

1. Adam switches the box on. After the splash he sees the empty meter box at the left and one 1 px line
   across the middle of the screen. Nothing moves.
2. He sings a note. The line becomes a waveform that holds still, about two to nine cycles across
   depending on the note, and its loudest peaks reach near the top and bottom of the screen. The meter
   box fills with his input.
3. He changes the note. The trace shows both notes for one frame, then holds still on the new shape.
4. He sings an "s" or breathes into the mic. The trace is a band of noise with no fixed shape, as a scope
   shows noise. A note with breath on it keeps its shape; only the edges shimmer.
5. He stops. The trace shrinks as the sound decays, and when the output falls under the floor the line
   comes back.
6. He turns a knob. The readout shows, and when it dissolves back the scope is live at once. There is no
   history to catch up on. The same when the menu closes.
7. He flips an effect toggle. The trace changes shape as the card's sound comes in, over the slot fade.
8. He flips MUTE or BYPASS. The MUTED or BYPASS screen hides the trace; the chain keeps running under
   MUTE, so the trace is live the moment the voice screen returns.
9. He swaps a card from the menu. The output fades out over 50 ms and the trace shrinks with it, the
   line shows while the chain is silent for the swap, then the trace grows back with the fade-in.
10. He shouts into the mic. The scale rises at once, so the shout fills the height and nothing
    flattens; the frames after it draw at 70 % and up while the scale settles.
11. Goes wrong, room noise. A synth idling on the input or a noisy room draws the line, not a band of
    hash, because the floor rule hides steady noise (owner 2026-10-30, the Korg Volca).
12. Goes wrong, hum. A hum rides the trace up and down and could pull the trigger. None is expected at
    the slot 8 output; the input gate and the cards sit ahead of it.
13. Goes wrong, a late block or a NaN. Every audio block writes one block into the ring: the chain output
    when the chain ran, zeros when the block was silenced or dry. The trace draws that silence as the
    line for the 8 columns it covers, and time never splices.

## 2. The picture

| Thing | Value |
|---|---|
| Scope area | columns 24 to 126, rows 10 to 54; horizon row 32, half-height 22 px |
| Blank | column 127, columns 21 to 23, as the ribbon left them |
| Time per column | 8 samples, 167 us |
| Frame | 103 columns, 824 samples, 17.2 ms |
| Columns a cycle | 6000 / f: G2 (98 Hz) 61, D3 41, A3 27, C4 23, A4 14, C5 (523 Hz) 11.5 |
| Cycles across | G2 1.7, A4 7.6, C5 9.0; Octave -12 on G2 (49 Hz) 0.84 |
| Trace | 1 px wide, solid, no rim dither |
| Silence | the horizon row alone, columns 24 to 126 |
| Refresh | every UI tick, 40 ms, only while the voice screen shows |

1. The trace is drawn column by column. A column lights the rows from the lowest to the highest of its
   8 samples, and that run is stretched to meet the last sample of the column before it, so the trace is
   one unbroken line even where the wave moves more than a row in one sample. The first column, 24,
   uses its own first sample as the one before it.
2. A sample at the top of the scale lands on row 10 and one at the bottom on row 54. Samples past the
   scale clip to those rows.
3. In silence every column is the one pixel on row 32.
4. The frame is drawn fresh every tick from the newest audio. Nothing scrolls.
5. The burn-in walk and the two-frame dissolve are unchanged. With the burn-in walk at +1 the trace
   reaches column 127 and row 55, as the ribbon did. The DEBUG load figure sits on rows 56 and under and
   never meets the trace.

## 3. Scale

Revised 2026-11-07 (owner: "the vertical scale should re-scale itself appropriately"). The first build
set the height against the loudest recent tick's RMS, which drew an echo tail or a quiet passage a few
rows tall. The scale now fits each frame.

1. The frame's own peak, the largest sample magnitude in its 824 samples, sets the scale, so the
   trace's loudest excursion lands on row 10 or 54 whatever the level. Loud and quiet draw the same
   size; the meter box beside the trace shows the level.
2. The scale falls, not jumps, between frames: the scale is the larger of this frame's peak and the
   last scale less 3 dB (75 dB a second at 25 frames). A quiet frame right after a loud one draws at
   70 % or more, and an echo tail 12 dB under the voice fills the height within four frames. The
   scale rises at once.
3. The floor rule is kept whole, from `level::OutputMeter`: a tick whose RMS is under the floor plus
   6 dB is silence, and the screen draws the line. The floor follows the quietest recent tick, rises
   3 dB a second, and never comes closer than 12 dB to the window top, so the loudest recent tick
   always draws. This is what keeps room noise and an idling synth on the line (owner 2026-10-30).
4. `OutputMeter` keeps its window top for the floor rule; the trace no longer reads it.
5. The tick RMS that drives the floor is the energy of the blocks the chain wrote into the ring during
   the tick, so the floor and the trace read one signal.

## 4. Trigger

1. The frame's left edge sits at a point that recurs once a cycle, so a steady note draws in the same
   place every tick. A plain first-rising-crossing rule fails on a vowel, which crosses zero several
   times a cycle and would flicker between alignments at about 12 Hz.
2. The search span is the samples from 824 back to 1924 back from the newest. 1100 samples is one period
   at 44 Hz, so the span holds at least one whole cycle of anything the chain can sing down to Octave
   -12 on G2.
3. In the span, find the sample with the largest value. Step back from it to the nearest rising crossing
   with hysteresis: the signal must have been under -5 % of that largest sample before it crosses up through
   zero. The frame starts at that crossing. A vowel has one strongest glottal peak a cycle, so the frame
   starts on the same feature each tick; when two cycles in the span have near-equal peaks the frame may
   shift by one whole period, which draws the same picture.
4. No crossing before the peak in the span, the frame is the newest 824 samples, free-running, which is
   what noise gives.
5. The trace lags the ear by 17 to 40 ms, about one frame.

## 5. The copy of the chain output

1. `BlockFlow::run` writes one block into the ring for every audio block: the chain output as it leaves
   `runChain`, after the NaN guard and before POLISH COMP, when the chain ran; zeros when the block was
   silenced (NaN cut, layout swap) or dry (the block after a late one). So the ring advances one block
   per audio block in every state and never shows stale audio.
2. The front half of VOX EQ runs ahead of slot 1, so its 40 Hz high-pass and de-esser, and the 70 Hz and
   6 kHz cuts at ON+CUTS, are in what the effects received and so in the trace. They cannot be taken out
   without taking the effects' own input out. The back half, POLISH COMP, output trim, the soft ceiling
   and the bypass mix are not in the trace.
3. The ring holds 2048 samples, 32 blocks, 42.7 ms: 824 for the frame, 1100 for the search, one block
   in flight, rounded up. 8 KB in D1 on the box (`block_mem.h`), plain memory on the host.
4. The read uses a block count. The writer stores the block, then the count, with release. The reader
   loads the count with acquire, copies the 31 newest complete blocks and the energy, then loads the
   count again. If it changed, the writer may have started on the block the reader began with, so the
   reader tries again, up to three times, and keeps the last good frame after that. The samples are
   `std::atomic<float>` with relaxed order, which costs nothing on either target and keeps the host's
   two threads inside the C++ memory model. No interrupt mask on the box.
5. The ring lives in `BlockFlow`, so the box and the emulator share it and the emulator draws the same
   trace from the same code.
6. A frame is taken only while the voice screen shows. `OutputMeter` updates every tick from the energy
   so the top and the floor keep tracking under other screens.

## 6. Code

| Where | What |
|---|---|
| `core/scope.h`, `scope.cpp` | the ring with its block count and energy; `scopeFrame()` pure: samples + last scale + silent to 103 column runs and the new scale (`int8_t lo, hi` rows about the horizon); the trigger |
| `core/block_flow.h`, `block_flow.cpp` | the write of every block into the ring; `Report.outputEnergy` becomes the ring's energy |
| `core/chain_run.h` | unchanged apart from comments |
| `core/level.h`, `level.cpp` | `OutputMeter` unchanged; its energy now comes from the ring; comments |
| `ui/screens.h`, `screens.cpp` | `Ribbon` removed; `renderVoice(canvas, const ScopeFrame&, segments, walk)` |
| `ui/face.h`, `face.cpp`, `ui/gui.h` | a frame from the ring each tick on the voice screen; comments |
| `board/audio.h`, `audio.cpp` | the DAC-side output tally goes; `audioTakeOutputEnergy` reads the ring's |
| `host/box.cpp`, `audio_block.*`, `host/main.cpp` | the same read on the host; the proof scene's output figure |
| `tools/oled-proof/oled_motion.cpp`, `oled_proof.cpp`, `tools/oled_golden.py` | the voice scene draws the scope; goldens regenerated |
| `engine/dsp.h` | `dbToGain` used for the scale; no private pow |
| root `CLAUDE.md` Names | "voice ribbon" becomes "the scope" |

## 7. Tests

1. `core_scope`, trigger: a vowel built from harmonics 1 to 10 weighted around 700 Hz at 98, 147 and
   220 Hz, and a 49 Hz tone; the newest sample steps 1920 samples a tick for 100 ticks; the left edge
   lands on the same cycle feature each tick within one sample, or shifts by whole periods. Repeated
   with breath noise at -20 dB, a DC offset of 2 %, and a glide. No crossing free-runs. A crossing at
   the span's limit does not flip between locked and free.
2. `core_scope`, columns: on random, clipped and full-scale square input every neighbouring pair of
   columns shares a row or touches, the first column included. A frame of zeros gives row 32 everywhere.
   A tick under the floor gives silent.
3. `core_scope`, scale: a 150 Hz sine at -20 dBFS and at -40 dBFS each reach rows 10 and 54; a frame
   at half the previous frame's peak draws at 70 % (the 3 dB fall); a frame 12 dB under fills the
   height by the fourth frame; a held note under the floor plus 6 dB goes to the line.
4. `core_scope`, ring: wrap-around returns the newest samples in order; the energy of a known signal
   equals the old DAC-side sum with the output trim at unity; a host two-thread stress where the writer
   fills each block with its block number and the reader asserts every frame is consecutive numbers.
5. `core_block_tail` or `core_chain_run`: the ring advances one block per audio block through a layout
   swap, a late block and a NaN cut, with zeros where the output was silenced; a POLISH COMP setting that
   changes the output leaves the ring the same.
6. `ui_screens`: the silence frame is the line and the meter box; a known frame draws the expected column
   runs; the burn-in walk shifts the trace with the box to column 127 and row 55 at +1.
7. `ui_face` golden `voice-scope` replaces `voice-meter-ribbon`: scene 12's scripted drive with a
   150 Hz synthetic tone, frame 79. The emulator `--check` golden is regenerated from the same scene.
8. The ribbon cases in `ui_screens`, `ui_face`, `core_level` and `core_canvas` are removed or rewritten.
   `level_test` and the engine tests are untouched. `ctest` runs in full.

## 8. Budget

| Item | Cost |
|---|---|
| Ring, D1 | 8,192 B |
| Write, per block | 64 stores and a 64-sample sum of squares, about 300 cycles, 0.05 % of the 640,000-cycle block budget |
| Read and frame, main loop per tick | one 8 KB copy, a 1100-sample peak search, 824 multiplies, 103 min/max pairs |
| DTCM | none |

## 9. Decisions

1. 8 samples a column (17 ms across) over 16 (34 ms): at 16 a 440 Hz cycle is 7 columns wide and reads
   as hash; at 8 it is 14 columns and the shape shows. The lowest sung notes show under two cycles, which
   a scope on a 20 ms sweep also shows.
2. A trigger over free-running: a free-running trace on a held note swims across the screen every tick
   and looks broken. The peak-anchored crossing over the first crossing: a vowel crosses zero several
   times a cycle (audit finding 3).
3. The scale fits each frame (owner 2026-11-07) over the ribbon's recent-peak rule, which drew an echo
   tail a few rows tall. The floor rule stays so noise draws the line. A fixed dBFS scale was never
   in play: it draws Adam's working level, around -20 dBFS, two pixels tall.
4. No rim dither: a scope trace is a sharp line.
5. The copy sits in `BlockFlow::run`, not `runChain`, so every audio block writes a block and the ring
   never freezes on a swap, a late block or a NaN cut (audit finding 7). The DAC-side energy tally goes
   with it, because the scale must read the signal the trace draws.
6. A block count with retry over a masked copy: the same code runs on the box and the host, and the
   host has two real threads (audit finding 8).

## 10. As built

Built 2026-11-06 at v2.01.66 with a crest-based scale, and rebuilt 2026-11-07 to the scale of section 3,
which fits each frame. The four gates pass: `ctest` 39 of 39, `cubevox-emulator --check`, the firmware
build with `-Wstack-usage=1152`, and `go test ./...`.

The scale fits each frame. `scopeFrame(samples, lastScale, silent)` returns the frame with its new
scale in `ScopeFrame::scale`. The scale is the larger of the frame's peak magnitude over its 824
samples and `lastScale` times `dbToGain(-3)`, so the frame's loudest sample lands on row 10 or 54.
`Face` keeps the scale in its last frame (`scope_.scale`) and passes it back on the next tick. A
silent tick, or a scale under 1e-6 (-120 dBFS), gives the horizon alone and a scale of 0, so a
`lastScale` of 0, negative or not a number starts the scale afresh. `kScopeCrest` is gone, and the scope
no longer reads `OutputMeter::top()`; the meter still updates every tick for the floor rule.

Where the build differs from the design:

1. `runChain` takes an optional `tap(block)` that it calls after the NaN guard and ahead of POLISH COMP.
   POLISH COMP and the back half of VOX EQ run inside `runChain`, so `BlockFlow::run` cannot see the
   block before them without it. `BlockFlow::run` passes a lambda that writes the ring.
2. The ring keeps one energy per block (32 floats) beside the samples, not a running sum and a count.
   `ScopeRing::takeEnergy()` sums the blocks written since its last call, at most the newest 31, with
   the same block-count check as the sample read. That needs no read-modify-write between the threads,
   which Cortex-M7 has no 64-bit form of, and no interrupt mask. `audioTakeOutputEnergy` and
   `AudioBlock::takeOutputEnergy` call it. The face takes its frame by `ScopeRing::read` in a second
   call, so the energy and the frame can differ by a block.
3. The face takes the ring as a `const ScopeRing&` beside the energy, and the board and the host add
   `audioScope()` and `AudioBlock::scope()`.
4. The trigger's crossing search may pass the old end of the span. The peak still has to lie inside the
   span, but its rising crossing is searched down to the second sample of the read. With the crossing
   confined to the span, a peak in the first 30 samples of the span had no crossing and the frame
   free-ran (a 125 sample jump at 49 Hz on 1 tick in about 30).
5. The hysteresis is 5 % of the largest sample in the search span, the peak the trigger anchors on,
   since the frame's scale is not known until the frame's start is.
6. The face sets the scale to 0 on every tick that is not on the voice screen, so the scope has no
   history to catch up on when the voice returns (walk item 6). The floor rule's meter keeps tracking.
7. The board checks at start that the ring sits in RAM_D1 (`checkMemoryAndClock`).
8. A second golden, `voice-scope-loud`, is frame 76 of scene 12, where the tone is at its loudest and the
   trace fills rows 10 to 54. Frame 79 (`voice-scope`) is three frames later, on a tone 18 dB quieter
   whose scale has fallen 9 dB from the loud peak, so the trace spans rows 24 to 40. `voice-meter-ribbon`
   is gone.
9. The emulator's `--check` has no golden of its own. The golden for the scene is in `firmware_test`.
10. The proof renderer (`tools/oled-proof/oled_motion.cpp`) draws the scope from the spec's words and
    draws the real clips' samples on its voice scenes. It needs 48 kHz clips. It replays the scale over
    the 120 frames before each frame, which leaves the scale within 1e-18 of the box's.

Measured on the host (`firmware_test`, ASan and UBSan), worst start drift over 100 ticks of a synthetic
vowel, shifts by whole periods not counted: 0.84 samples at 98 Hz, 0.69 at 147 Hz, 0.91 at 220 Hz and
0.53 at 49 Hz; 0.35 with a 2 % offset; 1.59 with breath at -20 dB; 0.78 with a glide from 98 to 110 Hz;
1.63 for a falling glide at 147 Hz with breath and offset. The tests allow 1.0, 1.0, 3.0, 1.5 and 3.0.
The test vowels peak at 1, so these figures hold for a hysteresis of 5 % of the peak.
The two-thread stress passes with no torn block.
