# Specs

One spec per stage and per subsystem. All still apply unless a later spec says it supersedes a
part. Filenames are `YYYY-MM-DD-name-design.md`. Add a row here with every new spec.

| Spec | Covers | Status |
|---|---|---|
| `2026-10-01-autotune-design.md` | Autotune: KEY and RESPONSE, correction inside `PitchFx` that replaces the dry path | current; engine B, chromatic mode and the Harmony key link are out of the code |
| `2026-10-01-chasm-design.md` | CHASM reverb, a second engine ported from DrumSynthV3, the Chasm Reverb card | current; its knobs are superseded by the v2 spec (MIX and TIME) and the engine radio by the registry's reverb cards |
| `2026-10-01-distortion-design.md` | Distortion stage, a Boss BD-2 model with DRIVE and TONE | current; the menu bypass is the slot toggle; always 2x oversampled |
| `2026-10-01-gate-design.md` | Gate stage; the stage specs follow its shape | current |
| `2026-10-01-harmony-design.md` | Harmony stage, a Zoom V3 model with KEY and MIX | the box has no Harmony card (owner 2026-10-08); out of the code: no card, engine, test or emulator controls; the key tables live in `engine/scale.h` |
| `2026-10-01-harmony-engines-design.md` | Harmony shifter engines A, B and C, per-voice formant, chromatic mode | out of the code with Harmony; reference only |
| `2026-10-01-level-rule.md` | The level rule (every stage at its default outputs 0 to +0.5 dB RMS over its input) and how `level_test` trims it | current; CLAUDE.md states the rule |
| `2026-10-01-octave-b-design.md` | Octave engine B, epoch-aligned PSOLA with formant control | out of the code; reference only |
| `2026-10-01-octave-c-design.md` | Octave engine C, an asynchronous granular shifter | current; the only Octave engine |
| `2026-10-01-octave-pitchfx-design.md` | The Octave stage and the shared pitch front end `PitchFx` (ring, tracker, crossfades) | front end current; Harmony and Octave option A are out of the code |
| `2026-10-01-polish-eq-design.md` | The final output EQ ("polish"), a menu stage last in the chain | current for the EQ; its place in the tail and the menu row are superseded by the polish spec |
| `2026-10-01-slapback-design.md` | Slapback stage: delay, low-pass, wet level | current; the panel is MIX and TIME (v2 spec), the 30..120 ms range is superseded by the v2 spec's 60..250 ms, and INTENSITY is the MIX knob |
| `2026-10-01-unison-emulator-design.md` | UNISON, and the emulator skeleton, engine contract, threading and build layout the stage specs share | current for the contract; its panel table is superseded by the slots spec; the fixed-detune shifter is out of the code |
| `2026-10-02-adam-face-design.md` | Adam's face: hardware knobs and musician tuning sliders in the emulator | emulator only, and the Mac emulator is parked (owner 2026-10-24); the reverb engine radio is superseded |
| `2026-10-02-hardware-design.md` | The board | the firmware-update sentences of items 10 and 13 (menu reboot into the ROM DFU) are superseded by the field-update spec; item 27 by the enclosure-closure spec |
| `2026-10-03-spring-b-design.md` | SPRING B reverb, the Spring Reverb card (`engine/spring.h`) | current; the panel is MIX and TIME (v2 spec) |
| `2026-10-07-slots-design.md` | Eight slots of two pots and a toggle, 16 pots on the mux, the MUTE toggle, the default slot table | current; card placement and chain order are superseded by the update-and-slots spec, and the menu reboot into the ROM DFU by the field-update spec |
| `2026-10-07-v2-mic-reverb-time-slapback.md` | v2: a live mic in the emulator, reverb TIME in place of DWELL, slapback 60 to 250 ms | built; the box has reverb TIME and the slapback range |
| `2026-10-08-update-and-slots-design.md` | Mac app, slot layout, firmware updates | page parts superseded by the Mac-app spec; the Release row (`cubevox.bin`, app tar, `install.sh`), the Recovery section (ROM DFU, BOOT0, no A/B) and the menu item that enters DFU are superseded by the field-update spec |
| `2026-10-08-effect-registry-design.md` | Firmware effect registry: card ids, knob descriptors, groups, costs, bench | current |
| `2026-10-08-enclosure-closure-design.md` | Enclosure closure: straight PETG bead-and-groove walls and a double-bead TPU boot, no fasteners | current |
| `2026-10-08-firmware-foundation-design.md` | Firmware directory tiers (core/board/ui), host tests, fault handling, toolchain pin | current |
| `2026-10-08-pitch-cpu-pass-design.md` | Pitch stage CPU pass: tracker gating, warm buffers, spinning window, block-rate gains | current |
| `2026-10-08-cpu-pass-2-design.md` | CPU pass 2: Unison, Distortion, DTCM placement, pane tracker, PSOLA weights, 75 % / 95 % budget rule | done 2026-10-08 with the bench numbers and the measured cost table; owner listening done 2026-10-09 (both pairs fine) |
| `2026-10-08-cpu-pass-3-design.md` | CPU pass 3: Distortion restore after the D3 regression, Spring B disperser pair, Unison voice interleave, restrict/locals, agent worktree merge rule | done 2026-10-09; chain 54.7 % / 64.2 %, default layout sums 61 / 72 |
| `2026-10-11-font-sizes-design.md` | OLED fonts: Stylized Hyperlegible 5x7, 9X12 and 16X24 (owner 2026-10-11), exported to `tools/font-editor/fonts/cubevox_fonts.h` | current; renamed Myrtle 5x7 / 9X12 / 16X24 (owner 2026-10-29) |
| `2026-10-12-layout-record-design.md` | Layout record in flash bank 2: schema v1 and v2, append log, ECC recovery | current |
| `2026-10-12-firmware-structure-design.md` | Firmware module map and ownership rules (snapshot, pure menu, canvas); new firmware follows it | current |
| `2026-10-12-serial-protocol-design.md` | USB JSON protocol, live layout apply with a 50 ms fade | current |
| `2026-10-15-mac-app-design.md` | Mac app: one page, slots and a card bank, headroom meter, one Update button | app parked (owner 2026-10-24); box side of Update superseded by the field-update spec |
| `2026-10-24-field-update-design.md` | Field update: TinyUF2 bootloader in sector 0, app at 0x08020000 with a CRC header, shared boot block, Adam drags a `.uf2` onto the `CUBEVOX` drive; `firmware/platform/` absorbed into `board/` | current; host side built and on `main` 2026-10-24, screens approved; shop steps and drills wait for a board |
| `2026-10-24-polish-design.md` | Polish tail: POLISH COMP and POLISH EQ close the chain, TOP SWITCH is MUTE or BYPASS, the final seven-row menu, layout record schema v2 and the protocol `settings` object | built, bench pending |
| `2026-10-25-font-16x24-neo-grotesque-design.md` | The 16X24 value font redrawn as a condensed neo-grotesque from pixel recipes: the construction rules, widths, per-glyph forms, the gate with 0/O dropped, and the build procedure | built 2026-10-25 |
| `2026-10-24-oled-gui-design.md` | OLED runtime screens: the voice ribbon, the knob readout with rolling digits in a fixed field per parameter (no re-centring), MUTED or BYPASS, the 2-frame dissolve, the burn-in walk, and the `level`, `gui`, `screens` and `face` modules; settles the `ui/screens` split the structure spec deferred; the SH1106 driver shared with the bootloader and its retry rule, which supersede item 6 (U8x8 tile mode) of the font-sizes spec | host part built 2026-10-24, proofs 3 to 10 approved with the fixed field, bench pending |
| `2026-10-24-solder-jig-design.md` | Printed jig for hand-soldering the four underside jacks; `solder-jig` component in the Fusion enclosure document | current; rev 4 shadow pockets built 2026-10-27 |
| `2026-10-27-oled-adapter-design.md` | Printed adapter between the board's OLED holes and the module's, which sit about 1 mm north; posts with crush ribs slid by the parameter `oledOffset` | current; pegs chosen 2026-10-27; crush ribs fit no hole, plain pegs held nothing, tapered barbs drawn; final 2026-10-30: eight split pins at the hole diameters with a 0.5 slot, owner's values, printed and fits |
| `2026-10-29-readout-card-design.md` | The readout redrawn as one effect's card: slot and name in a strip, a knob picture beside each parameter name, a switch picture for the toggle, the big value; rows follow the panel's upper and lower pot | proof D approved 2026-10-29; not built; supersedes the Readout section of the OLED GUI spec |

Research notes live in `docs/audits/`. `2026-10-08-pitch-tracker-research.md` is the note behind
the tracker plan.
