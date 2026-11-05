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

float knobPosition(const Layout& layout, const PanelReading& panel, const BenchOverrides& bench, int slot, int knob) {
  const Card c = layout.card[slot];
  const int pin = bench.knobPin[static_cast<size_t>(slot)][static_cast<size_t>(knob)];
  if (pin != kNoPin) return pinnedPosition(c, knob, pin);
  return bench.knobsPinned ? topPosition(c, knob) : panel.knob[slot][knob];
}

void applySlot(const Layout& layout, const PanelReading& panel, const BenchOverrides& bench, int slot,
               ChainSnapshot& snap) {
  const Card c = layout.card[slot];
  ChainParams& out = snap.params;
  applyPlacement(c, out);
  for (int k = 0; k < kKnobsPerCard; ++k) applyKnob(c, k, knobPosition(layout, panel, bench, slot, k), out);
  const bool forcedOn = (bench.slotsForcedOn >> slot) & 1u;
  const bool forcedOff = (bench.slotsOff >> slot) & 1u;
  const bool on = (panel.toggle[slot] || forcedOn) && !forcedOff;
  applyToggle(c, on, out);
  snap.slotOn[static_cast<size_t>(slot)] = on;
}

// Fills a snapshot that holds its default state.
void fill(ChainSnapshot& snap, const Layout& layout, const PanelReading& panel, const Settings& settings,
          const BenchOverrides& bench) {
  snap.layout = layout;
  for (int s = 0; s < kSlotCount; ++s) applySlot(layout, panel, bench, s, snap);
  if (bench.reverbChasm) snap.params.reverb.engine = cv::kReverbChasm;
  snap.params.comp.on = settings.polishComp && !bench.compOff;
  snap.params.eq.on = settings.voxEq != VoxEq::Off && !bench.eqOff;
  snap.params.eq.cuts = settings.voxEq == VoxEq::Cuts;
  snap.outputGain = cv::dbToGain(outputTrimDb(settings));
}

}  // namespace

ChainSnapshot composeSnapshot(const Layout& layout, const PanelReading& panel, const Settings& settings,
                              const BenchOverrides& bench) {
  ChainSnapshot snap;
  fill(snap, layout, panel, settings, bench);
  return snap;
}

void composeSnapshotInto(ChainSnapshot& out, const Layout& layout, const PanelReading& panel, const Settings& settings,
                         const BenchOverrides& bench) {
  new (&out) ChainSnapshot();
  fill(out, layout, panel, settings, bench);
}
