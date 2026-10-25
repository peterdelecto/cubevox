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
- Specs live in `docs/specs/`, one per stage and subsystem; `docs/specs/INDEX.md` lists every spec
  with what it covers and what superseded it. Read the index first, then the specs the task touches.
  Current direction (owner 2026-10-24): the Mac app is parked; the box ships to Adam with the default
  chain and updates by UF2 drag-and-drop, `2026-10-24-field-update-design.md`.
- `hardware/` holds the KiCad project. Teensy-H7-Port and DrumSynthV3 are references to read,
  never to edit; nothing in cubevox is copied from them any more (field-update spec).

## Names
Use these words or a plain description. Never a coined label (owner 2026-10-24).
- Enclosure: the box, lid, floor, front wall, rear wall (the back wall), beads, the TPU boot, coupon, solder jig.
- Panel: slot, pot, bypass toggle, MUTE toggle, MENU (the one encoder), mux, K102 (mute relay), K103 (pad relay).
- Effects: effect card (a library card), chain, layout, stage, engine, POLISH COMP, POLISH EQ, output trim, soft ceiling.
- Screens: splash, face, voice ribbon, readout and its fixed field, MUTED and BYPASS screens, menu, the update screens.
- Update path: bootloader, update mode, the CUBEVOX drive, DIAG.TXT, boot block, boot report, image header, diag log, stall watch, boot-fail counter.
- People: the owner, Adam (the singer).
- Agents: "the agent working the <area> files", or its name. A group of files is "its files", not a named unit.

## Build / run
```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
open build/cubevox-proto.app
build/cubevox-render in.wav out.wav --depth 0.7
node tools/font-editor/serve.js   # font editor; edits save straight to tools/font-editor/fonts/cubevox-fonts.json
cd app && go run . --fake        # Mac app on a fake box; go test ./... ; app/bundle.sh builds dist/cubevox.app
```

## Panel (planned)
Slots (Adam 2026-10-07, `docs/specs/2026-10-07-slots-design.md`): 8 effect slots, each two identical pots and a bypass toggle; 16 pots on the mux. The slot is the fixed hardware unit. Adam places effect cards in slots from the Mac app's page (owner 2026-10-08, `docs/specs/2026-10-08-update-and-slots-design.md`); the box stores the layout in flash bank 2 and the OLED only shows it. The chain is the slot order, left to right. Top-left toggle is a global MUTE (output only, chain keeps running); the bypass relay K101 is gone, K102 mutes. A slide power switch SWPWR1 on the rear wall between the combo jack and the barrel breaks the 9 V input (owner 2026-10-09, owner-soldered, slide toward the barrel = ON). MENU is the only encoder; KEY and SEMITONES are pots stepped in firmware. Firmware updates by dragging a `.uf2` onto the `CUBEVOX` drive, reached from the menu's `Update firmware` item or by holding MENU at power-on (`docs/specs/2026-10-24-field-update-design.md`).
Default layout, slot 1 to 8: Input Gain (analog, off the grid) · Input gate Threshold+Decay (same controls as Gate) · Autotune Key+Response (NATURAL → MECHANICAL, no mix) · Harmony has no card (owner 2026-10-08) and no engine in the code since 2026-10-24 ·
Octave Semitones (-12..+12, 25 positions, owner 2026-10-08)+Mix ·
Unison Depth+Rate · Slapback Mix (level + repeats)+Time (60..250 ms) · Distortion Drive+Tone · Gate Threshold+Decay ·
Reverb Mix+Time (decay; replaced Dwell, owner 2026-10-07): each engine is its own library card, "Spring Reverb" (SPRING B) in the default layout, "Chasm Reverb" available; the old Spring engine is removed (owner 2026-10-08; the engine menu item is gone) · EQ (menu) · Output (menu)
