// Which card sits in each of the eight slots. The chain order is the slot order.

#pragma once

#include <stddef.h>

#include <array>

#include "core/chain_params.h"
#include "core/registry.h"

struct Layout {
  std::array<Card, kSlotCount> card;
};

inline bool operator==(const Layout& a, const Layout& b) { return a.card == b.card; }
inline bool operator!=(const Layout& a, const Layout& b) { return !(a == b); }

// Slot 1 to 8 as shipped (CLAUDE.md "Default layout").
Layout defaultLayout();

enum class LayoutError : uint8_t { None, BadCard, SameCardTwice, TwoReverbs, OverAvg, OverWorst };

// Outcome of layoutValid. The fields beside `error` say which slot, card or percentage broke the rule.
struct LayoutCheck {
  LayoutError error = LayoutError::None;
  int slot = 0;      // BadCard: zero-based slot holding the out-of-range id
  Card card = Card::Empty;  // SameCardTwice: the repeated card
  int percent = 0;   // OverAvg, OverWorst: the layout's sum, including the fixed term

  bool ok() const { return error == LayoutError::None; }
};

// Placement rules of the registry spec: ids in range, no card twice (Empty excepted), at most one reverb card, and the
// placed costs plus the fixed term under the average and worst-block lines.
LayoutCheck layoutValid(const Layout& layout);

// Short readable reason for a failed check, for example "two reverb cards". Empty string when ok.
void layoutReason(const LayoutCheck& check, char* buf, size_t size);
