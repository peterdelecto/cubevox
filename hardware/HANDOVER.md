# cubevox board hand-over (2026-10-03)

Process rules are in the owner's memory files and `~/.claude/CLAUDE.md`. Short form: every board change goes
declare → edit → closure → accept with a ledger id, reasons are `canon:NN` only, owner-placed panel parts carry
do_not_move locks in `layout/register.jsonl`, every PNG goes to `renders/`, build steps go to Sonnet, design
calls stay with the lead. Driver script for placement moves: `/tmp/cvx_cluster.sh NAME "canon:NN,canon:NN" REF:X,Y,ROT ...`
(recreate from RUNLOG if /tmp was cleared). Foreman: `PYTHONPATH=/Users/j/Documents/Claude/Projects/ClaudeRouter
$KICAD_PY -m foreman <cmd>` from `hardware/`; filter stderr for swig/wxApp/assert/Debug/Fontconfig.

## State at hand-over

| Item | State |
|---|---|
| Placement | 42 clusters placed. OLED/ENC101 5 mm north (D0085, ledger 0110). RVGAIN1 y 65, SWBYPASS1 y 62 (D0088, 0114). Relay group 3 mm east (D0090, 0118). J1 0.05 mm inward (D0087, 0113). |
| Routed copper | Crystal, SAI stubs, VCAP, buck SW/BST, USB pair, all power rails, OLED 3V3 rebuild (ledgers 0089–0111). |
| DRC | 3 errors before the relay rebuild (J101 peg pads) plus the relay-move fallout being rebuilt now. |
| Open declaration | **D0091** (J101 null move, expect drc errors down). Owner runs the pcbnew one-liner that sets J101's three unnumbered PTH pads to NPTH after the router agent reports, then: closure D0091 → accept → re-add J101 lock (old lock E0030 retired, box from footprint courtyard on B.Cu, decision like E0096). Library footprint `cubevox.pretty/CONN-TH_NCJ6FA-H.kicad_mod` already changed (uncommitted, commit with D0091 records together with `layout/register.jsonl`). |
| Router agent | Teammate `router-critical` (Sonnet): relay-corner power rebuild (`layout/actions/power-relay-move.json`), then 51 pad-entry repairs in groups (3V3, 5V, VDDA, 3V3A, ADC_VCC, 5VA, 9V_JACK, VCAP, buck, SAI U1.1, USB R6.1, crystal last). Checker: `tools/pad_entry_check.py` (target 0). It reports ledger ids, closure numbers and any pad it could not square. |
| ClaudeRouter edits | `canon.md` rule 132 (straight pad entry) and `foreman/canon.py` (MAX_RULE_ID 132, PASSES + "schematic") uncommitted, owner's call to commit. Teammate `foreman-owner-move` (Sonnet) is adding `foreman declare --owner` so an owner move retires and re-adds its locks in one step; it reports a diff and test result. |
| Brief | confirmed sha 0ac6ed47; classification USB limit 60 mm. |
| Pending owner input | Adam may cut the effect columns from 8 to 6; hold pot/toggle nets in the remaining pass until confirmed. Codex pin-11 one-liner answer still outstanding. |

## Next steps in order
1. Relay rebuild + pad-entry repairs land (agent). Relay 3V3/5V closure targets: shorting 0, unconnected ≤ 379.
2. J101 NPTH (owner command) → close/accept D0091 → re-add lock → render → RUNLOG → commit records. DRC should read 0.
3. Check the display-to-codec section box reading (place ledger said −16.4 mm overlap of section boxes, not parts) against the pre-move board.
4. Remaining pass: MCU, codec, USB, display nets first; pot/toggle nets after Adam confirms the column count. 45° only, straight pad entry (canon 132), budgets from the brief.
5. Ground pass (pours, stitching, C45 via 2.25 vs 2.0 budget, dangling GND vias), owner stop with render + Haiku read.
6. Audit, diode polarity silk, Gerbers/BOM/CPL for JLC.

