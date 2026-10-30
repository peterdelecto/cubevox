// Checks the OLED menu on the host. Covers the input it drops before the face shows, the closed menu,
// the list, its scroll window and its click actions, the REVERB and DEBUG rows, the settings and
// layout those clicks save and play, the chain and update screens, the starting screen, the timeout,
// redraw, the screenshot files, the text grid and the list's geometry. The screens shown while the
// menu is closed are checked in ui_screens.cpp and ui_face.cpp.

#include "support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/canvas.h"
#include "core/compose.h"
#include "core/display.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/record.h"
#include "core/record_store.h"
#include "core/registry.h"
#include "core/settings.h"
#include "core/version.h"
#include "fake_flash.h"
#include "ui/actions.h"
#include "ui/menu.h"

namespace {

// Canvas page r holds text row r, so a row matches when its page equals the rendered text.
bool rowIs(const Canvas& canvas, int r, const char* text, bool inverted = false) {
  Canvas ref;
  ref.drawText(0, 0, font5x7(), text);
  for (int x = 0; x < kCanvasWidth; ++x) {
    const uint8_t want = inverted ? static_cast<uint8_t>(~ref.page(0)[x]) : ref.page(0)[x];
    if (canvas.page(r)[x] != want) return false;
  }
  return true;
}

// The list draws in Myrtle 7X12 on a 16 px pitch from y = 0 (owner 2026-10-30), so its rows do not
// line up with canvas pages. Row r spans pixel lines 16r to 16r + 15. The 12 px text starts at
// y = 16r + 1, the label's first ink is at x = 1 and the value's last ink at x = 126. The cursor bar
// covers lines 16r to 16r + 13 across the full width, 1 px clear above and below the ink (owner
// rule of 2026-10-12). These numbers are spelled out here, not read from the firmware.
constexpr int kListPitch = 16;
constexpr int kListTextTop = 1;
constexpr int kListBarHeight = 14;
constexpr int kLabelInkX = 1;
constexpr int kValueInkRightX = 126;

bool pixelOn(const Canvas& canvas, int x, int y) { return ((canvas.page(y / 8)[x] >> (y % 8)) & 1) != 0; }

// A list row matches when its 16 pixel lines equal the label at the left and the value at the right
// drawn in 7X12, with the bar inverted for the cursor row. The value is placed by its measured ink
// width so that its last ink column is x = 126.
bool listRowIs(const Canvas& canvas, int r, const char* label, const char* value, bool inverted = false) {
  Canvas ref;
  ref.drawText(kLabelInkX, kListTextTop, font7x12(), label);
  if (value[0] != '\0') {
    ref.drawText(kValueInkRightX + 1 - Canvas::textWidth(font7x12(), value), kListTextTop, font7x12(), value);
  }
  if (inverted) ref.invertRect(0, 0, kCanvasWidth, kListBarHeight);
  for (int line = 0; line < kListPitch; ++line) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (pixelOn(canvas, x, r * kListPitch + line) != pixelOn(ref, x, line)) return false;
    }
  }
  return true;
}

// The lit pixels of the pixel lines [from, to] as bounds, with `any` false when none is lit.
struct Ink {
  bool any = false;
  int left = kCanvasWidth;
  int right = -1;
  int top = kCanvasHeight;
  int bottom = -1;
};

Ink inkIn(const Canvas& canvas, int from, int to) {
  Ink ink;
  for (int y = from; y <= to; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (!pixelOn(canvas, x, y)) continue;
      ink.any = true;
      ink.left = x < ink.left ? x : ink.left;
      ink.right = x > ink.right ? x : ink.right;
      ink.top = y < ink.top ? y : ink.top;
      ink.bottom = y > ink.bottom ? y : ink.bottom;
    }
  }
  return ink;
}

// True when every pixel of line `y` has the state `on`.
bool lineIs(const Canvas& canvas, int y, bool on) {
  for (int x = 0; x < kCanvasWidth; ++x) {
    if (pixelOn(canvas, x, y) != on) return false;
  }
  return true;
}

bool sameCanvas(const Canvas& a, const Canvas& b) {
  for (int p = 0; p < kCanvasPages; ++p) {
    if (std::memcmp(a.page(p), b.page(p), kCanvasWidth) != 0) return false;
  }
  return true;
}

std::string chainRow(const Layout& layout, int slot) {
  char line[kDisplayCols + 1];
  std::snprintf(line, sizeof(line), "%d %s", slot + 1, cardDesc(layout.card[slot]).name);
  return line;
}

bool pagesClean(const Canvas& canvas) { return canvas.dirtyPages() == 0; }

// A menu as the face leaves it once it is on the panel, listening to the encoder.
struct MenuRig {
  Menu menu;
  Layout layout = defaultLayout();
  PanelReading panel;
  Settings settings;
  Canvas canvas;
  bool topSwitchOn = false;
  uint32_t nowMs = 100;

  MenuRig() { menu.listen(); }

  UiAction step(int detents, bool click, uint32_t advanceMs = 10) {
    nowMs += advanceMs;
    UiInput in;
    in.detents = detents;
    in.click = click;
    return menu.update(in, nowMs);
  }
  void draw() { menu.draw(UiContext{layout, panel, settings, topSwitchOn}, canvas); }

  // Opens the list and turns to `item` (see the Row constants).
  void openListAt(int item) {
    step(0, true);
    step(item, false);
  }
};

