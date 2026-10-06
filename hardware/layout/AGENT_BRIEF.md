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