## Update 2026-10-03 (later)
1. J101 NPTH done: D0106 (D0091 retired, stale baseline), ledger 0134, lock E0099, DRC errors 0. Commits 4ad30f3, d8f2676; pushed.
2. Owner cleanup of 3V3A/5VA/VREF recorded as D0108 (f3a40f9). pcbnew GUI saves drop `foreman.pour_*` board properties; restore them after any owner save.
3. D0109 (16fc6c7): R131/R133 pull-up via hops replaced by F.Cu links; C4/U1 pins 6+11 rebuilt (one feed via at 94.35,93.72); pin 100 group and C7/SWBOOT1 fed from the In2 trunks.
4. `foreman declare --owner` committed in ClaudeRouter 0343a25 (not pushed).
5. New canon 133 (tree routing) and 134 (surface before via), gates in foreman 0.2.30. Ledger 0153: 8 loops, 8 avoidable vias.
6. Router agent `router-critical` is clearing gates 133/134 to 0 on power nets (areas: C32/C6/C31/C59 row, C5/C61, then all loops). Verify its report against ledger gates, unconnected and DRC.
7. Open: D2 USB ESD flow-through pads unlinked (DP 1-6, DM 3-4) — lead designs. D104 nudge off J108 (owner chose B). Pin 11: Codex PASS only if pin 6 has its own cap; owner to pick A (move C7) or B (add 100 nF). Adam column count pending.
8. Decision 2026-10-03: 3V3 decoupling moves to a power-island stack. In1 solid GND; In2 power islands per section (3V3 digital/MCU, 3V3A analog, 5VA if warranted); each decoupler its own via to its island; GND legs own vias to In1. Rail traces on In2 come out. No B.Cu signal crosses an island split (USB pair at 94-103,71-72 on B.Cu must stay over one island or move to F.Cu). Rework cost is not a factor (owner); re-place parts if the island plan needs it. Next: lead drafts the island plan + render for owner approval, then router agent builds. router-critical is on hold except gate 45 corners and the 3V3A/9V_JACK pad-edge vias.

## Update 2026-10-03 (evening, cubevox lead)
1. The owner stopped the ClaudeRouter session that built the island plan (D0134-D0159). The cubevox session owns all board work from here (memory `cubevox-board-owner`). router-critical is stood down.
2. Checked after the handover: DRC errors 0, unconnected 169 (signal nets), but the USB pair was broken at D2 and J1's VBUS pins were split. Fixed in D0160.
3. Codec bus routed: D0161-D0163 (ledgers 0193-0195). Rules learned from the new foreman gates, all now in the plan files `layout/actions/codec-bus*.json` and `lrclk-u8-turn.json`:
   a. A via may not be a 90 degree junction; chain multi-pad nets through pads (BCLK through U8.8, LRCLK through U8.7).
   b. A via stub must be at least 0.7 mm before any corner (edit chamfers 0.4 mm minimum on 0.2 mm tracks, and gate 132 flags a chamfer start inside the via).
   c. A short straight between two 45 degree bends is a jog, even on a U-turn; use one long leg.
   d. Every signal via needs a ground via within 2.0 mm (gates 17, 20).
   e. `remove_net_tracks` also removes the net's vias; pairing it with `remove_via` on the same net segfaults foreman (B187). List segments individually instead.
4. Foreman defects filed in ClaudeRouter docs/QUEUE.md B187-B190 (edit segfault, gate 28 antipads, gate 49 stitch vias, mitre clamp vs gate 132). Gate 63/71 readings on trace-only rails and TH pads are B179/B185.
5. Gates at ledger 0195: 17, 45, 133, 134 pass; 63 27 legs (trace-only rails and diodes, B179/B185 class); 71 6 pins (U1.6 2.25, U1.27 2.54, U8.4 2.28, U6.8 2.57 all pin > cap > via; U3.1 is a sense pin; U4.5 is the LDO output into a trace); 136 14 (none on the bus nets); 49 7 (six stitch vias, B189, plus INA_O1 dangling at 231.0,90.8 from the owner's hand routing).
6. Next: VREF B.Cu split crossing, D0134 still open, the remaining 159 unconnected (MCU local nets, codec analog, front end, then the panel after Adam's column count), geometry cleanup of the owner's front-end routing (gate 132: 98 ends, gate 136: 14), ground pass, power-up order of 3V3 vs 3V3A (owner decision pending the STM32H7 injection limit).

## Update 2026-10-03 (night, cubevox lead)
1. MCU pin study landed (D0186, D0194). Map, reasons and the firmware delta are in `PINMAP.md` section 2; render `renders/cubevox-mcu-pinmap.png`.
2. Routing consequences the pass must honour:
   a. VA_SENSE crosses TOGGLE6-8 and XSMT once on B.Cu near x 110, over the 3V3 island.
   b. BOOT0 and NRST reach SWBOOT1, SWRESET1 and J3 on B.Cu.
   c. The west group runs between J3 and R3/D1; R3/D1 and R2 may need nudges.
3. Pots RK09D1130C2P and encoders EC11E landed (D0184, c1aec73). Shafts stand 5 mm taller than the enclosure drawing.
4. Next: MCU local nets, then the panel after Adam's column count, then the ground pass.
