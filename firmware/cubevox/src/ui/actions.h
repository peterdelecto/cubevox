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

// Applies `action` to `settings` and `layout`. Each toggle flips its setting, and the top switch and
// the brightness cycle through their values.
// The reverb action swaps the reverb card between Spring and Chasm in its slot. It changes nothing
// when no slot holds a reverb card, or when the other card would push the layout past the CPU budget.
// The Unison and Crowd action does the same between Unison and Crowd in the slot that holds either,
// and changes nothing when no slot does or when the other card fails the placement rules.
// The Octave and Formant action does the same between Octave and Formant, and the slot 6 action
// between Distortion and Growl.
// The slot 7 action moves the slot that holds Gate, Lo-fi or Chatter to the next of the three in that
// order, wrapping, and passes over a card the placement rules refuse. It changes nothing when no slot
// holds one of the three or when both others are refused.
ApplyResult applyAction(const UiAction& action, Settings& settings, Layout& layout);

}  // namespace ui
