#include "ui/menu.h"

#include <stdio.h>

#include "core/fonts.h"
#include "core/registry.h"
#include "core/version.h"

namespace {

constexpr uint32_t kMenuTimeoutMs = 15000;

// Menu list: one row per item, no title row.
constexpr int kRowListFirstItem = 0;

// The list shows seven rows at a time, which leaves breathing room, so they sit 9 px apart (owner
// 2026-10-24). The list holds more items than that and scrolls.
constexpr int kVisibleRows = 7;
constexpr int kListRowPitchPx = 9;

// The text sits 1 px below its row top, so the cursor bar keeps 1 px clear above and below it
// (owner 2026-10-12).
constexpr int kListTextTopPx = 1;

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
  Reverb,
  PolishComp,
  PolishEq,
  Debug,
  UpdateFirmware,
  Exit,
  Count,
};

constexpr int kItemCount = static_cast<int>(Item::Count);

static_assert(kItemCount >= kVisibleRows, "the list scrolls only when it holds more items than it shows");
static_assert(kRowListFirstItem + kVisibleRows <= kDisplayRows, "the visible menu rows must fit on the screen");
static_assert((kRowListFirstItem + kVisibleRows) * kListRowPitchPx < kCanvasHeight,
              "the visible menu rows must fit at their pitch and leave the last pixel line blank");

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
    case Item::Reverb: return "REVERB:";
    case Item::PolishComp: return "POLISH COMP:";
    case Item::PolishEq: return "POLISH EQ:";
    case Item::Debug: return "DEBUG:";
    case Item::UpdateFirmware: return "UPDATE FIRMWARE >";
    case Item::Exit: return "EXIT";
    case Item::Count: break;
  }
  return "";
}

const char* onOff(bool on) { return on ? "ON" : "OFF"; }

// The reverb card the layout holds, or NONE when no slot has one.
const char* reverbValue(const Layout& layout) {
  const int slot = reverbSlot(layout);
  if (slot < 0) return "NONE";
  return layout.card[slot] == Card::ReverbSpring ? "SPRING" : "CHASM";
}

const char* itemValue(Item it, const UiContext& ctx) {
  const Settings& s = ctx.settings;
  switch (it) {
    case Item::InputJack: return s.inputJack == InputJack::Mic ? "MIC" : "LINE";
    case Item::Output: return s.outputLevel == OutputLevel::Mic ? "MIC" : "LINE";
    case Item::TopSwitch: return s.topSwitch == TopSwitch::Bypass ? "BYPASS" : "MUTE";
    case Item::Reverb: return reverbValue(ctx.layout);
    case Item::PolishComp: return onOff(s.polishComp);
    case Item::PolishEq: return onOff(s.polishEq);
    case Item::Debug: return onOff(s.debug);
    default: return "";
  }
}

// Draws the kVisibleRows items from `top` on, with the cursor mark in front of item `cursor`.
void drawList(MenuFrame& f, const UiContext& ctx, int cursor, int top) {
  for (int row = 0; row < kVisibleRows; ++row) {
    const int i = top + row;
    const Item it = static_cast<Item>(i);
    const char* value = itemValue(it, ctx);
    char line[kDisplayCols + 1];
    snprintf(line, sizeof(line), "%s%s%s%s", i == cursor ? ">" : " ", itemLabel(it), value[0] != '\0' ? " " : "",
             value);
    putRow(f, kRowListFirstItem + row, line);
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

// Names the version on the box so Adam can compare it with the file he downloaded.
void drawConfirmDfu(MenuFrame& f) {
  char have[kDisplayCols + 1];
  snprintf(have, sizeof(have), "%s%.*s.", kHavePrefix, kHaveTagCols, kFirmwareVersion);
  putRow(f, kRowConfirmTitle, "Update the firmware?");
  putRow(f, kRowConfirmVersion, have);
  putRow(f, kRowConfirmBody, "Click to start.");
  putRow(f, kRowConfirmCancel, "Turn to cancel.");
}

// Shows from the confirm click until the reset, while a pending save and the diag entry reach flash.
void drawStarting(MenuFrame& f) {
  putRow(f, kRowConfirmTitle, "Starting the update.");
  putRow(f, kRowConfirmVersion, "Wait a moment.");
}

}  // namespace

UiAction Menu::onClick() {
  switch (mode_) {
    case Mode::Idle:
      mode_ = Mode::List;
      cursor_ = 0;
      top_ = 0;
      break;
    case Mode::List:
      switch (static_cast<Item>(cursor_)) {
        case Item::InputJack: return UiAction::ToggleInputJack;
        case Item::Output: return UiAction::ToggleOutput;
        case Item::TopSwitch: return UiAction::CycleTopSwitch;
        case Item::Reverb: return UiAction::SwapReverb;
        case Item::PolishComp: return UiAction::TogglePolishComp;
        case Item::PolishEq: return UiAction::TogglePolishEq;
        case Item::Debug: return UiAction::ToggleDebug;
        case Item::UpdateFirmware: mode_ = Mode::ConfirmDfu; break;
        case Item::Exit: mode_ = Mode::Idle; break;
        case Item::Count: break;
      }
      break;
    case Mode::ChainView: mode_ = Mode::List; break;
    case Mode::ConfirmDfu:
      mode_ = Mode::Starting;
      return UiAction::EnterDfu;
    case Mode::Starting: break;
  }
  return UiAction::None;
}

void Menu::onTurn(int32_t detents) {
  if (mode_ == Mode::List) {
    cursor_ = static_cast<uint8_t>((cursor_ + detents % kItemCount + kItemCount) % kItemCount);
    // The window moves just far enough to keep the cursor in view, so a step past an edge scrolls one row.
    if (cursor_ < top_) {
      top_ = cursor_;
    } else if (cursor_ >= top_ + kVisibleRows) {
      top_ = static_cast<uint8_t>(cursor_ - kVisibleRows + 1);
    }
  } else if (mode_ == Mode::ConfirmDfu) {
    mode_ = Mode::List;
  }
}

UiAction Menu::update(const UiInput& in, uint32_t nowMs) {
  if (!listening_) return UiAction::None;
  if (in.detents != 0 || in.click) lastActivityMs_ = nowMs;
  if (in.detents != 0) onTurn(in.detents);
  const UiAction action = in.click ? onClick() : UiAction::None;
  if (mode_ != Mode::Idle && mode_ != Mode::Starting && nowMs - lastActivityMs_ > kMenuTimeoutMs) mode_ = Mode::Idle;
  return action;
}

void Menu::draw(const UiContext& ctx, Canvas& canvas) {
  if (mode_ == Mode::Idle) return;
  MenuFrame frame = {};
  switch (mode_) {
    case Mode::Idle: break;
    case Mode::List: drawList(frame, ctx, cursor_, top_); break;
    case Mode::ChainView: drawChainView(frame, ctx.layout); break;
    case Mode::ConfirmDfu: drawConfirmDfu(frame); break;
    case Mode::Starting: drawStarting(frame); break;
  }
  if (shownValid_ && frame == shown_) return;
  shown_ = frame;
  shownValid_ = true;
  canvas.clear();
  const bool list = mode_ == Mode::List;
  const int pitch = list ? kListRowPitchPx : kDisplayRowPx;
  const int textTop = list ? kListTextTopPx : 0;
  for (int r = 0; r < kDisplayRows; ++r) {
    if (frame[r][0]) canvas.drawText(0, r * pitch + textTop, font5x7(), frame[r].data());
  }
  if (list) {
    canvas.invertRect(0, (kRowListFirstItem + cursor_ - top_) * pitch, kCanvasWidth, kListRowPitchPx);
  }
}
