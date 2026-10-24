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

// Menu list: one row per item, no title row.
constexpr int kRowListFirstItem = 0;

// Firmware update confirmation. Prose in the 5x7 font sits on alternate rows (owner rule).
constexpr int kRowConfirmTitle = 0;
constexpr int kRowConfirmVersion = 2;
constexpr int kRowConfirmBody = 4;
constexpr int kRowConfirmCancel = 6;
constexpr char kHavePrefix[] = "You have ";
// Columns left for the version between the prefix and the period; bounds the snprintf so the
// warn gate can prove the row never truncates.
constexpr int kHaveTagCols = kDisplayCols - static_cast<int>(sizeof(kHavePrefix) - 1) - 1;

enum class Item : uint8_t {
  InputJack,
  Output,
  TopSwitch,
  PolishComp,
  PolishEq,
  UpdateFirmware,
  Exit,
  Count,
};

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
    case Item::InputJack: return "INPUT:";
    case Item::Output: return "OUTPUT:";
    case Item::TopSwitch: return "TOP SWITCH:";
    case Item::PolishComp: return "POLISH COMP:";
    case Item::PolishEq: return "POLISH EQ:";
    case Item::UpdateFirmware: return "UPDATE FIRMWARE >";
    case Item::Exit: return "EXIT";
    case Item::Count: break;
  }
  return "";
}

const char* onOff(bool on) { return on ? "ON" : "OFF"; }

const char* itemValue(Item it, const Settings& s) {
  switch (it) {
    case Item::InputJack: return s.inputJack == InputJack::Mic ? "MIC" : "LINE";
    case Item::Output: return s.outputLevel == OutputLevel::Mic ? "MIC" : "LINE";
    case Item::TopSwitch: return s.topSwitch == TopSwitch::Bypass ? "BYPASS" : "MUTE";
    case Item::PolishComp: return onOff(s.polishComp);
    case Item::PolishEq: return onOff(s.polishEq);
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

// Names the version on the box so Adam can compare it with the file he downloaded. versionTag
// clips a bench suffix, so the line always fits the row.
void drawConfirmDfu(MenuFrame& f) {
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  char have[kDisplayCols + 1];
  snprintf(have, sizeof(have), "%s%.*s.", kHavePrefix, kHaveTagCols, tag);
  putRow(f, kRowConfirmTitle, "Update the firmware?");
  putRow(f, kRowConfirmVersion, have);
  putRow(f, kRowConfirmBody, "Click to start.");
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
        case Item::InputJack: return UiAction::ToggleInputJack;
        case Item::Output: return UiAction::ToggleOutput;
        case Item::TopSwitch: return UiAction::CycleTopSwitch;
        case Item::PolishComp: return UiAction::TogglePolishComp;
        case Item::PolishEq: return UiAction::TogglePolishEq;
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
