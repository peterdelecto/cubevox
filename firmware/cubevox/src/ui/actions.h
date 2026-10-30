// What the sketch does with a menu action. Pure, so the policy is testable on the host.

#pragma once

#include <stdint.h>

#include "core/layout.h"
#include "core/settings.h"
#include "ui/menu.h"

namespace ui {

enum class ApplyResult : uint8_t {
  None,             // nothing to do
  SettingsChanged,  // a setting flipped, so the snapshot and the saved record need updating
  LayoutChanged,    // the layout changed, so the snapshot and the saved record need updating and the audio fades
  EnterDfu,         // nothing changed; the caller mutes and resets into the bootloader
};

// Applies `action` to `settings` and `layout`. Each toggle flips its setting, and the top switch cycles.
// The reverb action swaps the reverb card between Spring and Chasm in its slot. It changes nothing
// when no slot holds a reverb card, or when the other card would push the layout past the CPU budget.
ApplyResult applyAction(const UiAction& action, Settings& settings, Layout& layout);

}  // namespace ui
