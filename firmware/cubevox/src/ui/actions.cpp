#include "ui/actions.h"

namespace ui {

ApplyResult applyAction(const UiAction& action, Settings& settings) {
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
    case UiAction::EnterDfu: return ApplyResult::EnterDfu;
    case UiAction::None: break;
  }
  return ApplyResult::None;
}

}  // namespace ui
