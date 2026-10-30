#include "ui/actions.h"

namespace ui {

namespace {

ApplyResult swapReverb(Layout& layout) {
  const int slot = reverbSlot(layout);
  if (slot < 0) return ApplyResult::None;
  Layout next = layout;
  next.card[slot] = layout.card[slot] == Card::ReverbSpring ? Card::ReverbChasm : Card::ReverbSpring;
  if (!layoutValid(next).ok()) return ApplyResult::None;
  layout = next;
  return ApplyResult::LayoutChanged;
}

}  // namespace

ApplyResult applyAction(const UiAction& action, Settings& settings, Layout& layout) {
  switch (action) {
    case UiAction::TogglePolishComp:
      settings.polishComp = !settings.polishComp;
      return ApplyResult::SettingsChanged;
    case UiAction::TogglePolishEq:
      settings.polishEq = !settings.polishEq;
      return ApplyResult::SettingsChanged;
    case UiAction::CycleTopSwitch:
      settings.topSwitch = settings.topSwitch == TopSwitch::Mute ? TopSwitch::Bypass : TopSwitch::Mute;
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
    case UiAction::SwapReverb: return swapReverb(layout);
    case UiAction::EnterDfu: return ApplyResult::EnterDfu;
    case UiAction::None: break;
  }
  return ApplyResult::None;
}

}  // namespace ui