// The list rows, top to bottom (spec 2026-10-24-polish-design.md). The list shows kVisible of them
// at a time and scrolls.
enum Row {
  kInput, kHpf, kLpf, kReverb, kPolishComp, kPolishEq, kOutput, kTopSwitch, kDebug, kUpdate, kExit, kRowCount
};
constexpr int kVisible = 4;

// The screen row an item sits on when the cursor walked down to it one step at a time: the window
// scrolls only once the cursor passes the last visible row.
constexpr int rowOnScreen(int item) { return item < kVisible ? item : kVisible - 1; }

// The label of each item, and the value it shows for the default settings. Exit has no value and
// Update firmware shows a chevron.
const char* const kLabel[kRowCount] = {"INPUT",  "HPF",    "LPF",    "REVERB", "POLISH COMP", "POLISH EQ",
                                       "OUTPUT", "TOP SWITCH", "DEBUG", "UPDATE FIRMWARE", "EXIT"};
const char* const kDefaultValue[kRowCount] = {"MIC", "120 Hz", "12 kHz", "SPRING", "ON", "ON", "LINE", "MUTE", "OFF", ">", ""};

// The row as the test names it: the label, then the value after a space.
std::string rowName(int item, const char* value) {
  return std::string(kLabel[item]) + (value[0] != '\0' ? " " : "") + value;
}

// The list as the default settings draw it with `top` the first visible item and the cursor on item
// `cursor`. The window rows have their text at y 1, 17, 33 and 49, the label at x 1 and the value
// ending at x 126, and the cursor bar covers the 14 pixel lines from the row top.
Canvas listCanvas(int top, int cursor) {
  static const int kTextY[kVisible] = {1, 17, 33, 49};
  Canvas want;
  for (int k = 0; k < kVisible; ++k) {
    const int item = top + k;
    want.drawText(1, kTextY[k], font7x12(), kLabel[item]);
    if (kDefaultValue[item][0] != '\0') {
      want.drawText(127 - Canvas::textWidth(font7x12(), kDefaultValue[item]), kTextY[k], font7x12(), kDefaultValue[item]);
    }
  }
  want.invertRect(0, kTextY[cursor - top] - 1, kCanvasWidth, 14);
  return want;
}

// A closed menu draws nothing, so the runtime screens own the canvas.
void checkMenuClosed() {
  MenuRig m;
  check(!m.menu.open(), "menu: starts closed");
  clearAllDirty(m.canvas);
  m.draw();
  check(pagesClean(m.canvas), "menu: a closed menu leaves the canvas alone");
  m.step(0, true);
  check(m.menu.open(), "menu: a click opens it");
  m.draw();
  check(!pagesClean(m.canvas), "menu: an open menu draws");
  m.topSwitchOn = true;
  m.panel.lastMoved.slot = 5;
  m.draw();
  check(listRowIs(m.canvas, 0, "INPUT", "MIC", true), "menu: the top switch and the last knob do not show in the menu");
}

void checkMenuList() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  check(kRowCount == 11, "menu: the list holds eleven items");
  for (int r = 0; r < kVisible; ++r) {
    const std::string name = rowName(r, kDefaultValue[r]);
    check(listRowIs(m.canvas, r, kLabel[r], kDefaultValue[r], r == kInput), name.c_str());
  }
  check(kVisible * kListPitch <= kCanvasHeight, "menu: four rows with no title row fit the canvas on their pitch");

  m.step(-1, false);
  m.draw();
  check(listRowIs(m.canvas, kVisible - 1, "EXIT", "", true) && listRowIs(m.canvas, 0, "TOP SWITCH", "MUTE"),
        "menu: turning back from 0 wraps to the last item, which scrolls in at the bottom");
  m.step(1, false);
  m.draw();
  check(listRowIs(m.canvas, 0, "INPUT", "MIC", true), "menu: turning forward wraps to 0 and the window returns to the top");
}

