# Agent brief for board work

Paste this into every routing or placement agent's first message. The agent
decides within it and reports once. It never sends a list of options back.

## Decision policy
1. Proper fix over patch. Fix the schematic, the placement or the traces, whichever is
   electrically right. Rework cost is never a reason.
2. Fewest vias that satisfy the rule. A via exists only for a layer change, a pad's
   GND via, or a GND return within 2 mm of a signal via. A via leaves with what it served.
3. Parts move when the route needs it. Any passive may move. ICs, connectors, pots,
   jacks and the H7 move only when the brief names them.
4. Geometry rules are in canon and the ledger gates. 45/90 only, straight centred pad
   entry, branch only at a pad or straight-through via, via stub >= 0.7 mm, jog =
   45-straight-45 with straight <= 3 mm, body gap >= 0.30 mm, 0.3 mm 3V3/GND,
   0.2 mm clearance. Overlap wholly inside pad copper is not a defect.
5. All parts F.Cu. Never propose a B.Cu component.

## Workflow
1. Write the board in place with the KiCad python once the permission rules allow it.
   Until then send the script path and wait for "written".
2. Verify once, at the end of the task. DRC, ledger closure, via_audit. Not after
   every edit.
3. Report once. Commit hashes, DRC counters, unconnected count, and any recorded
   exception with its coordinates. Owner-facing questions go at the end of that report.
4. If the brief does not settle a choice, pick the option with the fewest vias and
   the shortest critical net, state the assumption in one line, and continue.

## Definition of done
Required for any routing or placement task. A task that skips a step is not done.

1. Measured brief first. Before copper, write a brief file under `hardware/layout/` that
   states the rows, lanes and vias with the three measured numbers: trace pitch >= 0.4 mm,
   via centre to any other trace >= 0.6 mm, via to via >= 0.8 mm. It includes a band
   crossing check that lists every segment and via lying in each band and every vertical
   passing through it, on both layers, not only copper lying inside it.
2. Scratch first. Apply every group to a scratch copy of the board, run DRC on it and
   render it before the real apply.
3. Gates. `hardware/tools/gates.sh` prints all PASS on the saved file.
4. Corners and jogs. No right-angle corner and no jog remains anywhere the task touched
   (canon 44, 136). A direction change is two 45s with a chamfer >= 0.4 mm. No foreman
   finding is declared around. Fix it, or register an exception with the reason and the
   coordinates in `gates.sh`.
5. Renders. Write renders to `hardware/renders/` and look at them.
6. Independent check. A second agent verifies the saved board against the expected numbers
   from the brief, given those numbers up front.
7. Commit with explicit pathspecs. Do not push.
