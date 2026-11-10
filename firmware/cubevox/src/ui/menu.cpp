#include "ui/menu.h"

#include <stdio.h>

#include "core/brightness.h"
#include "core/fonts.h"
#include "core/registry.h"
#include "core/version.h"

namespace {

constexpr uint32_t kMenuTimeoutMs = 15000;

// Menu list: one row per item, no title row, drawn in the 7X12 font (owner 2026-10-30). The list
// shows four rows at a time, 16 px apart, and scrolls over the rest.
constexpr int kVisibleRows = 4;
constexpr int kListRowPitchPx = 16;

// The 12 px text sits 1 px below its row top, so the cursor bar, 14 px tall, keeps 1 px clear above
// and below it (owner rule of 2026-10-12) and leaves 2 px of dark before the next row.
constexpr int kListTextTopPx = 1;
constexpr int kListBarHeightPx = 14;

// The label's first ink column and the value's last one.
constexpr int kListLabelInkX = 1;
constexpr int kListValueInkRightX = 126;

// Firmware update confirmation. Prose in the 5x7 font sits on alternate rows (owner rule).
constexpr int kRowConfirmTitle = 0;
constexpr int kRowConfirmVersion = 2;
constexpr int kRowConfirmBody = 4;
constexpr int kRowConfirmCancel = 6;
constexpr char kHavePrefix[] = "You have ";
// Columns left for the version between the prefix and the period; bounds the snprintf so the
// warn gate can prove the row never truncates.
constexpr int kHaveTagCols = kDisplayCols - static_cast<int>(sizeof(kHavePrefix) - 1) - 1;

// The rows top to bottom.
enum class Item : uint8_t {
  InputJack,
  OctaveFormant,
  UnisonCrowd,
  DistortionGrowl,
  Slot7Card,
  Reverb,
  CleanupEq,
  Output,
  TopSwitch,
  Brightness,
  Debug,
  UpdateFirmware,
  Exit,
  Count,
};

constexpr int kItemCount = static_cast<int>(Item::Count);

static_assert(kItemCount >= kVisibleRows, "the list scrolls only when it holds more items than it shows");
static_assert(2 * kVisibleRows <= kDisplayRows, "the frame holds a value row and a label row per visible item");
static_assert((kVisibleRows - 1) * kListRowPitchPx + kListBarHeightPx < kCanvasHeight,
              "the last cursor bar must fit on the canvas and leave the last pixel line blank");

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

// The text of a SLOT row: the number of `slot` (zero-based), or a dash when no slot holds the card.
const char* slotLabel(int slot, char* buf, size_t size) {
  if (slot >= 0) {
    snprintf(buf, size, "SLOT %d", slot + 1);
  } else {
    snprintf(buf, size, "SLOT -");
  }
  return buf;
}

// The swappable slot a card row stands for. Only the five card rows call it.
SwapSlot swapSlotOfItem(Item it) {
  switch (it) {
    case Item::OctaveFormant: return SwapSlot::OctaveFormant;
    case Item::UnisonCrowd: return SwapSlot::UnisonMob;
    case Item::DistortionGrowl: return SwapSlot::DistortionDeepthroat;
    case Item::Slot7Card: return SwapSlot::Slot7;
    default: return SwapSlot::Reverb;
  }
}

// The five card rows sit together in the enum, in the family order that breaks ties below.
constexpr int kFirstCardPos = static_cast<int>(Item::OctaveFormant);
constexpr int kCardRowCount = 5;
static_assert(static_cast<int>(Item::Reverb) == kFirstCardPos + kCardRowCount - 1, "the card rows are contiguous");

// Where the card row of family `i` (0 to kCardRowCount - 1, in enum order) sorts: the slot that holds
// its card, and after every real slot when none does.
int cardRowKey(const Layout& layout, int i) {
  const int slot = swapSlotOf(layout, swapSlotOfItem(static_cast<Item>(kFirstCardPos + i)));
  return slot >= 0 ? slot : kSlotCount;
}

// The item shown at list position `pos` for `layout`. Every walk of the list goes through here. The
// card rows keep their five positions and run in ascending slot order; rows with no slot come last
// and tie in enum order, so the layout alone fixes the list.
Item itemAt(const Layout& layout, int pos) {
  if (pos < kFirstCardPos || pos >= kFirstCardPos + kCardRowCount) return static_cast<Item>(pos);
  for (int i = 0; i < kCardRowCount; ++i) {
    int rank = 0;
    for (int j = 0; j < kCardRowCount; ++j) {
      const int kj = cardRowKey(layout, j);
      const int ki = cardRowKey(layout, i);
      if (kj < ki || (kj == ki && j < i)) ++rank;
    }
    if (rank == pos - kFirstCardPos) return static_cast<Item>(kFirstCardPos + i);
  }
  return static_cast<Item>(pos);
}

// The label of `it`. The five card rows (Octave and Formant, Unison and Mob, Distortion and
// Deepthroat, slot 7, reverb) name the slot that holds their card, or a dash when none does, and
// build the text in `buf`.
const char* itemLabel(Item it, const Layout& layout, char* buf, size_t size) {
  switch (it) {
    case Item::InputJack: return "INPUT PAD";
    case Item::Output: return "OUTPUT PAD";
    case Item::TopSwitch: return "TOP SWITCH";
    case Item::Brightness: return "OLED BRIGHT";
    case Item::Reverb:
    case Item::OctaveFormant:
    case Item::UnisonCrowd:
    case Item::DistortionGrowl:
    case Item::Slot7Card: return slotLabel(swapSlotOf(layout, swapSlotOfItem(it)), buf, size);
    case Item::CleanupEq: return "CLEANUP EQ";
    case Item::Debug: return "DEBUG";
    case Item::UpdateFirmware: return "UPDATE FIRMWARE";
    case Item::Exit: return "EXIT";
    case Item::Count: break;
  }
  return "";
}

const char* onOff(bool on) { return on ? "ON" : "OFF"; }

const char* cleanupEqValue(CleanupEq v) {
  switch (v) {
    case CleanupEq::Light: return "LIGHT";
    case CleanupEq::Strong: return "STRONG";
    default: return "OFF";
  }
}

const char* itemValue(Item it, const UiContext& ctx) {
  const Settings& s = ctx.settings;
  switch (it) {
    case Item::InputJack: return onOff(s.inputJack == InputJack::Line);
    case Item::Output: return onOff(s.outputLevel == OutputLevel::Mic);
    case Item::TopSwitch: return s.topSwitch == TopSwitch::Bypass ? "BYPASS" : "MUTE";
    case Item::Brightness: return brightnessLevel(s.brightness).name;
    case Item::Reverb:
    case Item::OctaveFormant:
    case Item::UnisonCrowd:
    case Item::DistortionGrowl:
    case Item::Slot7Card: return swapWord(ctx.layout, swapSlotOfItem(it));
    case Item::CleanupEq: return cleanupEqValue(s.cleanupEq);
    case Item::Debug: return onOff(s.debug);
    case Item::UpdateFirmware: return ">";
    default: return "";
  }
}

// Writes the value of each of the kVisibleRows items from `top` on into row 0 to kVisibleRows - 1 of
// the frame, and the label of each into the kVisibleRows rows after them. A row past the last item
// stays empty.
void listRows(MenuFrame& f, const UiContext& ctx, int top) {
  for (int row = 0; row < kVisibleRows && top + row < kItemCount; ++row) {
    const Item it = itemAt(ctx.layout, top + row);
    char buf[kDisplayCols + 1];
    putRow(f, row, itemValue(it, ctx));
    putRow(f, kVisibleRows + row, itemLabel(it, ctx.layout, buf, sizeof(buf)));
  }
}

// Paints the list from the labels and values in `f`: each label at the left edge, each value ending
// at the right ink column, and the cursor row inverted over its bar.
void paintList(Canvas& canvas, const MenuFrame& f, int cursor, int top) {
  const FontDesc& font = font7x12();
  for (int row = 0; row < kVisibleRows && top + row < kItemCount; ++row) {
    const int y = row * kListRowPitchPx + kListTextTopPx;
    canvas.drawText(kListLabelInkX, y, font, f[kVisibleRows + row].data());
    const char* value = f[row].data();
    if (value[0] != '\0') canvas.drawText(kListValueInkRightX + 1 - Canvas::textWidth(font, value), y, font, value);
  }
  canvas.invertRect(0, (cursor - top) * kListRowPitchPx, kCanvasWidth, kListBarHeightPx);
}

// Paints the 5x7 text rows on the 8 px grid.
void paintRows(Canvas& canvas, const MenuFrame& f) {
  for (int r = 0; r < kDisplayRows; ++r) {
    if (f[r][0]) canvas.drawText(0, r * kDisplayRowPx, font5x7(), f[r].data());
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

UiAction Menu::onClick(const Layout& layout) {
  switch (mode_) {
    case Mode::Idle:
      mode_ = Mode::List;
      cursor_ = 0;
      top_ = 0;
      break;
    case Mode::List:
      switch (itemAt(layout, cursor_)) {
        case Item::InputJack: return UiAction::ToggleInputJack;
        case Item::Output: return UiAction::ToggleOutput;
        case Item::TopSwitch: return UiAction::CycleTopSwitch;
        case Item::Brightness: return UiAction::CycleBrightness;
        case Item::Reverb: return UiAction::SwapReverb;
        case Item::OctaveFormant: return UiAction::SwapOctaveFormant;
        case Item::UnisonCrowd: return UiAction::SwapUnisonCrowd;
        case Item::DistortionGrowl: return UiAction::SwapDistortionGrowl;
        case Item::Slot7Card: return UiAction::CycleSlot7Card;
        case Item::CleanupEq: return UiAction::CycleCleanupEq;
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

// A hold on the list closes it, as the EXIT row does. A hold with the menu closed opens it, as a click
// does. The confirm, chain and starting screens ignore it, so a long press never starts the update.
void Menu::onHold(const Layout& layout) {
  if (mode_ == Mode::Idle) {
    onClick(layout);
  } else if (mode_ == Mode::List) {
    mode_ = Mode::Idle;
  }
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

UiAction Menu::update(const UiInput& in, uint32_t nowMs, const Layout& layout) {
  if (!listening_) return UiAction::None;
  if (in.detents != 0 || in.click || in.hold || in.press) lastActivityMs_ = nowMs;
  if (in.detents != 0) onTurn(in.detents);
  const UiAction action = in.click ? onClick(layout) : UiAction::None;
  if (in.hold) onHold(layout);
  if (mode_ != Mode::Idle && mode_ != Mode::Starting && nowMs - lastActivityMs_ > kMenuTimeoutMs) mode_ = Mode::Idle;
  return action;
}

void Menu::draw(const UiContext& ctx, Canvas& canvas) {
  if (mode_ == Mode::Idle) return;
  Drawn now;
  now.mode = mode_;
  switch (mode_) {
    case Mode::Idle: break;
    case Mode::List:
      now.cursor = cursor_;
      now.top = top_;
      listRows(now.text, ctx, top_);
      break;
    case Mode::ChainView: drawChainView(now.text, ctx.layout); break;
    case Mode::ConfirmDfu: drawConfirmDfu(now.text); break;
    case Mode::Starting: drawStarting(now.text); break;
  }
  if (shownValid_ && now == shown_) return;
  shown_ = now;
  shownValid_ = true;
  canvas.clear();
  if (mode_ == Mode::List) {
    paintList(canvas, now.text, cursor_, top_);
  } else {
    paintRows(canvas, now.text);
  }
}
