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
  Reverb,
  OctaveFormant,
  UnisonCrowd,
  DistortionGrowl,
  Slot7Card,
  PolishComp,
  PolishEq,
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

// The label of `it`. The Octave and Formant row, the Unison and Crowd row, the Distortion and Growl row
// and the slot 7 row name the slot that holds their card, or a dash when none does, and build the text
// in `buf`.
const char* itemLabel(Item it, const Layout& layout, char* buf, size_t size) {
  switch (it) {
    case Item::InputJack: return "INPUT";
    case Item::Output: return "OUTPUT";
    case Item::TopSwitch: return "TOP SWITCH";
    case Item::Brightness: return "BRIGHTNESS";
    case Item::Reverb: return "REVERB";
    case Item::OctaveFormant: return slotLabel(octaveFormantSlot(layout), buf, size);
    case Item::UnisonCrowd: return slotLabel(unisonCrowdSlot(layout), buf, size);
    case Item::DistortionGrowl: return slotLabel(distortionGrowlSlot(layout), buf, size);
    case Item::Slot7Card: return slotLabel(slot7CardSlot(layout), buf, size);
    case Item::PolishComp: return "POLISH COMP";
    case Item::PolishEq: return "POLISH EQ";
    case Item::Debug: return "DEBUG";
    case Item::UpdateFirmware: return "UPDATE FIRMWARE";
    case Item::Exit: return "EXIT";
    case Item::Count: break;
  }
  return "";
}

const char* onOff(bool on) { return on ? "ON" : "OFF"; }

// The reverb card the layout holds. A valid layout always holds one; a layout without one reads
// SPRING, the default.
const char* reverbValue(const Layout& layout) {
  const int slot = reverbSlot(layout);
  return slot >= 0 && layout.card[slot] == Card::ReverbChasm ? "CHASM" : "SPRING";
}

// The card the slot 3 row holds. A layout with none of the three reads OCTAVE, the default.
const char* octaveFormantValue(const Layout& layout) {
  const int slot = octaveFormantSlot(layout);
  if (slot < 0) return "OCTAVE";
  switch (layout.card[slot]) {
    case Card::Formant: return "FORMANT";
    case Card::FormantFollower: return "YAWN";
    default: return "OCTAVE";
  }
}

// The card the Unison and Crowd slot holds, the Crowd card under its name MOB. A layout with neither
// reads UNISON, the default.
const char* unisonCrowdValue(const Layout& layout) {
  const int slot = unisonCrowdSlot(layout);
  if (slot < 0) return "UNISON";
  switch (layout.card[slot]) {
    case Card::Crowd: return "MOB";
    default: return "UNISON";
  }
}

// The card the Distortion and Growl slot holds. A layout with neither reads DISTORTION, the default.
const char* distortionGrowlValue(const Layout& layout) {
  const int slot = distortionGrowlSlot(layout);
  if (slot < 0) return "DISTORTION";
  switch (layout.card[slot]) {
    case Card::Growl: return "DEEPTHROAT";
    case Card::RingMod: return "RINGER";
    default: return "DISTORTION";
  }
}

// The card the slot 7 row holds. A layout with none of the three reads GATE, the default.
const char* slot7CardValue(const Layout& layout) {
  const int slot = slot7CardSlot(layout);
  if (slot < 0) return "GATE";
  switch (layout.card[slot]) {
    case Card::Lofi: return "LO-FI";
    case Card::Bloom: return "CHATTER";
    default: return "GATE";
  }
}

const char* itemValue(Item it, const UiContext& ctx) {
  const Settings& s = ctx.settings;
  switch (it) {
    case Item::InputJack: return s.inputJack == InputJack::Mic ? "MIC" : "LINE";
    case Item::Output: return s.outputLevel == OutputLevel::Mic ? "MIC" : "LINE";
    case Item::TopSwitch: return s.topSwitch == TopSwitch::Bypass ? "BYPASS" : "MUTE";
    case Item::Brightness: return brightnessLevel(s.brightness).name;
    case Item::Reverb: return reverbValue(ctx.layout);
    case Item::OctaveFormant: return octaveFormantValue(ctx.layout);
    case Item::UnisonCrowd: return unisonCrowdValue(ctx.layout);
    case Item::DistortionGrowl: return distortionGrowlValue(ctx.layout);
    case Item::Slot7Card: return slot7CardValue(ctx.layout);
    case Item::PolishComp: return onOff(s.polishComp);
    case Item::PolishEq: return onOff(s.polishEq);
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
    const Item it = static_cast<Item>(top + row);
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
        case Item::Brightness: return UiAction::CycleBrightness;
        case Item::Reverb: return UiAction::SwapReverb;
        case Item::OctaveFormant: return UiAction::SwapOctaveFormant;
        case Item::UnisonCrowd: return UiAction::SwapUnisonCrowd;
        case Item::DistortionGrowl: return UiAction::SwapDistortionGrowl;
        case Item::Slot7Card: return UiAction::CycleSlot7Card;
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
