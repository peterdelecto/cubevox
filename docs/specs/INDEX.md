# Specs

One spec per stage and per subsystem. All still apply unless a later spec says it supersedes a
part. Filenames are `YYYY-MM-DD-name-design.md`. Add a row here with every new spec.

| Spec | Covers | Status |
|---|---|---|
| `2026-10-01-autotune-design.md` | Autotune: KEY and RESPONSE, correction inside `PitchFx` that replaces the dry path | current |
| `2026-10-01-chasm-design.md` | CHASM reverb, a second engine ported from DrumSynthV3 | current; the engine radio is superseded, because each reverb engine is its own card ("Chasm Reverb") |
| `2026-10-01-distortion-design.md` | Distortion stage, a Boss BD-2 model with DRIVE and TONE | current |
| `2026-10-01-gate-design.md` | Gate stage; the stage specs follow its shape | current |
| `2026-10-01-harmony-design.md` | Harmony stage, a Zoom V3 model with KEY and MIX | the box has no Harmony card (owner 2026-10-08); reference only |
| `2026-10-01-harmony-engines-design.md` | Harmony shifter engines A, B and C, per-voice formant, chromatic mode | the box has no Harmony card; reference only |
| `2026-10-01-level-rule.md` | The level rule (every stage at its default outputs 0 to +0.5 dB RMS over its input) and how `level_test` trims it | current; CLAUDE.md states the rule |
| `2026-10-01-octave-b-design.md` | Octave engine B, epoch-aligned PSOLA with formant control | reference only; the box runs Octave C |
| `2026-10-01-octave-c-design.md` | Octave engine C, an asynchronous granular shifter | current; the engine the box runs |
| `2026-10-01-octave-pitchfx-design.md` | The Octave stage and the shared pitch front end `PitchFx` | current for the Octave stage and the front end; the Harmony pairing is reference only |
| `2026-10-01-polish-eq-design.md` | The final output EQ ("polish"), a menu stage last in the chain | current; POLISH EQ is a menu row (polish spec) |
| `2026-10-01-slapback-design.md` | Slapback stage: delay, low-pass, wet level | current; the panel is MIX and TIME (v2 spec) |
| `2026-10-01-unison-emulator-design.md` | UNISON, and the emulator skeleton, engine contract, threading and build layout the stage specs share | current for the contract; its panel table is superseded by the slots spec |
| `2026-10-02-adam-face-design.md` | Adam's face: hardware knobs and musician tuning sliders in the emulator | emulator only, and the Mac emulator is parked (owner 2026-10-24); the reverb engine radio is superseded |
| `2026-10-02-hardware-design.md` | The board | the firmware-update sentences of items 10 and 13 (menu reboot into the ROM DFU) are superseded by the field-update spec |
| `2026-10-03-spring-b-design.md` | SPRING B reverb | current; the panel is MIX and TIME (v2 spec) and the card is named "Spring Reverb" |
| `2026-10-07-slots-design.md` | Eight slots of two pots and a toggle, 16 pots on the mux, the MUTE toggle, the default slot table | current; the menu reboot into the ROM DFU is superseded by the field-update spec |
| `2026-10-07-v2-mic-reverb-time-slapback.md` | v2: a live mic in the emulator, reverb TIME in place of DWELL, slapback 60 to 250 ms | built; the box has reverb TIME and the slapback range |
| `2026-10-08-update-and-slots-design.md` | Mac app, slot layout, firmware updates | page parts superseded by the Mac-app spec; the Release row (`cubevox.bin`, app tar, `install.sh`), the Recovery section (ROM DFU, BOOT0, no A/B) and the menu item that enters DFU are superseded by the field-update spec |
| `2026-10-08-effect-registry-design.md` | Firmware effect registry: card ids, knob descriptors, groups, costs, bench | current |
| `2026-10-08-enclosure-closure-design.md` | Enclosure closure: straight PETG bead-and-groove walls and a double-bead TPU boot, no fasteners | current |
| `2026-10-08-firmware-foundation-design.md` | Firmware directory tiers (core/board/ui), host tests, fault handling, toolchain pin | current |
| `2026-10-08-pitch-cpu-pass-design.md` | Pitch stage CPU pass: tracker gating, warm buffers, spinning window, block-rate gains | current |
| `2026-10-08-cpu-pass-2-design.md` | CPU pass 2: Unison, Distortion, DTCM placement, pane tracker, PSOLA weights, 75 % / 95 % budget rule | done 2026-10-08 with the bench numbers and the measured cost table; owner listening done 2026-10-09 (both pairs fine) |
| `2026-10-08-cpu-pass-3-design.md` | CPU pass 3: Distortion restore after the D3 regression, Spring B disperser pair, Unison voice interleave, restrict/locals, agent worktree merge rule | done 2026-10-09; chain 54.7 % / 64.2 %, default layout sums 61 / 72 |
| `2026-10-11-font-sizes-design.md` | OLED fonts: Stylized Hyperlegible 5x7, 9X12 and 16X24 (owner 2026-10-11), exported to `tools/font-editor/fonts/cubevox_fonts.h` | current |
| `2026-10-12-layout-record-design.md` | Layout record in flash bank 2: schema v1 and v2, append log, ECC recovery | current |
| `2026-10-12-firmware-structure-design.md` | Firmware module map and ownership rules (snapshot, pure menu, canvas); new firmware follows it | current |
| `2026-10-12-serial-protocol-design.md` | USB JSON protocol, live layout apply with a 50 ms fade | current |
| `2026-10-15-mac-app-design.md` | Mac app: one page, slots and a card bank, headroom meter, one Update button | app parked (owner 2026-10-24); box side of Update superseded by the field-update spec |
| `2026-10-24-field-update-design.md` | Field update: TinyUF2 bootloader in sector 0, app at 0x08020000 with a CRC header, shared boot block, Adam drags a `.uf2` onto the `CUBEVOX` drive; `firmware/platform/` absorbed into `board/` | current; host side built and on `main` 2026-10-24, screens approved; shop steps and drills wait for a board |
| `2026-10-24-polish-design.md` | Polish tail: POLISH COMP and POLISH EQ close the chain, TOP SWITCH is MUTE or BYPASS, the final seven-row menu, layout record schema v2 and the protocol `settings` object | built, bench pending |
| `2026-10-24-oled-gui-design.md` | OLED runtime screens: the voice ribbon, the knob readout with rolling digits in a fixed field per parameter (no re-centring), MUTED or BYPASS, the 2-frame dissolve, the burn-in walk, and the `level`, `gui`, `screens` and `face` modules; settles the `ui/screens` split the structure spec deferred | host part built 2026-10-24, proofs 3 to 10 approved with the fixed field, bench pending |
| `2026-10-24-solder-jig-design.md` | Printed jig for hand-soldering the four underside jacks; `solder-jig` component in the Fusion enclosure document | current |

Research notes live in `docs/audits/`. `2026-10-08-pitch-tracker-research.md` is the note behind
the tracker plan.