// The geometry of the list, measured on the pixels. The label's first ink is at x = 1 and the value's
// last at x = 126, the text spans lines 16r + 1 to 16r + 12, the cursor bar is 14 lines tall and
// full width, and a row without a value or a chevron in front of the cursor item shows neither.
void checkMenuListGeometry() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  // The cursor row is inverted: the bar is lit, the ink dark. The bar is 14 lines from the row top
  // and full width, with the two lines under it dark.
  check(lineIs(m.canvas, 0, true) && lineIs(m.canvas, 13, true), "menu: the cursor bar's first and last lines are lit across the full width");
  bool edges = true;
  for (int y = 0; y <= 13; ++y) edges = edges && pixelOn(m.canvas, 0, y) && pixelOn(m.canvas, 127, y);
  check(edges, "menu: the cursor bar covers x 0 to 127 on each of its 14 lines");
  check(lineIs(m.canvas, 14, false) && lineIs(m.canvas, 15, false), "menu: the 2 lines under the bar are dark");
  check(!pixelOn(m.canvas, 1, 1) && !pixelOn(m.canvas, 6, 12), "menu: the cursor item's label starts at x 1 with no mark in front of it");

  m.step(2, false);
  m.draw();
  check(lineIs(m.canvas, 31, false) && lineIs(m.canvas, 32, true) && lineIs(m.canvas, 45, true) && lineIs(m.canvas, 46, false) &&
            lineIs(m.canvas, 47, false) && lineIs(m.canvas, 48, false),
        "menu: a bar on row 2 covers lines 32 to 45 and no line beside them");

  // Every item shown on a row without the bar, so its ink is measured as lit pixels. Five views,
  // each a cursor position and the window top it gives, put each of the eleven items on such a row.
  struct View {
    int jumpTo;
    int thenStep;
    int top;
  };
  const View views[] = {{kInput, 0, 0}, {kHpf, 0, 0}, {kTopSwitch, 0, 4}, {kExit, 0, 7}, {kExit, -1, 7}};
  bool seen[kRowCount] = {};
  for (const View& v : views) {
    MenuRig row;
    row.openListAt(v.jumpTo);
    if (v.thenStep != 0) row.step(v.thenStep, false);
    row.draw();
    const int cursor = v.jumpTo + v.thenStep;
    for (int r = 0; r < kVisible; ++r) {
      const int item = v.top + r;
      if (item == cursor) continue;
      seen[item] = true;
      const Ink ink = inkIn(row.canvas, r * kListPitch, r * kListPitch + kListPitch - 1);
      const bool hasValue = kDefaultValue[item][0] != '\0';
      const std::string name = "menu: " + rowName(item, kDefaultValue[item]) + " has its ink from x 1 to " +
                               (hasValue ? "126" : "its label's end") + " and from line 16r + 1 to 16r + 12 on row " + std::to_string(r);
      check(ink.any && ink.left == 1 && (hasValue ? ink.right == 126 : ink.right <= 30) && ink.top == r * kListPitch + 1 &&
                ink.bottom == r * kListPitch + 12,
            name.c_str());
    }
  }
  bool allSeen = true;
  for (bool s : seen) allSeen = allSeen && s;
  check(allSeen, "menu: the views measured all eleven items");

  // Every value an item can show clears the label by 7 px or more. The tightest are TOP SWITCH
  // BYPASS and UPDATE FIRMWARE >.
  struct Values {
    int item;
    std::array<const char*, 7> value;
  };
  const Values all[] = {
      {kInput, {"MIC", "LINE"}},
      {kHpf, {"OFF", "80 Hz", "100 Hz", "120 Hz", "150 Hz"}},
      {kLpf, {"OFF", "4 kHz", "5 kHz", "6 kHz", "8 kHz", "10 kHz", "12 kHz"}},
      {kReverb, {"SPRING", "CHASM"}},
      {kPolishComp, {"ON", "OFF"}},
      {kPolishEq, {"ON", "OFF"}},
      {kOutput, {"LINE", "MIC"}},
      {kTopSwitch, {"MUTE", "BYPASS"}},
      {kDebug, {"ON", "OFF"}},
      {kUpdate, {">"}},
  };
  int tightest = 999;
  for (const Values& v : all) {
    const int labelRight = kLabelInkX + Canvas::textWidth(font7x12(), kLabel[v.item]) - 1;
    for (const char* text : v.value) {
      if (text == nullptr) continue;
      const int valueLeft = kValueInkRightX + 1 - Canvas::textWidth(font7x12(), text);
      tightest = valueLeft - labelRight - 1 < tightest ? valueLeft - labelRight - 1 : tightest;
    }
  }
  check(tightest == 7, "menu: the tightest label to value gap is 7 px");
}

// The window starts at the top and moves only when the cursor leaves it, by one row for a step.
// Going down it scrolls at items 4 to 10; coming back up it holds until the cursor passes its top
// row, so it scrolls at items 6 to 0. The window top runs from 0 to 7. Each expected canvas is
// composed from literal y values.
void checkMenuScroll() {
  static const int kTopGoingDown[kRowCount] = {0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7};
  static const int kTopComingBack[kRowCount] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 7, 7};

  MenuRig m;
  m.step(0, true);
  for (int cursor = 0; cursor < kRowCount; ++cursor) {
    if (cursor > 0) m.step(1, false);
    m.draw();
    const std::string name = "menu: going down, the cursor on item " + std::to_string(cursor) + " shows the window from item " +
                             std::to_string(kTopGoingDown[cursor]);
    check(sameCanvas(m.canvas, listCanvas(kTopGoingDown[cursor], cursor)), name.c_str());
  }
  for (int cursor = kRowCount - 2; cursor >= 0; --cursor) {
    m.step(-1, false);
    m.draw();
    const std::string name = "menu: coming back, the cursor on item " + std::to_string(cursor) + " shows the window from item " +
                             std::to_string(kTopComingBack[cursor]);
    check(sameCanvas(m.canvas, listCanvas(kTopComingBack[cursor], cursor)), name.c_str());
  }

  m.step(-1, false);  // 0 back one wraps to the last item
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(7, kExit)), "menu: a turn back from item 0 wraps to Exit with the window at its end");
  m.step(1, false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kInput)), "menu: a turn forward from Exit wraps to item 0 with the window at its start");

  m.step(3, false);
  m.step(4, false);  // one pass of four steps leaves the window just far enough to show item 7
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(4, kTopSwitch)), "menu: a multi-step turn past the bottom scrolls only as far as the cursor needs");
  m.step(-2, false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(4, kPolishEq)), "menu: and a multi-step turn back inside the window leaves it where it is");

  MenuRig jump;
  jump.openListAt(kExit);
  jump.draw();
  check(sameCanvas(jump.canvas, listCanvas(7, kExit)), "menu: a jump straight to Exit puts it on the last row");
}

// The cursor and the window start over each time the menu opens.
void checkMenuScrollResets() {
  MenuRig m;
  m.openListAt(kExit);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(7, kExit)), "menu: the list is scrolled to Exit");
  check(m.step(0, true) == UiAction::None && !m.menu.open(), "menu: Exit closes the scrolled list");
  m.step(0, true);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kInput)), "menu: the list opens again with the cursor on item 0 and the window at the top");

  MenuRig timeout;
  timeout.openListAt(kExit);
  timeout.step(0, false, 15001);
  check(!timeout.menu.open(), "menu: the scrolled list times out");
  timeout.step(0, true);
  timeout.draw();
  check(sameCanvas(timeout.canvas, listCanvas(0, kInput)), "menu: after a timeout the list opens at the top again");
}

