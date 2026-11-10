# cubevox — VOX EQ is renamed CLEANUP EQ, with OFF, LIGHT and STRONG

Owner 2026-11-10: "those names, and vox eq, are not very descriptive", then "why dont we call it
CLEANUP EQ with settings: OFF, LIGHT", then "what aboute light strong", then "ok".

## Scope
1. The menu row `VOX EQ` becomes `CLEANUP EQ`. Its values `OFF`, `ON`, `ON+CUTS` become `OFF`,
   `LIGHT`, `STRONG`, in the same order, default `OFF`.
2. The stage itself is unchanged: the sound, the tunings, the fades, the trim and the CPU row are
   those of `2026-11-05-vox-eq-design.md`.

## The user's walk
1. Sees. The row reads `CLEANUP EQ` with `OFF` on a new box. `CLEANUP EQ` is 72 px wide in the 7X12
   font and `STRONG` is 46 px, so the gap between them is 8 px, against the 7 px every row holds.
2. Wants. His voice as it comes in, or cleaned up a little, or cleaned up a lot.
3. Does. One click steps `OFF`, `LIGHT`, `STRONG`, then `OFF` again.
4. Hears at `LIGHT`. Rumble under 40 Hz is gone, harsh "s" sounds are softened by up to 6 dB at
   5.8 kHz, 300 Hz is down 2.5 dB and 3.5 kHz is up 1.5 dB.
5. Hears at `STRONG`. Everything in `LIGHT`, and the microphone's signal is cut under 70 Hz and above
   6 kHz. The low-pass is a filter for the microphone, not for the effects.
6. Goes wrong, an old record. Byte 9 keeps its values 0, 1 and 2, so a saved setting loads as the
   same setting under its new name.
7. Goes wrong, an old request. A `settings` object that carries `vox_eq` has the key skipped like any
   unknown key.

## Names
| Where | Before | After |
|---|---|---|
| Menu, emulator, proofs, docs | `VOX EQ`, `OFF` / `ON` / `ON+CUTS` | `CLEANUP EQ`, `OFF` / `LIGHT` / `STRONG` |
| Settings | `Settings::voxEq`, `VoxEq::Off` / `On` / `Cuts` | `Settings::cleanupEq`, `CleanupEq::Off` / `Light` / `Strong` |
| Protocol | `vox_eq`: `off` / `on` / `cuts` | `cleanup_eq`: `off` / `light` / `strong` |
| Cost row, bench op | `kVoxEq`, `BenchOp::VoxEq`, id `vox_eq`, `[bench] VOX EQ forced off` | `kCleanupEq`, `BenchOp::CleanupEq`, id `cleanup_eq`, `[bench] CLEANUP EQ forced off` |
| `cubevox-render` | `--vox-eq off\|on\|cuts` | `--cleanup-eq off\|light\|strong` |
| Stress run | `settings.vox_eq` `"cuts"` | `settings.cleanup_eq` `"strong"` |

The engine's file `engine/polish.h`, its class `Polish` and `PolishParams::cuts` keep their names.

## Tests
1. `firmware_test`: the row's label and its three values in order; the 7 px gap rule over every row
   (the tightest stays 7 px); the record round trip at each setting; a request with `cleanup_eq` at
   each string; a request with `vox_eq` changes nothing; the reply carries `cleanup_eq`.
2. `tools/scripts_check.py` and `tools/bench_ops_check.py` pass with the new ids.
3. A menu proof of the row at `STRONG` for the owner.

## Supersedes
`2026-11-05-vox-eq-design.md`: the row's name, the three values' names, the `vox_eq` protocol
string and the "Names" section.
