#include "core/compose.h"

#include <new>

#include "core/registry.h"
#include "engine/dsp.h"

namespace {

float topPosition(Card c, int knob) {
  const int steps = knobSteps(c, knob);
  return steps > 0 ? static_cast<float>(steps - 1) : 1.0f;
}

// A stepped knob lands on round(percent / 100 * (steps - 1)), a continuous one on percent / 100.
float pinnedPosition(Card c, int knob, int percent) {
  const int steps = knobSteps(c, knob);
  if (steps > 0) return static_cast<float>((percent * (steps - 1) + 50) / 100);
  return static_cast<float>(percent) / 100.0f;
}

float slotKnobPosition(const Layout& layout, const PanelReading& panel, const BenchOverrides& bench, int slot, int knob) {
  const Card c = layout.card[slot];
  const int pin = bench.knobPin[static_cast<size_t>(slot)][static_cast<size_t>(knob)];
  if (pin != kNoPin) return pinnedPosition(c, knob, pin);
  return bench.knobsPinned ? topPosition(c, knob) : panel.knob[slot][knob];
}

void applySlot(const Layout& layout, const PanelReading& panel, const BenchOverrides& bench, bool toggleOn,
               uint8_t held, int slot, ChainSnapshot& snap) {
  const Card c = layout.card[slot];
  ChainParams& out = snap.params;
  applyPlacement(c, out);
  for (int k = 0; k < kKnobsPerCard; ++k) applyKnob(c, k, slotKnobPosition(layout, panel, bench, slot, k), out);
  // A held card stays off whatever its toggle reads.
  const bool on = toggleOn && !((held >> slot) & 1u);
  applyToggle(c, on, out);
  snap.slotOn[static_cast<size_t>(slot)] = on;
}

// Fills a snapshot that holds its default state.
void fill(ChainSnapshot& snap, const Layout& layout, const PanelReading& panel, const Settings& settings,
          const BenchOverrides& bench, uint8_t held) {
  snap.layout = layout;
  const std::array<bool, kSlotCount> toggles = slotToggles(panel, bench);
  for (int s = 0; s < kSlotCount; ++s) applySlot(layout, panel, bench, toggles[static_cast<size_t>(s)], held, s, snap);
  if (bench.reverbChasm) snap.params.reverb.engine = cv::kReverbChasm;
  snap.params.eq.on = settings.cleanupEq != CleanupEq::Off && !bench.eqOff;
  snap.params.eq.cuts = settings.cleanupEq == CleanupEq::Strong;
  snap.outputGain = cv::dbToGain(outputTrimDb(settings));
}

}  // namespace

std::array<bool, kSlotCount> slotToggles(const PanelReading& panel, const BenchOverrides& bench) {
  std::array<bool, kSlotCount> on{};
  for (int s = 0; s < kSlotCount; ++s) {
    const bool forcedOn = (bench.slotsForcedOn >> s) & 1u;
    const bool forcedOff = (bench.slotsOff >> s) & 1u;
    on[static_cast<size_t>(s)] = (panel.toggle[static_cast<size_t>(s)] || forcedOn) && !forcedOff;
  }
  return on;
}

ChainSnapshot composeSnapshot(const Layout& layout, const PanelReading& panel, const Settings& settings,
                              const BenchOverrides& bench, uint8_t held) {
  ChainSnapshot snap;
  fill(snap, layout, panel, settings, bench, held);
  return snap;
}

void composeSnapshotInto(ChainSnapshot& out, const Layout& layout, const PanelReading& panel, const Settings& settings,
                         const BenchOverrides& bench, uint8_t held) {
  new (&out) ChainSnapshot();
  fill(out, layout, panel, settings, bench, held);
}
