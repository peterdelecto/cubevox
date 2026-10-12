#include "ui/menu.h"

#include <stdio.h>
#include <string.h>

#include "core/registry.h"
#include "core/version.h"

namespace {

constexpr uint32_t kMenuTimeoutMs = 15000;

enum class Item : uint8_t { Output, InputJack, ChainOrder, UpdateFirmware, Exit, Count };

constexpr int kItemCount = static_cast<int>(Item::Count);

using FrameRows = char[kDisplayRows][kDisplayCols + 1];

// Rows past the screen are dropped; text clips to the row width.
void row(FrameRows& frame, int r, const char* text) {
  if (r >= kDisplayRows) return;
  int n = 0;
  while (n < kDisplayCols && text[n] != '\0') {
    frame[r][n] = text[n];
    ++n;
  }
  frame[r][n] = '\0';
}

const char* itemLabel(Item it) {
  switch (it) {
    case Item::Output: return "Out";
    case Item::InputJack: return "Input";
    case Item::ChainOrder: return "Chain order";
    case Item::UpdateFirmware: return "Update firmware";
    case Item::Exit: return "Exit";
    case Item::Count: break;
  }
  return "";
}

const char* itemValue(Item it, const Settings& s) {
  switch (it) {
    case Item::Output: return s.output == OutputLevel::Instrument ? "INSTRUMENT" : "LINE";
    case Item::InputJack: return s.inputJack == InputJack::Mic ? "MIC" : "PEDAL";
    default: return "";
  }
}

void drawIdle(FrameRows& f, const UiContext& ctx) {
  row(f, 0, "cubevox");
  row(f, 1, ctx.muted ? "MUTED" : "");
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  row(f, 7, tag);
  const LastMoved m = ctx.panel.lastMoved;
  if (m.slot < 0) return;
  const Card card = ctx.layout.card[m.slot];
  const int knob = m.knobB ? 1 : 0;
  char value[kDisplayCols + 1];
  formatKnob(card, knob, ctx.panel.knob[m.slot][knob], value, sizeof(value));
  row(f, 3, cardDesc(card).name);
  row(f, 4, cardDesc(card).knob[knob].name);
  row(f, 5, value);
  row(f, 6, ctx.panel.toggle[m.slot] ? "ON" : "OFF");
}

void drawList(FrameRows& f, const UiContext& ctx, int cursor) {
  row(f, 0, "MENU");
  for (int i = 0; i < kItemCount; ++i) {
    const Item it = static_cast<Item>(i);
    char line[kDisplayCols + 1];
    snprintf(line, sizeof(line), "%s%s %s", i == cursor ? ">" : " ", itemLabel(it), itemValue(it, ctx.settings));
    row(f, 1 + i, line);
  }
}

// Eight slots fill all eight rows, so the numbered rows stand in for a title.
void drawChainView(FrameRows& f, const Layout& layout) {
  for (int i = 0; i < kSlotCount; ++i) {
    char line[kDisplayCols + 1];
    snprintf(line, sizeof(line), "%d %s", i + 1, cardDesc(layout.card[i]).name);
    row(f, i, line);
  }
}

void drawConfirmDfu(FrameRows& f) {
  row(f, 0, "UPDATE FIRMWARE");
  row(f, 2, "Click to reboot");
  row(f, 3, "into DFU mode.");
  row(f, 5, "Turn to cancel.");
}

}  // namespace

UiAction Menu::onClick() {
  switch (mode_) {
    case Mode::Idle:
      mode_ = Mode::List;
      cursor_ = 0;
      break;
    case Mode::List:
      switch (static_cast<Item>(cursor_)) {
        case Item::Output: return UiAction::ToggleOutput;
        case Item::InputJack: return UiAction::ToggleInputJack;
        case Item::ChainOrder: mode_ = Mode::ChainView; break;
        case Item::UpdateFirmware: mode_ = Mode::ConfirmDfu; break;
        case Item::Exit: mode_ = Mode::Idle; break;
        case Item::Count: break;
      }
      break;
    case Mode::ChainView: mode_ = Mode::List; break;
    case Mode::ConfirmDfu: return UiAction::EnterDfu;
  }
  return UiAction::None;
}

void Menu::onTurn(int detents) {
  if (mode_ == Mode::List) {
    cursor_ = (cursor_ + detents % kItemCount + kItemCount) % kItemCount;
  } else if (mode_ == Mode::ConfirmDfu) {
    mode_ = Mode::List;
  }
}

UiAction Menu::update(const UiInput& in, uint32_t nowMs) {
  if (in.detents != 0 || in.click) lastActivityMs_ = nowMs;
  if (in.detents != 0) onTurn(in.detents);
  const UiAction action = in.click ? onClick() : UiAction::None;
  if (mode_ != Mode::Idle && nowMs - lastActivityMs_ > kMenuTimeoutMs) mode_ = Mode::Idle;
  return action;
}

void Menu::invalidate() { shown_.row[0][0] = '\x01'; }  // no frame holds this byte

void Menu::draw(const UiContext& ctx, Canvas& canvas) {
  Frame frame = {};
  switch (mode_) {
    case Mode::Idle: drawIdle(frame.row, ctx); break;
    case Mode::List: drawList(frame.row, ctx, cursor_); break;
    case Mode::ChainView: drawChainView(frame.row, ctx.layout); break;
    case Mode::ConfirmDfu: drawConfirmDfu(frame.row); break;
  }
  if (memcmp(&frame, &shown_, sizeof(frame)) == 0) return;
  shown_ = frame;
  canvas.clear();
  for (int r = 0; r < kDisplayRows; ++r) {
    if (frame.row[r][0]) canvas.drawText(0, r * kDisplayRowPx, font5x7(), frame.row[r]);
  }
  if (mode_ == Mode::List) canvas.invertRect(0, (1 + cursor_) * kDisplayRowPx, kCanvasWidth, kDisplayRowPx);
}
