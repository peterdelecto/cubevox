// What the sketch does with a menu action. Pure, so the policy is testable on the host.

#pragma once

#include <stdint.h>

#include "core/settings.h"
#include "ui/menu.h"

namespace ui {

enum class ApplyResult : uint8_t {
  None,             // nothing to do
  SettingsChanged,  // a setting flipped, so the snapshot and the saved record need updating
  EnterDfu,         // nothing changed; the caller mutes and resets into the bootloader
};

// Applies `action` to `settings`. The output and input toggles flip their setting.
ApplyResult applyAction(const UiAction& action, Settings& settings);

}  // namespace ui
