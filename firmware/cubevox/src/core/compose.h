// Builds the one value the audio interrupt reads per block.

#pragma once

#include <stdint.h>

#include <array>

#include "core/chain_params.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/settings.h"
#include "core/slots.h"

struct BenchOverrides {
  uint8_t slotsOff = 0;  // bit s forces slot s off
  bool reverbChasm = false;
  bool knobsPinned = false;  // every knob at its top position
};

// Every slot on, the start state of a snapshot.
constexpr std::array<bool, kSlotCount> allSlotsOn() {
  std::array<bool, kSlotCount> on{};
  for (int s = 0; s < kSlotCount; ++s) on[static_cast<size_t>(s)] = true;
  return on;
}

struct ChainSnapshot {
  Layout layout;
  ChainParams params;
  float outputGain = 1.0f;  // linear, applied after the chain
  // Each slot's toggle as the audio interrupt crossfades it, off for a slot the bench forces off.
  std::array<bool, kSlotCount> slotOn = allSlotsOn();
};

ChainSnapshot composeSnapshot(const Layout& layout, const PanelReading& panel, const Settings& settings,
                              const BenchOverrides& bench);
