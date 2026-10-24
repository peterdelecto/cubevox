# Specs

One spec per stage and per subsystem. All still apply unless a later spec says it supersedes a
part. Filenames are `YYYY-MM-DD-name-design.md`. Add a row here with every new spec.

| Spec | Covers | Status |
|---|---|---|
| `2026-10-01-gate-design.md` | Gate stage; the stage specs follow its shape | current |
| `2026-10-02-hardware-design.md` | The board | ROM-DFU items (lines 151, 180) superseded by the field-update spec |
| `2026-10-03-spring-b-design.md` | SPRING B reverb | current |
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

Research notes live in `docs/audits/`. `2026-10-08-pitch-tracker-research.md` is the note behind
the tracker plan.
