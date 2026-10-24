# Firmware structure — design

Owner 2026-10-12: the firmware is young, so build it properly now. This spec fixes the module
map and the ownership rules before the OLED font renderer and runtime layouts land. It extends
`2026-10-08-firmware-foundation-design.md` (tiers, host tests, fault handling), which still applies.

## Problems found (audit 2026-10-12)
1. PAD_EN (PE11) is never driven, so relay K103 never switches. MIC mode adds a +25.7 dB digital
   trim that the hardware spec retired (`2026-10-02-hardware-design.md` item 2).
2. The 1/4" INPUT setting defaults to PEDAL. The hardware default is MIC (coil off).
3. The audio IRQ reads the layout live through `slotCard()`; knob step counts are seeded from it
   once. A runtime layout change would run new cards on old params.
4. `ui/menu.cpp` owns the encoder ISR, `pinMode`, `digitalRead` and DFU entry. Menu screens read
   board state directly and draw into a 16 x 8 character grid that `core/display.h` bakes in.
5. Pure logic sits in board files with no host test (debounce, stepping, smoothing, param merge).
6. DFU entry does not mute first.

## Data flow
```
board/input ──UiInput──▶ ui/Menu ──UiAction──▶ sketch ──▶ Settings
board/controls(Layout) ──PanelReading──┐
Layout, Settings, BenchOverrides ──────┴──▶ core/compose ──ChainSnapshot──▶ board/audio (IRQ)
Menu + UiContext ──▶ Menu::draw ──▶ core/Canvas ──▶ board/display_sh1106 (one page per pass)
```
The sketch is the only place that wires modules together. No module calls another tier's
getters behind the sketch's back.

## Module map
| Tier | Module | Owns |
|---|---|---|
| core | `layout` | `Layout` (card per slot), `defaultLayout()`, `layoutValid()` |
| core | `settings` | `Settings` (output level, input jack), `outputTrimDb()` |
| core | `knob_filter` | `DebouncedInput`, `steppedPosition`, `KnobFilter` (smoothing, stepping, move detect, re-seed on step-count change) |
| core | `panel` | `PanelReading` (knob values, toggles, last moved), the value the board hands up |
| core | `compose` | `ChainSnapshot composeSnapshot(Layout, PanelReading, Settings, BenchOverrides)` |
| core | `canvas` | 128 x 64 1 bpp, page-major 1024 B, `drawText`, `textWidth`, `fillRect`, `invertRect`, dirty-page mask |
| core | `fonts` | `FontDesc` table over `cubevox_fonts.h` (Hyperlegible 5x7, 9X12, 16X24) |
| core | `registry` | cards, knob descriptors, apply, format; no layout state |
| ui | `menu` | `Menu` state machine, `UiInput` in, `UiAction` out, the screens as `Menu::draw`, host-tested |
| ui | `screens` | not split out yet; the OLED GUI spec decides when `render(const Menu&, const UiContext&, Canvas&)` leaves `menu.cpp` |
| board | `input` | encoder ISR (counts only), click debounce, one `UiInput` per pass |
| board | `input_jack` | drives PAD_EN from `Settings` |
| board | `controls` | mux scan, ADC, toggles; owns `KnobFilter[16]`; reads step counts from the `Layout` it is given |
| board | `audio` | SAI, DMA, chain walk over `ChainSnapshot.layout` only |
| board | `mute` | sequenced mute, `muteForReset()` (blocking, before DFU or reset) |
| board | `display_sh1106` | panel init, `flushStep(Canvas&)` sends one dirty page per call and clears its dirty bit, so it takes a mutable canvas |
| board | `bench` | framed JSON bench ops (`{"cmd":"bench","op":...}`), `BenchOverrides` |
| sketch | `cubevox.ino` | `setup()`, `loop()`, request dispatch and reply wiring; every decision it calls is pure code in `core/` or `ui/` |

## Rules
1. Tiers. `core` includes only core, engine and the font header. `ui` includes core and ui.
   `board` includes anything. A wrong include breaks the host build, which compiles core and ui.
2. Board code reads pins and drives peripherals; it never decides policy. Policy (which trim,
   which relay state, which screen) is a pure function in core or ui.
3. One source of truth. Pins live only in `pins.h`. Layout lives only in the sketch's `Layout`
   and reaches the IRQ only inside a snapshot. Settings live only in the sketch's `Settings`.
4. The IRQ reads exactly one published `ChainSnapshot` per block. Gains are linear in the
   snapshot (no `powf` in the IRQ).
5. Every core and ui module has host tests in `test/firmware/<tier>_<module>.cpp`.
6. `loop()` never blocks on I2C for a full frame. The display sends at most one page per pass.
7. Conventions follow `~/.claude/rules/cpp.md`; functions stay under 40 lines.

## Steps (one commit each, `ctest` and `firmware/build.sh` green each)
1. Bugs. `core/settings` with MIC as the default; `board/input_jack` drives PAD_EN; delete
   `inputGainDb` and the +25.7 dB trim; `muteForReset()` before every DFU entry; fix the stale
   trim note in the hardware spec.
2. Snapshot. `core/layout`, `core/panel`, `core/compose`; `audioPublish(const ChainSnapshot&)`;
   `slotCard()` removed from the registry; controls take the layout.
3. `core/knob_filter` from controls, with tests.
4. `board/input` and the pure `Menu`; host build compiles `ui/`; DFU is a `UiAction`.
   A 1/4" INPUT change dips the output mute around the K103 switch, so the 25.7 dB level step
   and the contact click never reach the output.
5. Display plumbing. `core/canvas`, `core/fonts`, SH1106 page flush; host test writes
   `build/oled-*.pgm`. The existing screens move onto the canvas unchanged in content.
   The OLED GUI itself (screens, navigation, what shows when) is planned with the owner in
   its own spec later (owner 2026-10-12); `ui/screens` is built from that spec.
6. Registry: curve field on `KnobDesc` (Decay is log, Response min > max), one gate decay
   mapping, key positions derived from `cv::kKeyName`.
7. Cleanups: stats reads with IRQ masked, PC5 / PB5 / PE2-PE6 into `pins.h`, wrong preemption
   comment in `audio.cpp`, firmware version on the OLED. `Cost::measured` stays because it is a
   protocol field the app reads.

## Verification
1. Each step: `ctest --test-dir build` and `firmware/build.sh`, output quoted in the report.
2. Step 5: the PGM renders read against the font editor's `screens.png`.
3. On hardware (relay click in PEDAL, OLED, audio on the snapshot): `firmware/bringup.py` checks
   on the test board or the first JLC board.

## Deferred
1. Duplicate cards (owner 2026-10-12, on hold). Engines are one object per card type, so
   `layoutValid` rejects a repeated card. Allowing repeats means per-slot engine instances and
   a RAM budget beside the CPU budget; pitch and reverb cards stay single (shared ~100 KB
   tracker in DTCM, large reverb buffers).
