#include "ui/gui.h"

#include <math.h>

namespace {

// The value closes the gap to the knob with this time constant.
constexpr float kEaseTauS = 0.02f;
constexpr float kTickS = 1.0f / kUiFps;

constexpr Walk kWalkRing[8] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

}  // namespace

Walk walkAt(uint32_t step) {
  if (step == 0) return {0, 0};
  return kWalkRing[(step - 1) % 8];
}

Gui::Gui() : ease_(1.0f - expf(-kTickS / kEaseTauS)) {}

void Gui::knobMoved(int slot, int knob, float target, uint32_t nowMs) {
  const bool same = readoutLive_ && slot == slot_ && knob == knob_;
  slot_ = static_cast<uint8_t>(slot);
  knob_ = static_cast<uint8_t>(knob);
  target_ = target;
  if (!same) value_ = target;
  readoutLive_ = true;
  touchedMs_ = nowMs;
}

Gui::Id Gui::wanted(bool switchOn, bool menuOpen) const {
  if (menuOpen) return {Screen::Menu, 0, 0};
  if (readoutLive_) return {Screen::Readout, slot_, knob_};
  return {switchOn ? Screen::Muted : Screen::Voice, 0, 0};
}

// A change of screen starts a dissolve that shows step 1 at once and then each step in turn.
void Gui::advanceDissolve(Id want) {
  const bool changed = want.screen != shown_.screen || want.slot != shown_.slot || want.knob != shown_.knob;
  if (changed || force_) {
    shown_ = want;
    dissolve_ = 1;
    force_ = false;
  } else if (dissolve_ > 0) {
    dissolve_ = dissolve_ < kDissolveSteps ? dissolve_ + 1 : 0;
  }
}

GuiFrame Gui::tick(uint32_t nowMs, bool switchOn, bool menuOpen) {
  if (!started_) {
    started_ = true;
    startMs_ = nowMs;
  }
  if (readoutLive_ && nowMs - touchedMs_ >= kReadoutHoldMs) readoutLive_ = false;
  if (readoutLive_) value_ += (target_ - value_) * ease_;

  advanceDissolve(wanted(switchOn, menuOpen));

  GuiFrame frame;
  frame.screen = shown_.screen;
  frame.dissolve = dissolve_;
  frame.slot = slot_;
  frame.knob = knob_;
  frame.shown = value_;
  frame.target = target_;
  frame.walk = walkAt((nowMs - startMs_) / kWalkStepMs);
  return frame;
}
