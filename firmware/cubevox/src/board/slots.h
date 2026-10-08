// Slot hardware: the eight fixed slots, each two pots on the mux and a toggle.
// Which card sits in a slot is the layout (core/registry.h), not hardware.

#pragma once

#include <stdint.h>

#include "core/chain_params.h"

constexpr int kPotCount = 2 * kSlotCount;  // POT01..POT16 on mux I0..I15

struct Slot {
  uint32_t togglePin;   // pins:: constant
  uint8_t muxChannelA;  // 0-based mux input, POTnn is channel nn - 1
  uint8_t muxChannelB;
};

extern const Slot kSlots[kSlotCount];
