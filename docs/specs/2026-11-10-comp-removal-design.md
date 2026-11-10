# cubevox — POLISH COMP is removed

Owner 2026-11-10: "maybe we shouldnt have the comp at all", then "remove".

## Why
1. The venue's mixer already compresses the vocal channel, set by someone who hears the room.
2. The threshold (-30 dBFS) and makeup (+5.85 dB) were fit to the three clips at -23 dBFS RMS. The
   clips are for demos and tests and do not stand for a performance (owner 2026-11-10), so on stage
   the depth and the output level follow wherever the Input Gain pot sits.
3. The makeup is the stage's gain on anything under -32 dBFS. Between phrases the box ran 5.85 dB
   louder than while Adam sang, which costs feedback margin at a wedge and swells reverb and echo tails.
4. With VOX EQ at OFF the makeup also lifted rumble under 40 Hz, which nothing ahead of it removed.

## The user's walk
1. Sees. The menu has 13 rows. `VOX EQ` follows the SLOT rows directly, and `POLISH COMP` is gone.
2. Hears. The chain's output reaches VOX EQ's back half as the last slot left it. A box that had
   POLISH COMP on plays up to 5.85 dB quieter in soft passages and near the same level in loud ones.
3. Goes wrong, an old record. A record saved with POLISH COMP on or off loads with every other
   setting kept. The byte is retired.
4. Goes wrong, an old request. A `settings` object that carries `polish_comp` has the key skipped
   like any unknown key. Replies no longer carry it.

## Signal path tail
```
slot 1 … slot 8 → NaN guard → scope tap → VOX EQ back half → soft ceiling → output trim → DAC
```
The soft ceiling, the output trim, the NaN guard and the BYPASS mix are unchanged.

## What goes
1. `engine/polish_comp.h`, `test/polish_comp_test.cpp` and their CMake entries.
2. `ChainEngines::comp`, `ChainParams::comp`, the comp call and its cycle bucket in `runChain`, the
   comp's stat bucket and every reader of it.
3. `Settings::polishComp`, the menu row and its action, the protocol key `polish_comp`.
4. The bench override and bench op that force the comp off, in the firmware and in the host scripts
   (`firmware/bench_ops.py`, `firmware/stress.py`), with `tools/bench_ops_check.py` and
   `tools/scripts_check.py` still passing.
5. `kPolishComp` in `core/costs.h`. `kFixed` becomes `kVoxEq + kSoftCeiling + kBypassMix`, 5 / 6,
   which gives every layout 3 % mean and 4 % worst back. `app/fixtures/hello.json` is refreshed.
6. `cubevox-render --polish-comp` and `--comp-stats`, the emulator's comp parameter, the comp rows of
   `level_test` and its whole-tail rows (the tail is VOX EQ alone now, which its own rows judge).

## The record
The byte at `kOffPolishComp` is retired like byte 3: written 0 and ignored on read, whatever it
holds. No schema bump. An earlier firmware reads the 0 as POLISH COMP off.

## VOX EQ
Unchanged in sound and settings. Its 40 Hz subsonic high-pass stays at ON and ON+CUTS: energy under
40 Hz carries no loudness and still counts in the level. The back half's trim stays +1.15 dB, fit on
VOX EQ alone. Comments and the cost header no longer name the comp.

## Tests
1. `firmware_test`: the menu's 13 rows in order; a record with the retired byte at 0, 1 and 255 loads
   with the other settings kept and saves the byte as 0; a request carrying `polish_comp` changes
   nothing and the reply has no such key; the chain's output with VOX EQ at OFF equals the block the
   guard passed, bit for bit.
2. `level_test`: the VOX EQ rows at ON and ON+CUTS hold as before.
3. The menu proofs are redrawn for the owner.

## Supersedes
1. `2026-10-24-polish-design.md`: the POLISH COMP section, menu row 7, `Settings::polishComp`, and
   the comp's place in the tail.
2. `2026-11-05-vox-eq-design.md`: walk item 9's whole-tail readings and the reason given for the
   subsonic high-pass.
3. `2026-11-01-stress-bench-design.md`: the comp bucket and its gate.
4. `2026-11-06-scope-design.md`: the tap sits ahead of VOX EQ's back half; no comp follows it.
