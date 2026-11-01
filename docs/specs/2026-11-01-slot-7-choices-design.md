# Slot 7 choices: Gate, Formant, Lo-fi or Bloom from the menu

Owner 2026-11-01: "for number 7 gate, is there another effect we could swap into that position as
an option in case a second backup gate is overkill?" Then the Formant, Lo-fi and Bloom specs. Spec
only, not built.

## 1. Menu row

1. A row after the SLOT n row for Unison and MOB (polish spec), built the same way: an `Item`, a
   finder, a `UiAction`, `ui::applyAction`.
2. Label `SLOT n`, n the slot holding one of the four cards (`SLOT -` when none). Value `GATE`,
   `FORMANT`, `LO-FI` or `BLOOM`.
3. A click moves to the next card in that order and wraps. A card the box will not load, over the
   CPU limit, is passed over, so the row never stops on a layout that is refused.
4. The layout change takes the path every menu swap takes: the snapshot goes out stale, the 50 ms
   layout fade, the engine resets, the save.
5. Finder `slot7CardSlot(const Layout&)` in `core/layout`, the first slot holding Gate, Formant,
   Lo-fi or Bloom. Action `UiAction::CycleSlot7Card`.

## 2. Emulator

1. The card switch under slot 7 is a four-position switch, `GATE | FORMANT | LO-FI | BLOOM`, the
   same widget as UNISON | MOB, driving the menu's own action. Its position follows the layout.
2. `--check` cycles the four and asserts the layout and the cost sum after each.

## 3. CPU, every combination

Rows from `costs.h` plus the three new estimates (`kFormant{3, 4}`, `kLofi{2, 2}`, `kBloom{5, 6}`;
Bloom's from the host bench against Echo and Gate, 2026-11-01, where the spec's first estimate of
{1, 2} did not hold). Each sum holds `kFixed` and `kTracker`.

| Slot 4 / slot 8 | Gate | Formant | Lo-fi | Bloom |
|---|---|---|---|---|
| Unison / Spring | 67 / 76 | 66 / 75 | 65 / 73 | 68 / 77 |
| MOB / Spring | 72 / 84 | 71 / 83 | 70 / 81 | 73 / 85 |
| Unison / Chasm | 69 / 78 | 68 / 77 | 67 / 75 | 70 / 79 |
| MOB / Chasm | 74 / 86 | 73 / 85 | 72 / 83 | 75 / 87 |

Every cell is mean / worst %, at or under the 75 / 95 lines (`layoutValid` refuses over, not at).
MOB, Chasm and Bloom together sit exactly on the mean line, so a board figure for Bloom above its
estimate would make the row pass over Bloom in that layout. The stress run measures each new card once
built, and its row replaces the estimate.

## 4. Order of building

Formant, Lo-fi and Bloom are independent cards; each lands with its own row in `costs.h` and joins
the cycle when it lands. The row itself lands with the first of them.
