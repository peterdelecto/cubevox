// Checks the OLED menu on the host. Covers the input it drops before the face shows, the closed menu,
// the list and its click actions, the settings those clicks save and play, the chain and update
// screens, the timeout, redraw, the screenshot files, the text grid and the list's row pitch. The
// screens shown while the menu is closed are checked in ui_screens.cpp and ui_face.cpp.

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
#include "core/registry.h"
#include "core/settings.h"
#include "core/version.h"
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

// The list rows, top to bottom (spec 2026-10-24-polish-design.md).
enum Row { kInput, kOutput, kTopSwitch, kPolishComp, kPolishEq, kUpdate, kExit, kRowCount };

// List row `r` as drawn for the default settings: the cursor mark or a blank in column 0, then the text.
std::string defaultRow(int r, bool cursor) {
  static const char* const text[kRowCount] = {"INPUT: MIC",      "OUTPUT: LINE", "TOP SWITCH: MUTE",
                                              "POLISH COMP: ON", "POLISH EQ: ON", "UPDATE FIRMWARE >",
                                              "EXIT"};
  return std::string(cursor ? ">" : " ") + text[r];
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
  for (int r = 0; r < kRowCount; ++r) {
    const std::string want = defaultRow(r, r == kInput);
    check(listRowIs(m.canvas, r, want.c_str(), r == kInput), want.c_str());
  }
  check(kRowCount * kListPitch <= kCanvasHeight, "menu: seven rows with no title row fit the canvas on their pitch");

  m.step(-1, false);
  m.draw();
  check(listRowIs(m.canvas, kExit, ">EXIT", true) && listRowIs(m.canvas, kInput, " INPUT: MIC"),
        "menu: turning back from 0 wraps to the last row");
  m.step(1, false);
  m.draw();
  check(listRowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: turning forward wraps to 0");
}

// The list draws its seven rows on a 9 px pitch (owner 2026-10-24). Each 7 px text starts 1 px below
// its row top, so the cursor bar, the 9 px row, keeps 1 px clear above and below the text (owner
// 2026-10-12). The last pixel line stays blank. The expected canvas is composed from literal y values.
void checkMenuListPitch() {
  static const int kTextY[kRowCount] = {1, 10, 19, 28, 37, 46, 55};
  const auto listWithCursorOn = [](int cursor) {
    Canvas want;
    for (int r = 0; r < kRowCount; ++r) want.drawText(0, kTextY[r], font5x7(), defaultRow(r, r == cursor).c_str());
    want.invertRect(0, kTextY[cursor] - 1, kCanvasWidth, kListPitch);
    return want;
  };

  MenuRig m;
  m.step(0, true);
  m.draw();
  check(sameCanvas(m.canvas, listWithCursorOn(kInput)), "menu: the list text starts at y 1, 10, 19, 28, 37, 46 and 55");
  m.step(kExit, false);
  m.draw();
  check(sameCanvas(m.canvas, listWithCursorOn(kExit)), "menu: the cursor bar follows to the last row, lines 54 to 62");
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

// What a click on each of the first five rows asks for, and the row as drawn once the click is
// applied and once a second click undoes it.
struct Click {
  UiAction action;
  const char* after;
  const char* back;
};
constexpr std::array<Click, 5> kClicks = {{
    {UiAction::ToggleInputJack, ">INPUT: LINE", ">INPUT: MIC"},
    {UiAction::ToggleOutput, ">OUTPUT: MIC", ">OUTPUT: LINE"},
    {UiAction::CycleTopSwitch, ">TOP SWITCH: BYPASS", ">TOP SWITCH: MUTE"},
    {UiAction::TogglePolishComp, ">POLISH COMP: OFF", ">POLISH COMP: ON"},
    {UiAction::TogglePolishEq, ">POLISH EQ: OFF", ">POLISH EQ: ON"},
}};

// Opens the list and clicks each of the first five rows once, top to bottom, applying each action.
// Returns the settings the clicks left.
Settings clickEveryRow(MenuRig& m) {
  m.step(0, true);
  for (int r = kInput; r <= kPolishEq; ++r) {
    if (r > kInput) m.step(1, false);
    const UiAction a = m.step(0, true);
    check(a == kClicks[r].action && ui::applyAction(a, m.settings) == ui::ApplyResult::SettingsChanged, kClicks[r].after);
    m.draw();
    check(listRowIs(m.canvas, r, kClicks[r].after, true), kClicks[r].after);
  }
  return m.settings;
}

// Each of the first five rows asks for its action, applying it flips the row's setting, and a second
// click flips it back.
void checkMenuClicks() {
  MenuRig m;
  const Settings flipped = clickEveryRow(m);
  check(!flipped.polishComp && !flipped.polishEq && flipped.outputLevel == OutputLevel::Mic &&
            flipped.inputJack == InputJack::Line && flipped.topSwitch == TopSwitch::Bypass,
        "menu: five clicks flipped the five settings");
  for (int r = kPolishEq; r >= kInput; --r) {
    if (r < kPolishEq) m.step(-1, false);
    const UiAction a = m.step(0, true);
    ui::applyAction(a, m.settings);
    m.draw();
    check(listRowIs(m.canvas, r, kClicks[r].back, true), kClicks[r].back);
  }
  check(m.settings.polishComp && m.settings.polishEq && m.settings.outputLevel == OutputLevel::Line &&
            m.settings.inputJack == InputJack::Mic && m.settings.topSwitch == TopSwitch::Mute,
        "menu: a second click on each row restores the defaults");
}

// What the box saves and plays is what the menu showed.
void checkMenuClicksSavedAndPlayed() {
  MenuRig m;
  const Settings flipped = clickEveryRow(m);
  RecordData saved;
  saved.settings = flipped;
  RecordData loaded;
  check(decodeRecord(encodeRecord(saved).data(), loaded) && loaded.settings.polishComp == flipped.polishComp &&
            loaded.settings.polishEq == flipped.polishEq && loaded.settings.outputLevel == flipped.outputLevel &&
            loaded.settings.inputJack == flipped.inputJack && loaded.settings.topSwitch == flipped.topSwitch,
        "menu: the flipped settings survive a save and a reload");
  const ChainSnapshot snap = composeSnapshot(m.layout, m.panel, flipped, BenchOverrides());
  check(!snap.params.comp.on && !snap.params.eq.on && snap.outputGain < 0.3f,
        "menu: the flipped settings reach the audio snapshot");
  const ChainSnapshot fresh = composeSnapshot(m.layout, m.panel, Settings(), BenchOverrides());
  check(fresh.params.comp.on && fresh.params.eq.on && fresh.outputGain == 1.0f, "menu: the default snapshot has both polish stages on");
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
  m.openListAt(9);  // 9 detents past the cursor wraps to row 2
  m.draw();
  check(listRowIs(m.canvas, kTopSwitch, ">TOP SWITCH: MUTE", true), "menu: +9 detents wraps around the list");
  m.step(-9, false);  // back by 9 lands on row 0
  m.draw();
  check(listRowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: -9 detents wraps the other way");

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
  check(listRowIs(m.canvas, kUpdate, ">UPDATE FIRMWARE >", true), "menu: turn cancels the confirm and returns to the list");
  m.step(0, true);
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: click on confirm enters DFU");
}

void checkMenuExit() {
  MenuRig m;
  m.openListAt(kExit);
  m.draw();
  check(listRowIs(m.canvas, kExit, ">EXIT", true), "menu: the cursor sits on Exit");
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
  for (int r = 0; r < kRowCount; ++r) listRows = listRows && listRowIs(m.canvas, r, defaultRow(r, r == kInput).c_str(), r == kInput);
  check(listRows, "screens: menu rows");
  writePgm(m.canvas, "oled-menu.pgm");

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
  checkMenuListPitch();
  checkMenuClicks();
  checkMenuClicksSavedAndPlayed();
  checkMenuWrapAndCombo();
  checkMenuScreens();
  checkOtherScreensKeepGrid();
  checkMenuExit();
  checkMenuTimeout();
  checkMenuRedraw();
  checkMenuInvalidate();
  checkScreenshots();
  checkDisplayGrid();
}
