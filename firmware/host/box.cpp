#include "box.h"

#include "core/timing.h"
#include "core/version.h"
#include "ui/actions.h"
#include "ui/gui.h"

namespace host {

PanelInput::PanelInput() {
  knob.fill(kMidTravel);
  toggle.fill(true);
}

void HostDisplay::flushStep(Canvas& canvas) {
  const int p = canvas.firstDirtyPage();
  if (p < 0) return;
  const uint8_t* page = canvas.page(p);
  for (int x = 0; x < kCanvasWidth; ++x) {
    for (int bit = 0; bit < kDisplayRowPx; ++bit) {
      pixels_[static_cast<size_t>((p * kDisplayRowPx + bit) * kCanvasWidth + x)] = (page[x] >> bit) & 1;
    }
  }
  canvas.clearDirty(p);
}

Box::Box(AudioBlock& audio, uint32_t nowMs) : audio_(audio), nowMs_(nowMs) {
  // The firmware keeps the relay open for kRelayCloseAfterPowerOnMs after power-on so the output
  // capacitor can settle. A Mac has no capacitor, so the sequence starts as if power came on that
  // long ago and the output releases within a few ticks.
  mute_.reset(true, nowMs - kRelayCloseAfterPowerOnMs);
  bypassSwitch_.reset(true);
  splash_.start(nowMs);
  dim_.start(nowMs);
}

void Box::audioStarted() {
  mute_.clocksRunning();
  audioRunning_ = true;
}

PanelReading Box::reading() const {
  PanelReading r;
  for (int s = 0; s < kSlotCount; ++s) {
    for (int k = 0; k < kKnobsPerCard; ++k) r.knob[s][k] = knobs_.value(s * kKnobsPerCard + k);
    r.toggle[s] = toggle_[s];
  }
  r.lastMoved = knobs_.lastMoved();
  return r;
}

void Box::publishSnapshot() { audio_.publish(composeSnapshot(layout_, reading(), settings_, BenchOverrides())); }

// The knobs go through the firmware's KnobBank with the step counts of the cards now in the
// slots, and the toggles raise a ToggleFlip when they change.
void Box::readControls(const PanelInput& in, uint32_t nowMs) {
  std::array<int, kPotCount> steps{};
  for (int pot = 0; pot < kPotCount; ++pot) {
    steps[pot] = knobSteps(layout_.card[pot / kKnobsPerCard], pot % kKnobsPerCard);
  }
  knobs_.setSteps(steps);
  for (int pot = 0; pot < kPotCount; ++pot) {
    const LastMoved owner{static_cast<int8_t>(pot / kKnobsPerCard), pot % kKnobsPerCard == 1};
    knobs_.update(pot, in.knob[pot], owner, nowMs);
  }
  for (int s = 0; s < kSlotCount; ++s) {
    if (toggleSeen_ && in.toggle[s] != toggle_[s]) {
      toggleChanged_ = true;
      flip_ = ToggleFlip{static_cast<int8_t>(s), in.toggle[s]};
    }
    toggle_[s] = in.toggle[s];
  }
  toggleSeen_ = true;
}

// The board's mutePoll: the MUTE toggle is MUTE_SW (released when off), debounced for the BYPASS role.
void Box::runMute(const PanelInput& in, uint32_t nowMs) {
  const bool released = !in.mute;
  bypassSwitch_.update(released, nowMs, kSwitchDebounceMs);
  bypassOn_ = settings_.topSwitch == TopSwitch::Bypass && !bypassSwitch_.level();
  mute_.hold(!knobs_.seeded());  // unseeded knobs read 0, so stay muted
  mute_.update(nowMs, settings_.topSwitch == TopSwitch::Mute ? released : true);
  audio_.setOutputHeld(mute_.outputHeld());
  audio_.setBypassOn(bypassOn_);
}

// Applies what the menu asked for. Returns true when a setting or the layout changed.
bool Box::applyAction(UiAction action) {
  switch (ui::applyAction(action, settings_, layout_)) {
    case ui::ApplyResult::SettingsChanged: return true;
    case ui::ApplyResult::LayoutChanged:
      publish_.markStale();
      return true;
    case ui::ApplyResult::EnterDfu:
      // There is no bootloader to reset into, so the menu closes and the face repaints.
      menu_ = Menu();
      face_.invalidate();
      break;
    case ui::ApplyResult::None: break;
  }
  return false;
}

void Box::swapOctaveFormant() { applyAction(UiAction::SwapOctaveFormant); }

void Box::swapUnisonMob() { applyAction(UiAction::SwapUnisonCrowd); }

void Box::swapDistortionGrowl() { applyAction(UiAction::SwapDistortionGrowl); }

void Box::swapReverb() { applyAction(UiAction::SwapReverb); }

void Box::pickSlot3Card(Card card) { pickCycledCard(card, UiAction::SwapOctaveFormant, octaveFormantSlot); }

void Box::pickSlot4Card(Card card) { pickCycledCard(card, UiAction::SwapUnisonCrowd, unisonCrowdSlot); }

void Box::pickSlot6Card(Card card) { pickCycledCard(card, UiAction::SwapDistortionGrowl, distortionGrowlSlot); }

void Box::pickSlot8Card(Card card) { pickCycledCard(card, UiAction::SwapReverb, reverbSlot); }

void Box::pickSlot7Card(Card card) { pickCycledCard(card, UiAction::CycleSlot7Card, slot7CardSlot); }

// Each cycle visits at most three cards, so two steps reach any card it can reach. The steps run on a
// copy, and the box takes the copy only when it holds the card.
void Box::pickCycledCard(Card card, UiAction action, int (*slotOf)(const Layout&)) {
  constexpr int kCycleSteps = 2;
  Layout trial = layout_;
  Settings scratch = settings_;
  for (int step = 0; step < kCycleSteps; ++step) {
    const int slot = slotOf(trial);
    if (slot < 0 || trial.card[static_cast<size_t>(slot)] == card) break;
    if (ui::applyAction(action, scratch, trial) != ui::ApplyResult::LayoutChanged) break;
  }
  const int slot = slotOf(trial);
  if (slot < 0 || trial.card[static_cast<size_t>(slot)] != card || trial == layout_) return;
  layout_ = trial;
  publish_.markStale();
}

// The splash owns the canvas until it ends; the face then dissolves in from what it left.
void Box::drawFace(const UiContext& ctx, uint32_t nowMs) {
  switch (splash_.draw(nowMs, kFirmwareVersion, canvas_)) {
    case SplashStep::Up: return;
    case SplashStep::Ended:
      face_.invalidate();
      splashEnded_ = true;
      break;
    case SplashStep::Off: break;
  }
  if (face_.due(nowMs, canvas_)) {
    const level::InputEnergy input = audio_.takeInputEnergy();
    dim_.noteVoice(input, nowMs);
    face_.tick(nowMs, ctx, input, audio_.takeOutputEnergy(), menu_, canvas_);
  }
}

void Box::tick(const PanelInput& in, uint32_t nowMs) {
  nowMs_ = nowMs;
  readControls(in, nowMs);
  runMute(in, nowMs);

  const UiInput menuIn{in.detents, in.click, in.hold};
  if (menuIn.detents != 0 || menuIn.click || menuIn.hold) dim_.noteActivity(nowMs);
  const bool settingsChanged = applyAction(menu_.update(menuIn, nowMs));
  const bool changed = knobs_.takeChanged() || toggleChanged_;
  toggleChanged_ = false;
  if (publish_.take(knobs_.seeded(), changed || settingsChanged)) publishSnapshot();

  const bool switchOn = topSwitchEngaged(settings_.topSwitch, mute_.active(), bypassOn_);
  dim_.noteTopSwitch(switchOn, nowMs);
  const int loadPercent = settings_.debug ? cost().worst : 0;
  const PanelReading panel = reading();
  const UiContext ctx{layout_, panel, settings_, switchOn, loadPercent};
  LastMoved touch;
  if (knobs_.takeTouch(touch)) {
    dim_.noteActivity(nowMs);
    face_.knobMoved(layout_, ctx.panel, touch, nowMs);
  }
  if (flip_.slot >= 0) {
    dim_.noteActivity(nowMs);
    face_.toggleFlipped(layout_, flip_, nowMs);
    flip_ = ToggleFlip();
  }
  drawFace(ctx, nowMs);
  display_.setContrast(dim_.contrast(nowMs, settings_.brightness));

  // The board sends one page per pass and loop() passes run by the thousand each second, so a
  // frame is out long before the next tick. One host tick sends them all.
  while (canvas_.dirtyPages() != 0) display_.flushStep(canvas_);
}

}  // namespace host
