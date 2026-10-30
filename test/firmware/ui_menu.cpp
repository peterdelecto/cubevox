// Checks the OLED menu on the host. Covers the input it drops before the face shows, the closed menu,
// the list, its scroll window and its click actions, the REVERB and DEBUG rows, the settings and
// layout those clicks save and play, the chain and update screens, the starting screen, the timeout,
// redraw, the screenshot files, the text grid and the list's row pitch. The screens shown while the
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

// The list rows sit on a 9 px pitch from y = 0 (owner 2026-10-24), so they do not line up with canvas
// pages. The 7 px text sits 1 px below its row top, 1 px clear above and below inside the cursor bar
// (owner 2026-10-12). A list row matches when its 9 pixel lines equal the rendered text, with the
// bar inverted for the cursor row.
constexpr int kListPitch = 9;
constexpr int kListTextTop = 1;

bool pixelOn(const Canvas& canvas, int x, int y) { return ((canvas.page(y / 8)[x] >> (y % 8)) & 1) != 0; }

bool listRowIs(const Canvas& canvas, int r, const char* text, bool inverted = false) {
  Canvas ref;
  ref.drawText(0, kListTextTop, font5x7(), text);
  if (inverted) ref.invertRect(0, 0, kCanvasWidth, kListPitch);
  for (int line = 0; line < kListPitch; ++line) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (pixelOn(canvas, x, r * kListPitch + line) != pixelOn(ref, x, line)) return false;
    }
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
  kInput, kOutput, kTopSwitch, kReverb, kPolishComp, kPolishEq, kDebug, kHpf, kLpf, kUpdate, kExit, kRowCount
};
constexpr int kVisible = 7;

// The screen row an item sits on when the cursor walked down to it one step at a time: the window
// scrolls only once the cursor passes the last visible row.
constexpr int rowOnScreen(int item) { return item < kVisible ? item : kVisible - 1; }

// List row `r` as drawn for the default settings: the cursor mark or a blank in column 0, then the text.
std::string defaultRow(int r, bool cursor) {
  static const char* const text[kRowCount] = {"INPUT: MIC",      "OUTPUT: LINE",  "TOP SWITCH: MUTE",
                                              "REVERB: SPRING",  "POLISH COMP: ON", "POLISH EQ: ON",
                                              "DEBUG: OFF",      "HPF: 120",       "LPF: 12k",
                                              "UPDATE FIRMWARE >", "EXIT"};
  return std::string(cursor ? ">" : " ") + text[r];
}

// The list as the default settings draw it with `top` the first visible item and the cursor on item
// `cursor`. The window rows have their text at y 1, 10, 19, 28, 37, 46 and 55, and the cursor bar
// covers the 9 pixel lines from 1 px above its text.
Canvas listCanvas(int top, int cursor) {
  static const int kTextY[kVisible] = {1, 10, 19, 28, 37, 46, 55};
  Canvas want;
  for (int k = 0; k < kVisible; ++k) want.drawText(0, kTextY[k], font5x7(), defaultRow(top + k, top + k == cursor).c_str());
  want.invertRect(0, kTextY[cursor - top] - 1, kCanvasWidth, kListPitch);
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
  check(listRowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: the top switch and the last knob do not show in the menu");
}

void checkMenuList() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  check(kRowCount == 11, "menu: the list holds eleven items");
  for (int r = 0; r < kVisible; ++r) {
    const std::string want = defaultRow(r, r == kInput);
    check(listRowIs(m.canvas, r, want.c_str(), r == kInput), want.c_str());
  }
  check(kVisible * kListPitch <= kCanvasHeight, "menu: seven rows with no title row fit the canvas on their pitch");

  m.step(-1, false);
  m.draw();
  check(listRowIs(m.canvas, kVisible - 1, ">EXIT", true) && listRowIs(m.canvas, 0, " POLISH COMP: ON"),
        "menu: turning back from 0 wraps to the last item, which scrolls in at the bottom");
  m.step(1, false);
  m.draw();
  check(listRowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: turning forward wraps to 0 and the window returns to the top");
}

// The window starts at the top and moves only when the cursor leaves it, by one row for a step.
// Going down it scrolls at items 7 to 10; coming back up it holds until the cursor passes its top
// row, so it scrolls at items 3 to 0. Each expected canvas is composed from literal y values.
void checkMenuScroll() {
  static const int kTopGoingDown[kRowCount] = {0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  static const int kTopComingBack[kRowCount] = {0, 1, 2, 3, 4, 4, 4, 4, 4, 4, 4};

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
  check(sameCanvas(m.canvas, listCanvas(4, kExit)), "menu: a turn back from item 0 wraps to Exit with the window at its end");
  m.step(1, false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kInput)), "menu: a turn forward from Exit wraps to item 0 with the window at its start");

  m.step(3, false);
  m.step(4, false);  // one pass of four steps leaves the window just far enough to show item 7
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(1, kHpf)), "menu: a multi-step turn past the bottom scrolls only as far as the cursor needs");
  m.step(-4, false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(1, kReverb)), "menu: and a multi-step turn back inside the window leaves it where it is");

  MenuRig jump;
  jump.openListAt(kExit);
  jump.draw();
  check(sameCanvas(jump.canvas, listCanvas(4, kExit)), "menu: a jump straight to Exit puts it on the last row");
}