// The list draws its four visible rows on a 16 px pitch (owner 2026-10-30). Each 12 px text starts
// 1 px below its row top, so the cursor bar, 14 px tall, keeps 1 px clear above and below the text
// (owner 2026-10-12). The last two pixel lines stay blank, in the scrolled window as well.
void checkMenuListPitch() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kInput)), "menu: the list text starts at y 1, 17, 33 and 49");
  m.step(kVisible - 1, false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kVisible - 1)), "menu: the cursor bar follows to the last visible row, lines 48 to 61");
  m.step(kExit - (kVisible - 1), false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(7, kExit)), "menu: the scrolled window keeps the same pitch and the bar on its last row");
  check(lineIs(m.canvas, 62, false) && lineIs(m.canvas, 63, false), "menu: the last two pixel lines stay blank");
}

// Every screen but the list keeps its text rows 8 px apart, one per canvas page. The expected
// canvases are composed from literal y values.
void checkOtherScreensKeepGrid() {
  MenuRig chain;
  chain.step(0, true);
  chain.menu.showChainView();
  chain.draw();
  Canvas wantChain;
  for (int s = 0; s < kSlotCount; ++s) wantChain.drawText(0, 8 * s, font5x7(), chainRow(chain.layout, s).c_str());
  check(sameCanvas(chain.canvas, wantChain), "menu: the chain view keeps the 8 px grid");

  MenuRig confirm;
  confirm.openListAt(kUpdate);
  confirm.step(0, true);
  confirm.draw();
  const std::string have = std::string("You have ") + kFirmwareVersion + ".";
  Canvas wantConfirm;
  wantConfirm.drawText(0, 0, font5x7(), "Update the firmware?");
  wantConfirm.drawText(0, 16, font5x7(), have.c_str());
  wantConfirm.drawText(0, 32, font5x7(), "Click to start.");
  wantConfirm.drawText(0, 48, font5x7(), "Turn to cancel.");
  check(sameCanvas(confirm.canvas, wantConfirm), "menu: the confirm screen keeps the 8 px grid");
}

// A row with the cursor on it: the label of `item` and `value`, the bar inverted.
bool cursorRowIs(const Canvas& canvas, int r, int item, const char* value) {
  return listRowIs(canvas, r, kLabel[item], value, true);
}

// What a click on each of the seven rows that flip a value asks for, what applying it reports, and the
// value the row shows once the click is applied and once a second click undoes it. The rows are in
// list order. HPF and LPF step through a longer list, so checkMenuCutoffRows covers them. The reverb
// click changes the layout, so it reports LayoutChanged. The others change a setting.
struct Click {
  int item;
  UiAction action;
  ui::ApplyResult result;
  const char* after;
  const char* back;
};
constexpr std::array<Click, 7> kClicks = {{
    {kInput, UiAction::ToggleInputJack, ui::ApplyResult::SettingsChanged, "LINE", "MIC"},
    {kReverb, UiAction::SwapReverb, ui::ApplyResult::LayoutChanged, "CHASM", "SPRING"},
    {kPolishComp, UiAction::TogglePolishComp, ui::ApplyResult::SettingsChanged, "OFF", "ON"},
    {kPolishEq, UiAction::TogglePolishEq, ui::ApplyResult::SettingsChanged, "OFF", "ON"},
    {kOutput, UiAction::ToggleOutput, ui::ApplyResult::SettingsChanged, "MIC", "LINE"},
    {kTopSwitch, UiAction::CycleTopSwitch, ui::ApplyResult::SettingsChanged, "BYPASS", "MUTE"},
    {kDebug, UiAction::ToggleDebug, ui::ApplyResult::SettingsChanged, "ON", "OFF"},
}};

// Opens the list and clicks each row of kClicks once, top to bottom, applying each action. The
// cursor walks down from item 0, so a row past the fourth sits on the last list row, rowOnScreen(item).
// Returns the settings the clicks left; the layout stays in `m`.
Settings clickEveryRow(MenuRig& m) {
  m.step(0, true);
  int at = kInput;
  for (const Click& c : kClicks) {
    if (c.item != at) m.step(c.item - at, false);
    at = c.item;
    const UiAction a = m.step(0, true);
    const std::string name = rowName(c.item, c.after);
    check(a == c.action && ui::applyAction(a, m.settings, m.layout) == c.result, name.c_str());
    m.draw();
    check(cursorRowIs(m.canvas, rowOnScreen(c.item), c.item, c.after), name.c_str());
  }
  return m.settings;
}

