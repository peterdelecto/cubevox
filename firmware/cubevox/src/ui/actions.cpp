#include "ui/actions.h"

namespace ui {

namespace {

// The CLEANUP EQ setting after `v`, OFF to LIGHT to STRONG and back to OFF.
CleanupEq nextCleanupEq(CleanupEq v) {
  switch (v) {
    case CleanupEq::Off: return CleanupEq::Light;
    case CleanupEq::Light: return CleanupEq::Strong;
    default: return CleanupEq::Off;
  }
}

// Moves the card of swappable slot `which` to the next card in its cycle that layoutValid accepts, so
// a card over the CPU lines or already placed in another slot is passed over. When none of the others
// fits the layout stays as it is.
ApplyResult cycleCard(Layout& layout, SwapSlot which) {
  const int slot = swapSlotOf(layout, which);
  if (slot < 0) return ApplyResult::None;
  Layout trial = layout;
  for (Card c = nextSwapCard(which, layout.card[slot]); c != layout.card[slot]; c = nextSwapCard(which, c)) {
    trial.card[slot] = c;
    if (layoutValid(trial).ok()) {
      layout = trial;
      return ApplyResult::LayoutChanged;
    }
  }
  return ApplyResult::None;
}

}  // namespace

ApplyResult applyAction(const UiAction& action, Settings& settings, Layout& layout) {
  switch (action) {
    case UiAction::CycleCleanupEq:
      settings.cleanupEq = nextCleanupEq(settings.cleanupEq);
      return ApplyResult::SettingsChanged;
    case UiAction::CycleTopSwitch:
      settings.topSwitch = settings.topSwitch == TopSwitch::Mute ? TopSwitch::Bypass : TopSwitch::Mute;
      return ApplyResult::SettingsChanged;
    case UiAction::CycleBrightness:
      settings.brightness = nextBrightness(settings.brightness);
      return ApplyResult::SettingsChanged;
    case UiAction::ToggleOutput:
      settings.outputLevel = settings.outputLevel == OutputLevel::Line ? OutputLevel::Mic : OutputLevel::Line;
      return ApplyResult::SettingsChanged;
    case UiAction::ToggleInputJack:
      settings.inputJack = settings.inputJack == InputJack::Mic ? InputJack::Line : InputJack::Mic;
      return ApplyResult::SettingsChanged;
    case UiAction::ToggleDebug:
      settings.debug = !settings.debug;
      return ApplyResult::SettingsChanged;
    case UiAction::SwapReverb: return cycleCard(layout, SwapSlot::Reverb);
    case UiAction::SwapOctaveFormant: return cycleCard(layout, SwapSlot::OctaveFormant);
    case UiAction::SwapUnisonCrowd: return cycleCard(layout, SwapSlot::UnisonMob);
    case UiAction::SwapDistortionGrowl: return cycleCard(layout, SwapSlot::DistortionDeepthroat);
    case UiAction::CycleSlot7Card: return cycleCard(layout, SwapSlot::Slot7);
    case UiAction::EnterDfu: return ApplyResult::EnterDfu;
    case UiAction::None: break;
  }
  return ApplyResult::None;
}

}  // namespace ui
