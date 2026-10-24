# Specs

One spec per stage and per subsystem. All still apply unless a later spec says it supersedes a
part. Filenames are `YYYY-MM-DD-name-design.md`. Add a row here with every new spec.

| Spec | Covers | Status |
|---|---|---|
| `2026-10-01-unison-emulator-design.md` | Unison stage, the engine contract, and the Mac prototype emulator | current; the fixed-detune shifter is out of the code |
| `2026-10-01-harmony-design.md` | Harmony stage (Zoom V3 model) | out of the code: no card, engine, test or emulator controls; key tables in `engine/scale.h` |
| `2026-10-01-harmony-engines-design.md` | Harmony shifter engines A, B, C and chromatic mode | out of the code with Harmony |
| `2026-10-01-octave-pitchfx-design.md` | Shared pitch front end (ring, tracker, crossfades) and Octave option A | front end current; Harmony and option A out of the code |
| `2026-10-01-octave-b-design.md` | Octave option B (epoch PSOLA, formant) | out of the code |
| `2026-10-01-octave-c-design.md` | Octave option C, the granular shifter the box runs | current; the only Octave engine |
| `2026-10-01-autotune-design.md` | Autotune: grid PSOLA correction to the key's major scale, KEY and RESPONSE | current; engine B, chromatic mode and the Harmony key link out of the code |
| `2026-10-01-slapback-design.md` | Slapback stage | 30..120 ms range superseded by the v2 spec (60..250 ms); INTENSITY is the MIX knob |
| `2026-10-01-distortion-design.md` | Distortion (BD-2 model), DRIVE and TONE | current; the menu bypass is the slot toggle; always 2x oversampled |
| `2026-10-01-gate-design.md` | Gate stage; the stage specs follow its shape | current |
| `2026-10-01-chasm-design.md` | CHASM reverb engine, the Chasm Reverb card | knobs superseded by the v2 spec (MIX and TIME); engine radio superseded by the registry's reverb cards |
| `2026-10-01-polish-eq-design.md` | POLISH EQ shape and defaults | current for the EQ; its place in the tail and the menu row superseded by the polish spec |
| `2026-10-01-level-rule.md` | Level rule: every stage near unity at its default; `level_test` method and trims | current |
| `2026-10-02-adam-face-design.md` | Emulator face: hardware knobs first, musician tuning sliders | current for the emulator |
| `2026-10-02-hardware-design.md` | The board | ROM-DFU items (lines 151, 180) superseded by the field-update spec; item 27 by the enclosure-closure spec |
| `2026-10-03-spring-b-design.md` | SPRING B reverb, the Spring Reverb card (`engine/spring.h`) | current |
| `2026-10-07-slots-design.md` | Eight slots of two knobs and a toggle, MUTE, the 16-pot mux | current for the panel; card placement and chain order superseded by the update-and-slots spec, the ROM-DFU update by the field-update spec |
| `2026-10-07-v2-mic-reverb-time-slapback.md` | Emulator v2: live mic, reverb TIME, slapback 60..250 ms | current |
| `2026-10-08-enclosure-closure-design.md` | Enclosure closure: dish and lid, PETG bead and TPU boot, no fasteners | current |
| `2026-10-08-update-and-slots-design.md` | Mac app, slot layout, firmware updates | page parts superseded by the Mac-app spec |
| `2026-10-08-effect-registry-design.md` | Firmware effect registry: card ids, knob descriptors, groups, costs, bench | current |
| `2026-10-08-firmware-foundation-design.md` | Firmware directory tiers (core/board/ui), host tests, fault handling, toolchain pin | current |
| `2026-10-08-pitch-cpu-pass-design.md` | Pitch stage CPU pass: tracker gating, warm buffers, spinning window, block-rate gains | current |
| `2026-10-08-cpu-pass-2-design.md` | CPU pass 2: Unison, Distortion, DTCM placement, pane tracker, PSOLA weights, 75 % / 95 % budget rule | done 2026-10-08 with the bench numbers and the measured cost table; owner listening done 2026-10-09 (both pairs fine) |
| `2026-10-08-cpu-pass-3-design.md` | CPU pass 3: Distortion restore after the D3 regression, Spring B disperser pair, Unison voice interleave, restrict/locals, agent worktree merge rule | done 2026-10-09; chain 54.7 % / 64.2 %, default layout sums 61 / 72 |
| `2026-10-11-font-sizes-design.md` | OLED fonts: Stylized Hyperlegible 5x7, 9X12 and 16X24 (owner 2026-10-11), exported to `tools/font-editor/fonts/cubevox_fonts.h` | current |
| `2026-10-12-layout-record-design.md` | Layout record in flash bank 2: schema v1, append log, ECC recovery | current |
| `2026-10-12-firmware-structure-design.md` | Firmware module map and ownership rules (snapshot, pure menu, canvas); new firmware follows it | current |
| `2026-10-12-serial-protocol-design.md` | USB JSON protocol, live layout apply with a 50 ms fade | current |
| `2026-10-15-mac-app-design.md` | Mac app: one page, slots and a card bank, headroom meter, one Update button | app parked (owner 2026-10-24); box side of Update superseded by the field-update spec |
| `2026-10-24-field-update-design.md` | Field update: TinyUF2 bootloader in sector 0, app at 0x08020000 with a CRC header, shared boot block, Adam drags a `.uf2` onto the `CUBEVOX` drive; `firmware/platform/` absorbed into `board/` | current; host side built and on `main` 2026-10-24, screens approved; shop steps and drills wait for a board |
| `2026-10-24-polish-design.md` | Polish tail: POLISH COMP and POLISH EQ close the chain, TOP SWITCH is MUTE or BYPASS, the final seven-row menu, layout record schema v2 and the protocol `settings` object | built, bench pending |
| `2026-10-24-oled-gui-design.md` | OLED runtime screens: the voice ribbon, the knob readout with rolling digits in a fixed field per parameter (no re-centring), MUTED or BYPASS, the 2-frame dissolve, the burn-in walk, and the `level`, `gui`, `screens` and `face` modules; settles the `ui/screens` split the structure spec deferred | host part built 2026-10-24, proofs 3 to 10 approved with the fixed field, bench pending |
| `2026-10-24-solder-jig-design.md` | Printed jig for hand-soldering the four underside jacks; `solder-jig` component in the Fusion enclosure document | current |

Research notes live in `docs/audits/`. `2026-10-08-pitch-tracker-research.md` is the note behind
the tracker plan.