// Each row of kClicks asks for its action, applying it flips the row's setting or swaps the reverb
// card, and a second click flips it back.
void checkMenuClicks() {
  MenuRig m;
  const Settings flipped = clickEveryRow(m);
  check(!flipped.polishComp && !flipped.polishEq && flipped.outputLevel == OutputLevel::Mic &&
            flipped.inputJack == InputJack::Line && flipped.topSwitch == TopSwitch::Bypass && flipped.debug,
        "menu: six clicks flipped the six settings");
  check(m.layout.card[7] == Card::ReverbChasm, "menu: the reverb click swapped the reverb card");
  int at = kDebug;
  const int topAtDebug = kDebug - (kVisible - 1);  // the window the walk down left
  for (int i = static_cast<int>(kClicks.size()) - 1; i >= 0; --i) {
    const Click& c = kClicks[i];
    if (c.item != at) m.step(c.item - at, false);
    at = c.item;
    const UiAction a = m.step(0, true);
    ui::applyAction(a, m.settings, m.layout);
    m.draw();
    const int row = c.item > topAtDebug ? c.item - topAtDebug : 0;  // the window holds until the cursor passes its top
    check(cursorRowIs(m.canvas, row, c.item, c.back), rowName(c.item, c.back).c_str());
  }
  check(m.settings.polishComp && m.settings.polishEq && m.settings.outputLevel == OutputLevel::Line &&
            m.settings.inputJack == InputJack::Mic && m.settings.topSwitch == TopSwitch::Mute && !m.settings.debug &&
            m.layout == defaultLayout(),
        "menu: a second click on each row restores the defaults");
}

// The REVERB row reads the layout, so its value follows the card in whichever slot holds one.
void checkMenuReverbValue() {
  struct Case {
    const char* name;
    Layout layout;
    const char* value;
  };
  Layout chasm = defaultLayout();
  chasm.card[7] = Card::ReverbChasm;
  Layout moved = defaultLayout();  // the reverb card in slot 1, with Empty where it was
  moved.card[0] = Card::ReverbChasm;
  moved.card[7] = Card::Empty;
  Layout none = defaultLayout();  // never valid; the row still names a card
  none.card[7] = Card::Empty;
  const Case cases[] = {
      {"Spring", defaultLayout(), "SPRING"},
      {"Chasm", chasm, "CHASM"},
      {"Chasm in slot 1", moved, "CHASM"},
      {"a layout with no reverb card", none, "SPRING"},
  };
  for (const Case& c : cases) {
    MenuRig m;
    m.layout = c.layout;
    m.openListAt(kReverb);
    m.draw();
    const std::string name = std::string("menu: the REVERB row reads ") + c.value + " for " + c.name;
    check(cursorRowIs(m.canvas, rowOnScreen(kReverb), kReverb, c.value), name.c_str());
  }
}

// A click on REVERB swaps the card in the slot that holds it, between Spring and Chasm, and leaves
// every other slot alone.
void checkMenuReverbClick() {
  MenuRig m;
  m.openListAt(kReverb);
  const Layout before = m.layout;
  const UiAction a = m.step(0, true);
  check(a == UiAction::SwapReverb && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::LayoutChanged,
        "menu: a click on REVERB asks for the swap and the layout changes");
  bool othersSame = true;
  for (int s = 0; s < kSlotCount; ++s) othersSame = othersSame && (s == 7 || m.layout.card[s] == before.card[s]);
  check(m.layout.card[7] == Card::ReverbChasm && othersSame, "menu: Spring becomes Chasm in its slot, the other slots stay");
  check(layoutValid(m.layout).ok(), "menu: the swapped layout passes the placement rules");
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kReverb), kReverb, "CHASM"), "menu: the row shows CHASM after the swap");

  check(ui::applyAction(m.step(0, true), m.settings, m.layout) == ui::ApplyResult::LayoutChanged &&
            m.layout == before,
        "menu: a second click swaps Chasm back to Spring");

  MenuRig moved;
  moved.layout.card[0] = Card::ReverbChasm;
  moved.layout.card[7] = Card::Empty;
  moved.openListAt(kReverb);
  check(ui::applyAction(moved.step(0, true), moved.settings, moved.layout) == ui::ApplyResult::LayoutChanged &&
            moved.layout.card[0] == Card::ReverbSpring && moved.layout.card[7] == Card::Empty,
        "menu: the card swaps in the slot that holds it");
}

// The DEBUG row toggles its setting, shows OFF by default and sits below TOP SWITCH.
void checkMenuDebugRow() {
  MenuRig m;
  m.openListAt(kDebug);
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kDebug), kDebug, "OFF") && !m.settings.debug, "menu: DEBUG starts OFF");
  const UiAction a = m.step(0, true);
  check(a == UiAction::ToggleDebug && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::SettingsChanged &&
            m.settings.debug,
        "menu: a click on DEBUG turns it ON");
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kDebug), kDebug, "ON"), "menu: the row shows ON");
  ui::applyAction(m.step(0, true), m.settings, m.layout);
  check(!m.settings.debug, "menu: a second click turns it OFF");
}

