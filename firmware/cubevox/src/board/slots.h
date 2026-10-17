// Slot hardware: the eight fixed slots, each two pots on the mux and a toggle.
// Which card sits in a slot is the layout (core/registry.h), not hardware.

#pragma once

#include <stdint.h>

#include <array>

#include "board/build_kind.h"
#include "core/slots.h"

struct Slot {
  uint32_t togglePin;   // pins:: constant
  uint8_t muxChannelA;  // 0-based mux input, POTnn is channel nn - 1
  uint8_t muxChannelB;
};

extern const std::array<Slot, kSlotCount> kSlots;

#if CUBEVOX_BENCH
// Silkscreen number (1..16) of the pot wired to a 0-based mux channel.
int potNumber(int muxChannel);
#endif

// The slot knob wired to a mux channel; slot is -1 for a channel no slot uses.
struct PotOwner {
  int8_t slot;
  bool knobB;
};
PotOwner potOwner(int muxChannel);
