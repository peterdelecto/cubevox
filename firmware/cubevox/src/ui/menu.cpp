#include "ui/menu.h"

#include <stdio.h>

#include "core/fonts.h"
#include "core/registry.h"
#include "core/version.h"

namespace {

constexpr uint32_t kMenuTimeoutMs = 15000;

// Idle screen.
constexpr int kRowTitle = 0;
constexpr int kRowMuted = 1;
constexpr int kRowCardName = 3;
constexpr int kRowKnobName = 4;
constexpr int kRowKnobValue = 5;
constexpr int kRowToggle = 6;
constexpr int kRowVersion = kDisplayRows - 1;

// Menu list: a title row, then one row per item.
constexpr int kRowListTitle = 0;
constexpr int kRowListFirstItem = 1;

// Firmware update confirmation.
constexpr int kRowConfirmTitle = 0;
constexpr int kRowConfirmBody1 = 2;
constexpr int kRowConfirmBody2 = 3;
constexpr int kRowConfirmCancel = 5;

enum class Item : uint8_t { Output, InputJack, ChainOrder, UpdateFirmware, Exit, Count };

constexpr int kItemCount = static_cast<int>(Item::Count);

static_assert(kRowListFirstItem + kItemCount <= kDisplayRows, "the menu items must fit on the screen");

// Rows past the screen are dropped; text clips to the row width.
void putRow(MenuFrame& frame, int r, const char* text) {
  if (r < 0 || r >= kDisplayRows) return;
  size_t n = 0;
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
    case Item::Output: return s.outputLevel == OutputLevel::Instrument ? "INSTRUMENT" : "LINE";
    case Item::InputJack: return s.inputJack == InputJack::Mic ? "MIC" : "PEDAL";
    default: return "";
  }
}

void drawIdle(MenuFrame& f, const UiContext& ctx) {
  putRow(f, kRowTitle, "cubevox");
  putRow(f, kRowMuted, ctx.muted ? "MUTED" : "");
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  putRow(f, kRowVersion, tag);
  const LastMoved m = ctx.panel.lastMoved;
  if (m.slot < 0 || m.slot >= kSlotCount) return;
  const Card card = ctx.layout.card[m.slot];
  const int knob = m.knobB ? 1 : 0;
  char value[kDisplayCols + 1];
  formatKnob(card, knob, ctx.panel.knob[m.slot][knob], value, sizeof(value));
  putRow(f, kRowCardName, cardDesc(card).name);
  putRow(f, kRowKnobName, cardDesc(card).knob[knob].name);
  putRow(f, kRowKnobValue, value);
  putRow(f, kRowToggle, ctx.panel.toggle[m.slot] ? "ON" : "OFF");
}

void drawList(MenuFrame& f, const UiContext& ctx, int cursor) {
  putRow(f, kRowListTitle, "MENU");
  for (int i = 0; i < kItemCount; ++i) {
    const Item it = static_cast<Item>(i);
    const char* value = itemValue(it, ctx.settings);
    char line[kDisplayCols + 1];
    snprintf(line, sizeof(line), "%s%s%s%s", i == cursor ? ">" : " ", itemLabel(it), value[0] != '\0' ? " " : "",
             value);
    putRow(f, kRowListFirstItem + i, line);
  }
}

// The slots fill every row, so the numbered rows stand in for a title.
void drawChainView(MenuFrame& f, const Layout& layout) {
  static_assert(kSlotCount <= kDisplayRows, "the chain view shows one slot per row");
  for (int i = 0; i < kSlotCount; ++i) {
    char line[kDisplayCols + 1];
    snprintf(line, sizeof(line), "%d %s", i + 1, cardDesc(layout.card[i]).name);
    putRow(f, i, line);
  }
}

void drawConfirmDfu(MenuFrame& f) {
  putRow(f, kRowConfirmTitle, "UPDATE FIRMWARE");
  putRow(f, kRowConfirmBody1, "Click to reboot");
  putRow(f, kRowConfirmBody2, "into DFU mode.");
  putRow(f, kRowConfirmCancel, "Turn to cancel.");
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

void Menu::onTurn(int32_t detents) {
  if (mode_ == Mode::List) {
    cursor_ = static_cast<uint8_t>((cursor_ + detents % kItemCount + kItemCount) % kItemCount);
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

void Menu::draw(const UiContext& ctx, Canvas& canvas) {
  MenuFrame frame = {};
  switch (mode_) {
    case Mode::Idle: drawIdle(frame, ctx); break;
    case Mode::List: drawList(frame, ctx, cursor_); break;
    case Mode::ChainView: drawChainView(frame, ctx.layout); break;
    case Mode::ConfirmDfu: drawConfirmDfu(frame); break;
  }
  if (shownValid_ && frame == shown_) return;
  shown_ = frame;
  shownValid_ = true;
  canvas.clear();
  for (int r = 0; r < kDisplayRows; ++r) {
    if (frame[r][0]) canvas.drawText(0, r * kDisplayRowPx, font5x7(), frame[r].data());
  }
  if (mode_ == Mode::List) {
    canvas.invertRect(0, (kRowListFirstItem + cursor_) * kDisplayRowPx, kCanvasWidth, kDisplayRowPx);
  }
}