// The HPF and LPF rows sit after INPUT and show the default cutoffs. A click steps to the next
// cutoff in the row's list and wraps after the last, so one lap of clicks returns to the default.
void checkMenuCutoffRows() {
  check(kHpf == kInput + 1 && kLpf == kHpf + 1 && kReverb == kLpf + 1,
        "menu: HPF and LPF follow INPUT, then REVERB");
  static const char* const kHpfAfterClick[] = {"150 Hz", "OFF", "80 Hz", "100 Hz", "120 Hz"};
  static const char* const kLpfAfterClick[] = {"OFF", "4 kHz", "5 kHz", "6 kHz", "8 kHz", "10 kHz", "12 kHz"};
  check(sizeof(kHpfAfterClick) / sizeof(kHpfAfterClick[0]) == static_cast<size_t>(cv::kHpfCount) &&
            sizeof(kLpfAfterClick) / sizeof(kLpfAfterClick[0]) == static_cast<size_t>(cv::kLpfCount),
        "menu: one lap of clicks covers each list");

  MenuRig m;
  m.openListAt(kHpf);
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kHpf), kHpf, "120 Hz"), "menu: HPF starts at 120 Hz");
  for (int i = 0; i < cv::kHpfCount; ++i) {
    const UiAction a = m.step(0, true);
    const std::string row = rowName(kHpf, kHpfAfterClick[i]);
    check(a == UiAction::CycleHpf && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::SettingsChanged,
          (row + " click asks for the next cutoff").c_str());
    m.draw();
    check(cursorRowIs(m.canvas, rowOnScreen(kHpf), kHpf, kHpfAfterClick[i]), row.c_str());
  }
  check(m.settings.hpf == cv::kHpfDefaultIndex, "menu: one lap of clicks returns HPF to 120 Hz");

  m.step(1, false);
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kLpf), kLpf, "12 kHz"), "menu: LPF starts at 12 kHz");
  for (int i = 0; i < cv::kLpfCount; ++i) {
    const UiAction a = m.step(0, true);
    const std::string row = rowName(kLpf, kLpfAfterClick[i]);
    check(a == UiAction::CycleLpf && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::SettingsChanged,
          (row + " click asks for the next cutoff").c_str());
    m.draw();
    check(cursorRowIs(m.canvas, rowOnScreen(kLpf), kLpf, kLpfAfterClick[i]), row.c_str());
  }
  check(m.settings.lpf == cv::kLpfDefaultIndex && m.layout == defaultLayout() && m.settings.polishComp &&
            m.settings.polishEq && !m.settings.debug,
        "menu: one lap of clicks returns LPF to 12 kHz and touches no other setting");
}

// The cutoffs the menu set are what the box saves and plays: the record stores the list index plus
// one, a reload shows the same rows, and the audio snapshot carries the same cutoffs.
void checkMenuCutoffsSavedAndPlayed() {
  MenuRig m;
  m.openListAt(kHpf);
  ui::applyAction(m.step(0, true), m.settings, m.layout);  // 120 to 150
  m.step(1, false);
  ui::applyAction(m.step(0, true), m.settings, m.layout);  // 12k to OFF
  ui::applyAction(m.step(0, true), m.settings, m.layout);  // OFF to 4k
  check(m.settings.hpf == 4 && m.settings.lpf == 1, "menu: the clicks left HPF 150 and LPF 4k");

  RecordData saved;
  saved.settings = m.settings;
  saved.layout = m.layout;
  const RecordBytes bytes = encodeRecord(saved);
  RecordData loaded;
  check(bytes[kOffHpf] == 5 && bytes[kOffLpf] == 2 && decodeRecord(bytes.data(), loaded) && loaded.settings.hpf == 4 &&
            loaded.settings.lpf == 1,
        "menu: the record holds the list index plus one, 5 for 150 and 2 for 4k, and a reload reads them back");

  MenuRig again;
  again.settings = loaded.settings;
  again.openListAt(kHpf);
  again.draw();
  check(cursorRowIs(again.canvas, rowOnScreen(kHpf), kHpf, "150 Hz"), "menu: after the reload the HPF row reads 150 Hz");
  again.step(1, false);
  again.draw();
  check(cursorRowIs(again.canvas, rowOnScreen(kLpf), kLpf, "4 kHz"), "menu: and the LPF row reads 4 kHz");

  const ChainSnapshot snap = composeSnapshot(m.layout, m.panel, m.settings, BenchOverrides());
  check(snap.params.conditioning.hpf == 4 && snap.params.conditioning.lpf == 1,
        "menu: the audio snapshot carries the cutoffs the rows show");
  const ChainSnapshot fresh = composeSnapshot(m.layout, m.panel, Settings(), BenchOverrides());
  check(fresh.params.conditioning.hpf == cv::kHpfDefaultIndex && fresh.params.conditioning.lpf == cv::kLpfDefaultIndex,
        "menu: the default snapshot runs HPF 120 and LPF 12k");

  // Through the flash store, the way the sketch saves after the debounce.
  FakeFlash flash;
  RecordStore store(flash);
  RecordData booted;
  store.begin(booted);
  store.request(m.layout, m.settings, 1000);
  uint32_t t = 1000 + kSaveDebounceMs;
  for (int i = 0; i < 100 && store.status().busy; ++i) store.poll(t++);
  FakeFlash rebooted;
  std::memcpy(rebooted.mem[0].data(), flash.mem[0].data(), kRecordBytes);
  RecordStore reboot(rebooted);
  RecordData out;
  check(reboot.begin(out) && out.settings.hpf == 4 && out.settings.lpf == 1, "menu: the next boot loads HPF 150 and LPF 4k");
}

// What the box saves and plays is what the menu showed.
void checkMenuClicksSavedAndPlayed() {
  MenuRig m;
  const Settings flipped = clickEveryRow(m);
  RecordData saved;
  saved.settings = flipped;
  saved.layout = m.layout;
  RecordData loaded;
  check(decodeRecord(encodeRecord(saved).data(), loaded) && loaded.settings.polishComp == flipped.polishComp &&
            loaded.settings.polishEq == flipped.polishEq && loaded.settings.outputLevel == flipped.outputLevel &&
            loaded.settings.inputJack == flipped.inputJack && loaded.settings.topSwitch == flipped.topSwitch &&
            loaded.settings.debug && loaded.layout == m.layout && loaded.layout.card[7] == Card::ReverbChasm,
        "menu: the flipped settings and the swapped reverb card survive a save and a reload");
  const ChainSnapshot snap = composeSnapshot(m.layout, m.panel, flipped, BenchOverrides());
  check(!snap.params.comp.on && !snap.params.eq.on && snap.outputGain < 0.3f,
        "menu: the flipped settings reach the audio snapshot");
  check(snap.params.reverb.engine == cv::kReverbChasm && snap.layout == m.layout,
        "menu: the swapped reverb card reaches the audio snapshot");
  const ChainSnapshot fresh = composeSnapshot(m.layout, m.panel, Settings(), BenchOverrides());
  check(fresh.params.comp.on && fresh.params.eq.on && fresh.outputGain == 1.0f, "menu: the default snapshot has both polish stages on");
}

