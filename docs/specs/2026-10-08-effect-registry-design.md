# Effect registry design (2026-10-08)

Step 1 of `2026-10-08-update-and-slots-design.md`. Owner interviews 2026-10-08 (two rounds).

## Plan

Firmware holds the registry and is the only source of truth. The Mac app reads it over
`hello` (step 3) and never hardcodes it. The slot table becomes a runtime layout, slot
to card index, with defaults from the slots spec table. Flash persistence is step 2.

Files: `firmware/cubevox/registry.h/.cpp` (cards, knob descriptors, groups, apply),
`firmware/cubevox/costs.h` (per-release numbers), `firmware/cubevox/bench.cpp` (bench mode).

## Decisions

| Item | Decision |
|---|---|
| Card ids | `empty`, `input_gate`, `autotune`, `octave`, `unison`, `slapback`, `distortion`, `gate`, `reverb_spring`, `reverb_chasm`. Never renamed or reused once shipped. |
| Card index | `enum class Card : uint8_t` in id order; the enum value is the table index. |
| Knob descriptor | name, kind (continuous or stepped), positions, min, max, unit. Bipolar knobs are min below 0, max above. |
| Knobs | Input gate and Gate: Threshold (-70..-10 dB), Decay (5..1000 ms, log). Autotune: Key (12 positions, `kKeyName`), Response (log between `kMinResponseMs` and `kMaxResponseMs`). Octave: Semitones (-12..+12, 25 positions), Mix (0..100 %). Unison: Depth, Rate (%). Slapback: Mix (%), Time (60..250 ms). Distortion: Drive, Tone (%). Reverb cards: Mix (%), Time (%). Empty: no knobs. |
| Display names | "Input gate", "Autotune", "Octave", "Unison", "Slapback", "Distortion", "Gate", "Spring Reverb" (SPRING B engine), "Chasm Reverb". |
| Groups | `reverb`: one card per layout, box rejects a second, page greys the rest. `pitch`: autotune and octave, run as one stage at the first one's slot; their relative order has no effect. |
| Harmony | No card. The engine stays in `engine/` but the firmware keeps it forced off. Removed from the registry because a second Key pot conflicted with Autotune's. |
| Old Spring | Engine `kReverbSpring` (`engine/spring.h`) removed from engine, emulator, renderer and tests in its own commit before the registry. Chasm and Spring B won the audition. Spring B takes the id `reverb_spring`. |
| Slot hardware | Toggle pin and the two mux channels stay a fixed per-slot table. The card index is the only runtime part. |
| Cost | Two integer percents of one audio block, mean and worst single block, rounded up (CPU pass 2 decision 13, 2026-10-08). Pitch tracker counted once; each pitch card adds only its own work. A fixed term covers the output EQ and sample conversion. A layout is accepted when the placed cards plus the fixed term sum under 75 % on the mean column and 95 % on the worst column. The page shows the mean sum. |
| Estimates | Each card carries `measured`. Until a board measures it the value is an estimate and the page shows "est." Every card was measured on the WeAct on 2026-10-08. |
| Bench | Default layout on the built-in voice clip, every knob pinned at its top position (bench key `k`), per-slot cycle counters from the chain run, worst taken as the lowest worst over repeated runs because interrupt time inside a block inflates single readings. Octave is the pitch slot with Octave on minus off; Chasm runs in the reverb slot under bench key `c`. Numbers go into `costs.h` by hand per release. The earlier plan (built-in sweep signal, each card alone, `CUBEVOX_BENCH` table at boot) is superseded by this in-chain measurement, which includes the cache interaction the cards see in use. |
| Empty card | Cost 0, no knobs, passes audio through. |

## Open questions resolved

1. Two reverb cards: not allowed (one `Reverb` object serves all engines; RAM is at 71 %).
2. Bench input: built-in signal, not live mic, so costs are repeatable.
3. Costs before boards: flagged estimates, not zeros, so the budget check is exercised (measured since 2026-10-08).
4. Reverb id naming: all under `reverb_*`; nothing had shipped.
5. Descriptor depth: full fields, not names only, so `status` can report values later.
6. Pitch group cost: tracker once, not summed per card.
7. Harmony Key pot: card removed rather than linked to Autotune.
8. Semitones on a pot: 49 positions is 6° each on a 300° pot, so the range halves to ±12.

## Recommended next steps

1. Rip out the old Spring engine (own commit, like Parker).
2. Write `registry.h/.cpp`: card table with descriptors, groups and apply functions; retire `Effect`/`Param` enums in favour of card index + knob index.
3. Runtime layout array with the default table; chain walks the layout.
4. `costs.h` with estimates from the emulator's relative timings, `measured = false`.
5. `bench.cpp` behind `CUBEVOX_BENCH`; prints the costs table over serial at boot.
6. Build with `firmware/build.sh`, both with and without the bench flag.
7. Step 3 then exposes the registry and `bench` over serial.
