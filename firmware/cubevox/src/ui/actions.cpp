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

// The card after `c` in the slot 4 cycle, Unison to Mob and back to Unison.
Card nextSlot4Card(Card c) { return c == Card::Unison ? Card::Crowd : Card::Unison; }

// The card after `c` in the slot 3 cycle, Octave to Formant to the Formant follower and back to Octave.
Card nextSlot3Card(Card c) {
  switch (c) {
    case Card::Octave: return Card::Formant;
    case Card::Formant: return Card::FormantFollower;
    default: return Card::Octave;
  }
}

// The card after `c` in the slot 6 cycle, Distortion to Growl and back to Distortion.
Card nextSlot6Card(Card c) { return c == Card::Distortion ? Card::Growl : Card::Distortion; }

// The card after `c` in the slot 7 cycle, Gate to Lo-fi to Chatter and back to Gate.
Card nextSlot7Card(Card c) {
  switch (c) {
    case Card::Gate: return Card::Lofi;
    case Card::Lofi: return Card::Bloom;
    default: return Card::Gate;
  }
}

// Moves the card in `slot` to the next card in its cycle that layoutValid accepts, so a card over the
// CPU lines or already placed in another slot is passed over. When none of the others fits the layout
// stays as it is.
ApplyResult cycleCard(Layout& layout, int slot, Card (*next)(Card)) {
  if (slot < 0) return ApplyResult::None;
  Layout trial = layout;
  for (Card c = next(layout.card[slot]); c != layout.card[slot]; c = next(c)) {
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
    case UiAction::TogglePolishComp:
      settings.polishComp = !settings.polishComp;
      return ApplyResult::SettingsChanged;
    case UiAction::TogglePolishEq:
      settings.polishEq = !settings.polishEq;
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
    case UiAction::SwapReverb: return swapReverb(layout);
    case UiAction::SwapOctaveFormant: return cycleCard(layout, octaveFormantSlot(layout), nextSlot3Card);
    case UiAction::SwapUnisonCrowd: return cycleCard(layout, unisonCrowdSlot(layout), nextSlot4Card);
    case UiAction::SwapDistortionGrowl: return cycleCard(layout, distortionGrowlSlot(layout), nextSlot6Card);
    case UiAction::CycleSlot7Card: return cycleCard(layout, slot7CardSlot(layout), nextSlot7Card);
    case UiAction::EnterDfu: return ApplyResult::EnterDfu;
    case UiAction::None: break;
  }
  return ApplyResult::None;
}

}  // namespace ui