// A click that changes the layout or DEBUG leaves a record in flash that the next boot loads, the way
// the sketch's save does after the debounce.
void checkMenuSavedThroughStore() {
  MenuRig m;
  m.openListAt(kReverb);
  ui::applyAction(m.step(0, true), m.settings, m.layout);
  m.step(kDebug - kReverb, false);
  ui::applyAction(m.step(0, true), m.settings, m.layout);

  FakeFlash flash;
  RecordStore store(flash);
  RecordData booted;
  store.begin(booted);
  store.request(m.layout, m.settings, 1000);
  uint32_t t = 1000 + kSaveDebounceMs;
  for (int i = 0; i < 100 && store.status().busy; ++i) store.poll(t++);
  check(store.status().sequence == 1 && flash.programs == kWordsPerRecord, "menu: the click's layout and settings write one record");

  FakeFlash again;
  std::memcpy(again.mem[0].data(), flash.mem[0].data(), kRecordBytes);
  RecordStore reboot(again);
  RecordData out;
  check(reboot.begin(out) && out.layout.card[7] == Card::ReverbChasm && out.settings.debug,
        "menu: the next boot loads Chasm and DEBUG ON");
}

// Until the face is on the panel, nobody can see the menu, so a click or a turn must do nothing.
void checkMenuDeafUntilListening() {
  Menu menu;
  UiInput click;
  click.click = true;
  UiInput turn;
  turn.detents = 3;
  check(menu.update(click, 100) == UiAction::None && menu.update(turn, 110) == UiAction::None && !menu.open(),
        "menu: before listen() a click and a turn open nothing");
  check(menu.update(click, 120) == UiAction::None && !menu.open(), "menu: a second click before listen() flips no setting");
  menu.listen();
  check(menu.update(click, 130) == UiAction::None && menu.open(), "menu: once listening, a click opens the list");
  check(menu.update(click, 140) == UiAction::ToggleInputJack, "menu: and the list starts on its first row, untouched by the dropped turn");
}

void checkMenuWrapAndCombo() {
  MenuRig m;
  m.openListAt(kRowCount + 2);  // a full lap and two more detents lands on item 2
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kLpf), kLpf, "12 kHz"), "menu: +13 detents wraps around the list");
  m.step(-(kRowCount + 2), false);  // back by the same lands on item 0
  m.draw();
  check(cursorRowIs(m.canvas, kInput, kInput, "MIC"), "menu: -13 detents wraps the other way");

  MenuRig n;
  n.openListAt(0);
  check(n.step(1, true) == UiAction::CycleHpf, "menu: a turn and a click in one pass act on the new cursor");
}

void checkMenuScreens() {
  MenuRig m;
  m.step(0, true);
  m.menu.showChainView();
  m.draw();
  check(rowIs(m.canvas, 0, chainRow(m.layout, 0).c_str()), "menu: chain view starts at slot 1");
  check(rowIs(m.canvas, kDisplayRows - 1, chainRow(m.layout, 7).c_str()), "menu: chain view shows slot 8");
  check(m.step(0, true) == UiAction::None, "menu: a click in the chain view has no action");
  m.draw();
  check(cursorRowIs(m.canvas, 0, kInput, "MIC"), "menu: click leaves the chain view for the list");

  m.step(kUpdate, false);
  m.step(0, true);
  m.draw();
  const std::string have = std::string("You have ") + kFirmwareVersion + ".";
  check(rowIs(m.canvas, 0, "Update the firmware?") && rowIs(m.canvas, 2, have.c_str()),
        "menu: confirm screen asks and names the version on the box");
  check(rowIs(m.canvas, 4, "Click to start.") && rowIs(m.canvas, 6, "Turn to cancel."),
        "menu: confirm screen says a click starts and a turn cancels");
  check(rowIs(m.canvas, 1, "") && rowIs(m.canvas, 3, "") && rowIs(m.canvas, 5, "") && rowIs(m.canvas, 7, ""),
        "menu: confirm screen keeps a blank row between sentences");
  m.step(1, false);
  m.draw();
  check(cursorRowIs(m.canvas, rowOnScreen(kUpdate), kUpdate, ">"),
        "menu: turn cancels the confirm and returns to the list, still scrolled to Update");
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: click on confirm enters DFU");
}

// From the confirm click to the reset the glass says the update is starting, takes no input and
// never times out, because the reset waits on the flash.
void checkMenuUpdateStarting() {
  MenuRig m;
  m.openListAt(kUpdate);
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: the confirm click starts the update");
  m.draw();
  check(rowIs(m.canvas, 0, "Starting the update.") && rowIs(m.canvas, 2, "Wait a moment.") && rowIs(m.canvas, 4, "") &&
            rowIs(m.canvas, 6, ""),
        "menu: the starting screen replaces the confirm screen");
  check(m.step(1, false) == UiAction::None && m.step(0, true) == UiAction::None && m.menu.open(),
        "menu: turns and clicks leave the starting screen up");
  m.step(0, false, 60000);
  check(m.menu.open(), "menu: the starting screen stays up until the reset");
  writePgm(m.canvas, "oled-update-starting.pgm");
}

