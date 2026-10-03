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
