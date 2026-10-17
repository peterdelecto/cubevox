#include "ui/actions.h"

namespace ui {

ApplyResult applyAction(const UiAction& action, Settings& settings) {
  switch (action) {
    case UiAction::ToggleOutput:
      settings.outputLevel = settings.outputLevel == OutputLevel::Line ? OutputLevel::Instrument : OutputLevel::Line;
      return ApplyResult::SettingsChanged;
    case UiAction::ToggleInputJack:
      settings.inputJack = settings.inputJack == InputJack::Mic ? InputJack::Pedal : InputJack::Mic;
      return ApplyResult::SettingsChanged;
    case UiAction::EnterDfu: return ApplyResult::EnterDfu;
    case UiAction::None: break;
  }
  return ApplyResult::None;
}

}  // namespace ui
