// Builds the one value the audio interrupt reads per block.

#pragma once

#include <stdint.h>

#include "core/chain_params.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/settings.h"

struct BenchOverrides {
  uint8_t slotsOff = 0;  // bit s forces slot s off
  bool reverbChasm = false;
  bool knobsPinned = false;  // every knob at its top position
};

struct ChainSnapshot {
  Layout layout;
  ChainParams params;
  float outputGain = 1.0f;  // linear, applied after the chain
};

ChainSnapshot composeSnapshot(const Layout& layout, const PanelReading& panel, const Settings& settings,
                              const BenchOverrides& bench);
