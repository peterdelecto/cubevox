# cubevox

Live vocal effects box for Adam Keith. Tabletop, SM58 in, one knob per parameter.
Final target: STM32H7 (H7 VIT6 board). Hardware spec: `docs/specs/2026-10-02-hardware-design.md`;
the schematic follows ClaudeRouter's board recipe and canon §XIV. Effects are prototyped one
at a time in a Mac emulator that plays a loaded loop through the effect while the knob is tweaked.

## Rules
- Engine and emulator rules live in `engine/CLAUDE.md` and `emulator/CLAUDE.md`.
- Level rule (owner 2026-10-01): every stage engaged at its default setting outputs
  near unity, 0 to +0.5 dB RMS over its input on program material. The trim that
  gets a stage there lives in its tuning (wetDb / trimDb); the panel knobs never
  carry a hidden level offset. `level_test` enforces it. Stages with a MIX knob
  (Harmony, Octave, Reverb) are trimmed so the WET path alone at MIX 100 % is unity
  (Harmony: one voice at High); the equal-power crossfade then stays near unity at
  every MIX. Trimming at MIX 50 % instead pushed the wet +8 dB and clipped at full mix
  (owner 2026-10-02).
- Version (owner 2026-10-02): the face shows `VERSION.txt` bottom left (started v0.01.00).
  Every change that rebuilds the app bumps it first with `tools/bump_version.sh`, in the
  same commit. Not named `VERSION`: the Mac's case-insensitive disk lets `<version>` find it.
- Gates never open a window: `cubevox-proto --layout` and `ctest` are the checks.
- Specs live in `docs/specs/`. Current: `2026-10-01-gate-design.md`; one spec per stage under `docs/specs/`, all still apply. `2026-10-02-hardware-design.md` is the board. `2026-10-03-spring-b-design.md` is SPRING B. `2026-10-08-update-and-slots-design.md` is the Mac app, the slot layout and firmware updates. `2026-10-08-effect-registry-design.md` is the firmware effect registry (card ids, knob descriptors, groups, costs, bench).
- `hardware/` holds the KiCad project. Teensy-H7-Port and DrumSynthV3 are read-only sources
  to copy from, never to edit.

## Build / run
```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
open build/cubevox-proto.app
build/cubevox-render in.wav out.wav --depth 0.7
```

## Panel (planned)
Slots (Adam 2026-10-07, `docs/specs/2026-10-07-slots-design.md`): 8 effect slots, each two identical pots and a bypass toggle; 16 pots on the mux. The slot is the fixed hardware unit. Adam places effect cards in slots from the Mac app's page (owner 2026-10-08, `docs/specs/2026-10-08-update-and-slots-design.md`); the box stores the layout in flash bank 2 and the OLED only shows it. The chain is the slot order, left to right. Top-left toggle is a global MUTE (output only, chain keeps running); the bypass relay K101 is gone, K102 mutes. MENU is the only encoder; KEY and SEMITONES are pots stepped in firmware. Firmware updates by DFU from the app's Update screen or from the menu.
Default layout, slot 1 to 8: Input Gain (analog, off the grid) · Input gate Threshold+Decay (same controls as Gate) · Autotune Key+Response (NATURAL → MECHANICAL, no mix) · Harmony: a library card, not in the default layout (owner 2026-10-08) ·
Octave Semitones+Mix ·
Unison Depth+Rate · Slapback Mix (level + repeats)+Time (60..250 ms) · Distortion Drive+Tone · Gate Threshold+Decay ·
Reverb Mix+Time (decay; replaced Dwell, owner 2026-10-07): each engine is its own library card, SPRING B in the default layout, CHASM and SPRING available (owner 2026-10-08; the engine menu item is gone) · EQ (menu) · Output (menu)
