#include "core/compose.h"

#include <math.h>

#include "core/registry.h"

namespace {

constexpr float kDbPerDecade = 20.0f;

float topPosition(Card c, int knob) {
  const int steps = knobSteps(c, knob);
  return steps > 0 ? static_cast<float>(steps - 1) : 1.0f;
}

void applySlot(const Layout& layout, const PanelReading& panel, const BenchOverrides& bench, int slot,
               ChainParams& out) {
  const Card c = layout.card[slot];
  applyPlacement(c, out);
  for (int k = 0; k < kKnobsPerCard; ++k) {
    applyKnob(c, k, bench.knobsPinned ? topPosition(c, k) : panel.knob[slot][k], out);
  }
  const bool forcedOff = (bench.slotsOff >> slot) & 1u;
  applyToggle(c, panel.toggle[slot] && !forcedOff, out);
}

}  // namespace

ChainSnapshot composeSnapshot(const Layout& layout, const PanelReading& panel, const Settings& settings,
                              const BenchOverrides& bench) {
  ChainSnapshot snap;
  snap.layout = layout;
  for (int s = 0; s < kSlotCount; ++s) applySlot(layout, panel, bench, s, snap.params);
  if (bench.reverbChasm) snap.params.reverb.engine = cv::kReverbChasm;
  snap.params.comp.on = settings.polishComp;
  snap.params.eq.on = settings.polishEq;
  snap.outputGain = powf(10.0f, outputTrimDb(settings) / kDbPerDecade);
  return snap;
}
