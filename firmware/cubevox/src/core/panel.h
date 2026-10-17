// What the panel hardware reports, handed up by the board tier.

#pragma once

#include <stdint.h>

#include <array>

#include "core/slots.h"

struct LastMoved {
  int8_t slot = -1;  // -1 until a knob has moved
  bool knobB = false;
};

struct PanelReading {
  // 0..1, or the step index for a stepped knob.
  std::array<std::array<float, kKnobsPerCard>, kSlotCount> knob{};
  std::array<bool, kSlotCount> toggle{};
  LastMoved lastMoved;
};