void checkMenuExit() {
  MenuRig m;
  m.openListAt(kExit);
  m.draw();
  check(cursorRowIs(m.canvas, kVisible - 1, kExit, ""), "menu: the cursor sits on Exit, the last visible row");
  check(m.step(0, true) == UiAction::None, "menu: Exit click has no action");
  check(!m.menu.open(), "menu: Exit closes the menu");
  clearAllDirty(m.canvas);
  m.draw();
  check(pagesClean(m.canvas), "menu: a closed menu draws nothing after Exit");
}

void checkMenuTimeout() {
  MenuRig m;
  m.step(0, true);
  m.step(0, false, 14000);
  m.draw();
  check(cursorRowIs(m.canvas, 0, kInput, "MIC") && m.menu.open(), "menu: still open before the timeout");
  m.step(0, false, 1500);
  check(!m.menu.open(), "menu: 15 s without input closes it");

  MenuRig chain;
  chain.step(0, true);
  chain.menu.showChainView();
  chain.step(0, false, 15001);
  check(!chain.menu.open(), "menu: the chain view times out");

  MenuRig dfu;
  dfu.openListAt(kUpdate);
  dfu.step(0, true);
  dfu.step(0, false, 15001);
  check(!dfu.menu.open(), "menu: the DFU confirm times out without acting");
}

void checkMenuRedraw() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  clearAllDirty(m.canvas);
  m.draw();
  check(pagesClean(m.canvas), "menu: an unchanged frame is not redrawn");

  // A change to anything the list shows repaints; a change to a row outside the window does not.
  m.settings.polishEq = false;  // POLISH EQ is item 5, outside the window of items 0 to 3
  m.draw();
  check(pagesClean(m.canvas), "menu: a value change outside the window is not redrawn");
  m.settings.inputJack = InputJack::Line;
  m.draw();
  check(!pagesClean(m.canvas) && cursorRowIs(m.canvas, 0, kInput, "LINE"), "menu: a value change in the window repaints it");
  clearAllDirty(m.canvas);
  m.step(1, false);
  m.draw();
  check(!pagesClean(m.canvas) && listRowIs(m.canvas, 0, "INPUT", "LINE") && cursorRowIs(m.canvas, 1, kHpf, "120 Hz"),
        "menu: a cursor move repaints");
  clearAllDirty(m.canvas);
  m.step(4, false);  // the cursor passes the last row to item 5, so the window scrolls to start at item 2
  m.draw();
  check(!pagesClean(m.canvas) && cursorRowIs(m.canvas, 3, kPolishEq, "OFF"), "menu: a scroll repaints");
  clearAllDirty(m.canvas);
  m.draw();
  check(pagesClean(m.canvas), "menu: and the scrolled window is then skipped when unchanged");
}

void checkMenuInvalidate() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  clearAllDirty(m.canvas);
  m.menu.invalidate();
  m.draw();
  check(!pagesClean(m.canvas), "menu: invalidate makes the next draw repaint");
}

void checkScreenshots() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  bool listRows = true;
  for (int r = 0; r < kVisible; ++r) listRows = listRows && listRowIs(m.canvas, r, kLabel[r], kDefaultValue[r], r == kInput);
  check(listRows, "screens: menu rows");
  writePgm(m.canvas, "oled-menu.pgm");

  MenuRig scrolled;
  scrolled.openListAt(kExit);
  scrolled.draw();
  check(sameCanvas(scrolled.canvas, listCanvas(7, kExit)), "screens: scrolled menu rows");
  writePgm(scrolled.canvas, "oled-menu-scrolled.pgm");

  m.menu.showChainView();
  m.draw();
  check(rowIs(m.canvas, 0, chainRow(m.layout, 0).c_str()) && rowIs(m.canvas, 7, chainRow(m.layout, 7).c_str()),
        "screens: chain rows");
  writePgm(m.canvas, "oled-chain.pgm");

  m.step(0, true);
  m.step(kUpdate, false);
  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, "Update the firmware?") && rowIs(m.canvas, 6, "Turn to cancel."), "screens: confirm rows");
  writePgm(m.canvas, "oled-confirm.pgm");
}

void checkDisplayGrid() {
  check(kDisplayRows * kDisplayRowPx == kCanvasHeight, "display: the rows fill the canvas height");
  check(kDisplayCols * kFont5x7Advance <= kCanvasWidth && (kDisplayCols + 1) * kFont5x7Advance > kCanvasWidth,
        "display: the columns fill the canvas width at the 5x7 advance");
}

}  // namespace

void checkMenu() {
  checkMenuDeafUntilListening();
  checkMenuClosed();
  checkMenuList();
  checkMenuListGeometry();
  checkMenuScroll();
  checkMenuScrollResets();
  checkMenuListPitch();
  checkMenuClicks();
  checkMenuReverbValue();
  checkMenuReverbClick();
  checkMenuDebugRow();
  checkMenuCutoffRows();
  checkMenuCutoffsSavedAndPlayed();
  checkMenuClicksSavedAndPlayed();
  checkMenuSavedThroughStore();
  checkMenuWrapAndCombo();
  checkMenuScreens();
  checkMenuUpdateStarting();
  checkOtherScreensKeepGrid();
  checkMenuExit();
  checkMenuTimeout();
  checkMenuRedraw();
  checkMenuInvalidate();
  checkScreenshots();
  checkDisplayGrid();
}
