// Builds the one value the audio interrupt reads per block.

#pragma once

#include <stdint.h>

#include <array>

#include "core/chain_params.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/settings.h"
#include "core/slots.h"

// A knob pin of kNoPin leaves the knob to the panel or to pin-at-top.
constexpr uint8_t kNoPin = 0xFF;

using KnobPins = std::array<std::array<uint8_t, kKnobsPerCard>, kSlotCount>;

constexpr KnobPins noKnobPins() {
  KnobPins pins{};
  for (auto& slot : pins) {
    for (uint8_t& pin : slot) pin = kNoPin;
  }
  return pins;
}

struct BenchOverrides {
  uint8_t slotsOff = 0;        // bit s forces slot s off, and wins over slotsForcedOn
  uint8_t slotsForcedOn = 0;   // bit s turns slot s on whatever its panel toggle reads
  bool reverbChasm = false;
  bool bypassOn = false;       // BYPASS held on, whatever the top switch reads
  bool compOff = false;        // POLISH COMP forced off, whatever the menu says
  bool eqOff = false;          // VOX EQ forced off, whatever the menu says
  bool knobsPinned = false;    // every knob at its top position
  KnobPins knobPin = noKnobPins();  // whole percent of travel per knob; wins over knobsPinned

  // Pins one knob, slot and knob counted from 0, at `percent` (0..100) of its travel.
  void pinKnob(int slot, int knob, int percent) {
    knobPin[static_cast<size_t>(slot)][static_cast<size_t>(knob)] = static_cast<uint8_t>(percent);
  }

  // Clears both force masks, so the panel toggles rule again.
  void slotsLive() {
    slotsOff = 0;
    slotsForcedOn = 0;
  }

  // Clears every per-knob pin and the pin-at-top, so the panel knobs rule again.
  void knobsLive() {
    knobsPinned = false;
    knobPin = noKnobPins();
  }
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
  // Each slot's toggle as the audio interrupt crossfades it, after the bench's force-on and
  // force-off masks.
  std::array<bool, kSlotCount> slotOn = allSlotsOn();
};

ChainSnapshot composeSnapshot(const Layout& layout, const PanelReading& panel, const Settings& settings,
                              const BenchOverrides& bench);

// The same snapshot built in `out`, which the caller owns. A snapshot is over a kilobyte, so a caller
// on the firmware's shared stack builds it here and keeps the object off its own frame. `out` takes
// the default state first, so a reused object keeps nothing from the layout before.
void composeSnapshotInto(ChainSnapshot& out, const Layout& layout, const PanelReading& panel, const Settings& settings,
                         const BenchOverrides& bench);