// The cursor and the window start over each time the menu opens.
void checkMenuScrollResets() {
  MenuRig m;
  m.openListAt(kExit);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(4, kExit)), "menu: the list is scrolled to Exit");
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

// The list draws its seven visible rows on a 9 px pitch (owner 2026-10-24). Each 7 px text starts
// 1 px below its row top, so the cursor bar, the 9 px row, keeps 1 px clear above and below the text
// (owner 2026-10-12). The last pixel line stays blank, in the scrolled window as well.
void checkMenuListPitch() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kInput)), "menu: the list text starts at y 1, 10, 19, 28, 37, 46 and 55");
  m.step(kVisible - 1, false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(0, kVisible - 1)), "menu: the cursor bar follows to the last visible row, lines 54 to 62");
  m.step(kExit - (kVisible - 1), false);
  m.draw();
  check(sameCanvas(m.canvas, listCanvas(4, kExit)), "menu: the scrolled window keeps the same pitch and the bar on its last row");
  bool lastLineBlank = true;
  for (int x = 0; x < kCanvasWidth; ++x) lastLineBlank = lastLineBlank && !pixelOn(m.canvas, x, kCanvasHeight - 1);
  check(lastLineBlank, "menu: the last pixel line stays blank");
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

// What a click on each of the first seven rows asks for, what applying it reports, and the row as
// drawn once the click is applied and once a second click undoes it. The reverb click changes the
// layout, so it reports LayoutChanged. The others change a setting.
struct Click {
  UiAction action;
  ui::ApplyResult result;
  const char* after;
  const char* back;
};
constexpr std::array<Click, 7> kClicks = {{
    {UiAction::ToggleInputJack, ui::ApplyResult::SettingsChanged, ">INPUT: LINE", ">INPUT: MIC"},
    {UiAction::ToggleOutput, ui::ApplyResult::SettingsChanged, ">OUTPUT: MIC", ">OUTPUT: LINE"},
    {UiAction::CycleTopSwitch, ui::ApplyResult::SettingsChanged, ">TOP SWITCH: BYPASS", ">TOP SWITCH: MUTE"},
    {UiAction::SwapReverb, ui::ApplyResult::LayoutChanged, ">REVERB: CHASM", ">REVERB: SPRING"},
    {UiAction::TogglePolishComp, ui::ApplyResult::SettingsChanged, ">POLISH COMP: OFF", ">POLISH COMP: ON"},
    {UiAction::TogglePolishEq, ui::ApplyResult::SettingsChanged, ">POLISH EQ: OFF", ">POLISH EQ: ON"},
    {UiAction::ToggleDebug, ui::ApplyResult::SettingsChanged, ">DEBUG: ON", ">DEBUG: OFF"},
}};

// Opens the list and clicks each of the first seven rows once, top to bottom, applying each action.
// Those rows all show without scrolling, so item r is on list row r. Returns the settings the clicks
// left; the layout stays in `m`.
Settings clickEveryRow(MenuRig& m) {
  m.step(0, true);
  for (int r = kInput; r <= kDebug; ++r) {
    if (r > kInput) m.step(1, false);
    const UiAction a = m.step(0, true);
    check(a == kClicks[r].action && ui::applyAction(a, m.settings, m.layout) == kClicks[r].result, kClicks[r].after);
    m.draw();
    check(listRowIs(m.canvas, r, kClicks[r].after, true), kClicks[r].after);
  }
  return m.settings;
}

