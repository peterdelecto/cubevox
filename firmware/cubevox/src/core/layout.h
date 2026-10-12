// Which card sits in each of the eight slots. The chain order is the slot order.

#pragma once

#include <array>

#include "core/chain_params.h"
#include "core/registry.h"

struct Layout {
  std::array<Card, kSlotCount> card;
};

// Slot 1 to 8 as shipped (CLAUDE.md "Default layout").
Layout defaultLayout();

// Placement rules of the registry spec: ids in range, at most one reverb card, and the
// placed costs plus the fixed term under the average and worst-block lines.
bool layoutValid(const Layout& layout);
