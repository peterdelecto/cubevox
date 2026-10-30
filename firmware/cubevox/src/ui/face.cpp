#include "ui/face.h"

#include <math.h>

namespace {

// The readout of the knob the gui holds, from the card table.
ReadoutView readoutView(const GuiFrame& frame, const UiContext& ctx) {
  const Card card = ctx.layout.card[static_cast<size_t>(frame.slot)];
  ReadoutView view;
  view.slot = frame.slot;
  view.card = cardDesc(card).name;
  view.toggle = frame.toggle;
  view.on = frame.on;
  view.knob = frame.toggle ? nullptr : knobDesc(card, frame.knob);
  view.shown = frame.shown;
  view.target = static_cast<int>(lroundf(frame.target));
  return view;
}

}  // namespace

void Face::knobMoved(const Layout& layout, const PanelReading& panel, LastMoved moved, uint32_t nowMs) {
  if (moved.slot < 0 || moved.slot >= kSlotCount) return;
  const int knob = moved.knobB ? 1 : 0;
  const Card card = layout.card[static_cast<size_t>(moved.slot)];
  const KnobDesc* desc = knobDesc(card, knob);
  if (!desc) return;
  const int shown = knobShown(*desc, panel.knob[static_cast<size_t>(moved.slot)][static_cast<size_t>(knob)]);
  gui_.knobMoved(moved.slot, knob, static_cast<float>(shown), nowMs);
  readoutSlot_ = moved.slot;
  readoutCard_ = card;
}

void Face::toggleFlipped(const Layout& layout, ToggleFlip flip, uint32_t nowMs) {
  if (flip.slot < 0 || flip.slot >= kSlotCount) return;
  const Card card = layout.card[static_cast<size_t>(flip.slot)];
  if (card == Card::Empty) return;
  gui_.toggleFlipped(flip.slot, flip.on, nowMs);
  readoutSlot_ = flip.slot;
  readoutCard_ = card;
}

bool Face::due(uint32_t nowMs, const Canvas& live) { return live.dirtyPages() == 0 && tick_.due(nowMs); }

void Face::render(const GuiFrame& frame, const UiContext& ctx, Menu& menu) {
  switch (frame.screen) {
    case Screen::Voice: renderVoice(target_, ribbon_, meterRows_, frame.walk); break;
    case Screen::Readout: renderReadout(target_, readoutView(frame, ctx)); break;
    case Screen::Muted: renderMuted(target_, topSwitchWord(ctx.settings.topSwitch)); break;
    case Screen::Menu:
      if (!targetHoldsMenu_) menu.invalidate();
      menu.draw(ctx, target_);
      break;
  }
  if (frame.screen == Screen::Voice && ctx.settings.debug) drawLoad(target_, ctx.loadPercent);
  targetHoldsMenu_ = frame.screen == Screen::Menu;
}

void Face::tick(uint32_t nowMs, const UiContext& ctx, const level::InputEnergy& input,
                const level::OutputEnergy& output, Menu& menu, Canvas& live) {
  const uint32_t sinceMs = ticked_ ? nowMs - lastTickMs_ : kUiTickMs;
  ticked_ = true;
  lastTickMs_ = nowMs;
  menu.listen();
  // The number came from the card the knob belonged to, so it means nothing under another card.
  if (readoutSlot_ >= 0 && ctx.layout.card[static_cast<size_t>(readoutSlot_)] != readoutCard_) {
    gui_.dropReadout();
    readoutSlot_ = -1;
  }
  const float dtMs = static_cast<float>(sinceMs);
  meterRows_ = meterRows(inputMeter_.update(input, dtMs));
  ribbon_.push(outputMeter_.update(output, dtMs));
  const GuiFrame frame = gui_.tick(nowMs, ctx.topSwitchOn, menu.open());
  render(frame, ctx, menu);
  if (frame.dissolve == 0) {
    live.copyFrom(target_);
  } else {
    live.dissolve(live, target_, frame.dissolve);
  }
}
