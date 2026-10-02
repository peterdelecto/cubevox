# Run log
mechanical rules | no ledger (ran before this pass) | rules write: fab table, 3 netclasses, 0 custom rules; facts minima clearance 0.15, track 0.15, via 0.6/0.3, annular 0.15, hole clearance 0.25, edge 0.5 mm.
mechanical register | P0001-P0029 | register init seeded 29 pending entries from classification.json (brief sha 24c1c9e0).
mechanical fixed parts | E0001-E0036 | 36 decided entries, 36 do_not_move courtyard reservations (J1/J101/J106/J108 cite canon:37), board sha aa7361d3 unchanged. Edge-mount dry read for J1/J101/J106 computed x 148.5 (J1 +23.11, J101 -41.5, J106 -83.5 mm from the owner pose), J108 refused (axis +x vs extents 13.57/13.65); nothing applied.
mechanical gates | ledger 0012 (audit) | courtyard_inside_outline PASS 222 checked; edge_connector_mouth_at_edge not measured (J101, J108 no .wrl; J1 +1.527, J106 +3.700 mm); model_inside_courtyard FAIL J106 (right 3.4, below_board 11.0 mm). Gate rows carry no ledger id.
mechanical phase | state.json | phase placement moved from intake; only gate 1 read (state pass intake folds to purpose), mechanical rows were not part of the leaving gates.