// Each of the first seven rows asks for its action, applying it flips the row's setting or swaps the
// reverb card, and a second click flips it back.
void checkMenuClicks() {
  MenuRig m;
  const Settings flipped = clickEveryRow(m);
  check(!flipped.polishComp && !flipped.polishEq && flipped.outputLevel == OutputLevel::Mic &&
            flipped.inputJack == InputJack::Line && flipped.topSwitch == TopSwitch::Bypass && flipped.debug,
        "menu: six clicks flipped the six settings");
  check(m.layout.card[7] == Card::ReverbChasm, "menu: the reverb click swapped the reverb card");
  for (int r = kDebug; r >= kInput; --r) {
    if (r < kDebug) m.step(-1, false);
    const UiAction a = m.step(0, true);
    ui::applyAction(a, m.settings, m.layout);
    m.draw();
    check(listRowIs(m.canvas, r, kClicks[r].back, true), kClicks[r].back);
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
    const char* row;
  };
  Layout chasm = defaultLayout();
  chasm.card[7] = Card::ReverbChasm;
  Layout none = defaultLayout();
  none.card[7] = Card::Empty;
  Layout moved = defaultLayout();  // the reverb card in slot 1, with Empty where it was
  moved.card[0] = Card::ReverbChasm;
  moved.card[7] = Card::Empty;
  const Case cases[] = {
      {"Spring", defaultLayout(), ">REVERB: SPRING"},
      {"Chasm", chasm, ">REVERB: CHASM"},
      {"no reverb card", none, ">REVERB: NONE"},
      {"Chasm in slot 1", moved, ">REVERB: CHASM"},
  };
  for (const Case& c : cases) {
    MenuRig m;
    m.layout = c.layout;
    m.openListAt(kReverb);
    m.draw();
    const std::string name = std::string("menu: the REVERB row reads ") + c.row + " for " + c.name;
    check(listRowIs(m.canvas, kReverb, c.row, true), name.c_str());
  }
}

// A click on REVERB swaps the card in the slot that holds it, between Spring and Chasm, and leaves
// every other slot alone. With no reverb card it asks for the swap and nothing changes.
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
  check(listRowIs(m.canvas, kReverb, ">REVERB: CHASM", true), "menu: the row shows CHASM after the swap");

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

  MenuRig none;
  none.layout.card[7] = Card::Empty;
  none.openListAt(kReverb);
  const Layout noneBefore = none.layout;
  const Settings settingsBefore = none.settings;
  const UiAction click = none.step(0, true);
  check(click == UiAction::SwapReverb && ui::applyAction(click, none.settings, none.layout) == ui::ApplyResult::None &&
            none.layout == noneBefore && std::memcmp(&none.settings, &settingsBefore, sizeof(Settings)) == 0,
        "menu: with no reverb card a click changes nothing");
  none.draw();
  check(listRowIs(none.canvas, kReverb, ">REVERB: NONE", true), "menu: and the row still reads NONE");
}

// The DEBUG row toggles its setting, shows OFF by default and keeps its place below POLISH EQ.
void checkMenuDebugRow() {
  MenuRig m;
  m.openListAt(kDebug);
  m.draw();
  check(listRowIs(m.canvas, kDebug, ">DEBUG: OFF", true) && !m.settings.debug, "menu: DEBUG starts OFF");
  const UiAction a = m.step(0, true);
  check(a == UiAction::ToggleDebug && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::SettingsChanged &&
            m.settings.debug,
        "menu: a click on DEBUG turns it ON");
  m.draw();
  check(listRowIs(m.canvas, kDebug, ">DEBUG: ON", true), "menu: the row shows ON");
  ui::applyAction(m.step(0, true), m.settings, m.layout);
  check(!m.settings.debug, "menu: a second click turns it OFF");
}

