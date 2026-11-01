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

ApplyResult swapUnisonCrowd(Layout& layout) {
  const int slot = unisonCrowdSlot(layout);
  if (slot < 0) return ApplyResult::None;
  Layout next = layout;
  next.card[slot] = layout.card[slot] == Card::Unison ? Card::Crowd : Card::Unison;
  if (!layoutValid(next).ok()) return ApplyResult::None;
  layout = next;
  return ApplyResult::LayoutChanged;
}

ApplyResult swapOctaveFormant(Layout& layout) {
  const int slot = octaveFormantSlot(layout);
  if (slot < 0) return ApplyResult::None;
  Layout next = layout;
  next.card[slot] = layout.card[slot] == Card::Octave ? Card::Formant : Card::Octave;
  if (!layoutValid(next).ok()) return ApplyResult::None;
  layout = next;
  return ApplyResult::LayoutChanged;
}

// The card after `c` in the slot 7 cycle, Gate to Lo-fi to Bloom and back to Gate.
Card nextSlot7Card(Card c) {
  switch (c) {
    case Card::Gate: return Card::Lofi;
    case Card::Lofi: return Card::Bloom;
    default: return Card::Gate;
  }
}

// Moves the slot that holds one of the three to the next card in the cycle that layoutValid accepts,
// so a card over the CPU lines or already placed in another slot is passed over. When neither of the
// other two fits the layout stays as it is.
ApplyResult cycleSlot7Card(Layout& layout) {
  const int slot = slot7CardSlot(layout);
  if (slot < 0) return ApplyResult::None;
  Layout next = layout;
  for (Card c = nextSlot7Card(layout.card[slot]); c != layout.card[slot]; c = nextSlot7Card(c)) {
    next.card[slot] = c;
    if (layoutValid(next).ok()) {
      layout = next;
      return ApplyResult::LayoutChanged;
    }
  }
  return ApplyResult::None;
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
    case UiAction::SwapOctaveFormant: return swapOctaveFormant(layout);
    case UiAction::SwapUnisonCrowd: return swapUnisonCrowd(layout);
    case UiAction::CycleSlot7Card: return cycleSlot7Card(layout);
    case UiAction::EnterDfu: return ApplyResult::EnterDfu;
    case UiAction::None: break;
  }
  return ApplyResult::None;
}

}  // namespace ui
