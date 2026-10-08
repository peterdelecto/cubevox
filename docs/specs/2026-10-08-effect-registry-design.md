# Effect registry design (2026-10-08)

Step 1 of `2026-10-08-update-and-slots-design.md`. Owner interview 2026-10-08.

## Plan

Firmware holds the registry and is the only source of truth. The Mac app reads it over
`hello` (step 3) and never hardcodes it. The slot table becomes a runtime layout, slot
to card index, with defaults from the slots spec table. Flash persistence is step 2.

Files: `firmware/cubevox/registry.h/.cpp` (cards, knob descriptors, groups, apply),
`firmware/cubevox/costs.h` (per-release numbers), `firmware/cubevox/bench.cpp` (bench mode).

## Decisions

| Item | Decision |
|---|---|
| Card ids | `empty`, `input_gate`, `autotune`, `octave`, `harmony`, `unison`, `slapback`, `distortion`, `gate`, `reverb_spring_b`, `reverb_chasm`, `reverb_spring`. Never renamed or reused once shipped. |
| Knob descriptor | name, kind (continuous or stepped), positions, min, max, unit. Bipolar knobs are min below 0, max above. |
| Groups | `reverb`: one card per layout, box rejects a second, page greys the rest. `pitch`: autotune, octave, harmony all placeable, run as one stage at the first one's slot; their relative order has no effect. |
| Cost | Integer percent of one audio frame at peak. Pitch tracker counted once; each pitch card adds only its own work. 85 % line sums placed cards. |
| Estimates | Each card carries `measured`. Until a board measures it the value is an estimate and the page shows "est." |
| Bench | Built-in signal (sine sweep plus noise bursts with gaps), each card alone, 3 s, peak block time. Compile flag runs it at boot and prints the table now; serial `bench` command in step 3. Numbers go into `costs.h` by hand per release. |
| Harmony | Placing the card turns the engine on (replaces the forced-off from 2026-10-02). |
| Empty card | Cost 0, no knobs, passes audio through. |

## Open questions resolved

1. Two reverb cards: not allowed (one `Reverb` object serves all engines; RAM is at 71 %).
2. Bench input: built-in signal, not live mic, so costs are repeatable.
3. Costs before boards: flagged estimates, not zeros, so the 85 % check is exercised.
4. Reverb id naming: all three under `reverb_*`; nothing had shipped.
5. Descriptor depth: full fields, not names only, so `status` can report values later.
6. Pitch group cost: tracker once, not summed per card.

## Recommended next steps

1. Write `registry.h/.cpp`: card table with descriptors, groups and apply functions; retire `Effect`/`Param` enums in favour of card index + knob index.
2. Runtime layout array with the default table; chain walks the layout.
3. `costs.h` with estimates from the emulator's relative timings, `measured = false`.
4. `bench.cpp` behind `CUBEVOX_BENCH`; prints the costs table over serial at boot.
5. Build with `firmware/build.sh`, both with and without the bench flag.
6. Step 3 then exposes the registry and `bench` over serial.