// The HPF and LPF rows sit after DEBUG and show the default cutoffs. A click steps to the next
// cutoff in the row's list and wraps after the last, so one lap of clicks returns to the default.
void checkMenuCutoffRows() {
  check(kHpf == kDebug + 1 && kLpf == kHpf + 1 && kUpdate == kLpf + 1,
        "menu: HPF and LPF follow DEBUG, then UPDATE FIRMWARE");
  static const char* const kHpfAfterClick[] = {"150", "OFF", "80", "100", "120"};
  static const char* const kLpfAfterClick[] = {"OFF", "4k", "5k", "6k", "8k", "10k", "12k"};
  check(sizeof(kHpfAfterClick) / sizeof(kHpfAfterClick[0]) == static_cast<size_t>(cv::kHpfCount) &&
            sizeof(kLpfAfterClick) / sizeof(kLpfAfterClick[0]) == static_cast<size_t>(cv::kLpfCount),
        "menu: one lap of clicks covers each list");

  MenuRig m;
  m.openListAt(kHpf);
  m.draw();
  check(listRowIs(m.canvas, rowOnScreen(kHpf), ">HPF: 120", true), "menu: HPF starts at 120");
  for (int i = 0; i < cv::kHpfCount; ++i) {
    const UiAction a = m.step(0, true);
    const std::string row = std::string(">HPF: ") + kHpfAfterClick[i];
    check(a == UiAction::CycleHpf && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::SettingsChanged,
          (row + " click asks for the next cutoff").c_str());
    m.draw();
    check(listRowIs(m.canvas, rowOnScreen(kHpf), row.c_str(), true), row.c_str());
  }
  check(m.settings.hpf == cv::kHpfDefaultIndex, "menu: one lap of clicks returns HPF to 120");

  m.step(1, false);
  m.draw();
  check(listRowIs(m.canvas, rowOnScreen(kLpf), ">LPF: 12k", true), "menu: LPF starts at 12k");
  for (int i = 0; i < cv::kLpfCount; ++i) {
    const UiAction a = m.step(0, true);
    const std::string row = std::string(">LPF: ") + kLpfAfterClick[i];
    check(a == UiAction::CycleLpf && ui::applyAction(a, m.settings, m.layout) == ui::ApplyResult::SettingsChanged,
          (row + " click asks for the next cutoff").c_str());
    m.draw();
    check(listRowIs(m.canvas, rowOnScreen(kLpf), row.c_str(), true), row.c_str());
  }
  check(m.settings.lpf == cv::kLpfDefaultIndex && m.layout == defaultLayout() && m.settings.polishComp &&
            m.settings.polishEq && !m.settings.debug,
        "menu: one lap of clicks returns LPF to 12k and touches no other setting");
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
  check(listRowIs(again.canvas, rowOnScreen(kHpf), ">HPF: 150", true), "menu: after the reload the HPF row reads 150");
  again.step(1, false);
  again.draw();
  check(listRowIs(again.canvas, rowOnScreen(kLpf), ">LPF: 4k", true), "menu: and the LPF row reads 4k");

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
  m.openListAt(kRowCount + 2);  // a full lap and two more detents lands on row 2
  m.draw();
  check(listRowIs(m.canvas, kTopSwitch, ">TOP SWITCH: MUTE", true), "menu: +14 detents wraps around the list");
  m.step(-(kRowCount + 2), false);  // back by the same lands on row 0
  m.draw();
  check(listRowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: -14 detents wraps the other way");

  MenuRig n;
  n.openListAt(0);
  check(n.step(1, true) == UiAction::ToggleOutput, "menu: a turn and a click in one pass act on the new cursor");
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
  check(listRowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: click leaves the chain view for the list");

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
  check(listRowIs(m.canvas, kVisible - 1, ">UPDATE FIRMWARE >", true),
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
  check(listRowIs(m.canvas, kVisible - 1, ">EXIT", true), "menu: the cursor sits on Exit, the last visible row");
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
  check(listRowIs(m.canvas, 0, ">INPUT: MIC", true) && m.menu.open(), "menu: still open before the timeout");
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
  for (int r = 0; r < kVisible; ++r) listRows = listRows && listRowIs(m.canvas, r, defaultRow(r, r == kInput).c_str(), r == kInput);
  check(listRows, "screens: menu rows");
  writePgm(m.canvas, "oled-menu.pgm");

  MenuRig scrolled;
  scrolled.openListAt(kExit);
  scrolled.draw();
  check(sameCanvas(scrolled.canvas, listCanvas(4, kExit)), "screens: scrolled menu rows");
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
