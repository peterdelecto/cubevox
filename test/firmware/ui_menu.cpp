// Checks the OLED menu on the host. Covers the idle screen, the list and its click actions, the
// settings those clicks save and play, the chain and update screens, the timeout, redraw, the
// screenshot files and the text grid.

#include "support.h"

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

std::string chainRow(const Layout& layout, int slot) {
  char line[kDisplayCols + 1];
  std::snprintf(line, sizeof(line), "%d %s", slot + 1, cardDesc(layout.card[slot]).name);
  return line;
}

bool pagesClean(const Canvas& canvas) { return canvas.dirtyPages() == 0; }

struct MenuRig {
  Menu menu;
  Layout layout = defaultLayout();
  PanelReading panel;
  Settings settings;
  Canvas canvas;
  bool muted = false;
  uint32_t nowMs = 100;

  UiAction step(int detents, bool click, uint32_t advanceMs = 10) {
    nowMs += advanceMs;
    UiInput in;
    in.detents = detents;
    in.click = click;
    return menu.update(in, nowMs);
  }
  void draw() { menu.draw(UiContext{layout, panel, settings, muted}, canvas); }

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

void checkMenuIdle() {
  MenuRig m;
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: idle title");
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  check(rowIs(m.canvas, 7, tag) && std::strchr(tag, '-') == nullptr, "menu: idle shows the version tag only");
  check(rowIs(m.canvas, 1, ""), "menu: no MUTED row while the output plays");
  m.muted = true;
  m.draw();
  check(rowIs(m.canvas, 1, "MUTED"), "menu: the MUTED row shows while muted");
  m.muted = false;
  m.draw();
  check(rowIs(m.canvas, 1, ""), "menu: the MUTED row clears when the mute lifts");
}

void checkMenuIdleReadout() {
  MenuRig m;
  m.panel.lastMoved.slot = 5;  // Distortion
  m.panel.knob[5][0] = 0.6f;
  m.panel.toggle[5] = true;
  m.draw();
  check(rowIs(m.canvas, 3, "Distortion") && rowIs(m.canvas, 4, "Drive"), "menu: readout names the card and knob");
  check(rowIs(m.canvas, 5, "60 %") && rowIs(m.canvas, 6, "ON"), "menu: readout shows the value and the toggle");
  m.panel.toggle[5] = false;
  m.draw();
  check(rowIs(m.canvas, 6, "OFF"), "menu: readout shows the toggle off");

  m.layout.card[0] = Card::Empty;
  m.panel.lastMoved.slot = 0;
  m.draw();
  check(rowIs(m.canvas, 3, "Empty") && rowIs(m.canvas, 4, "") && rowIs(m.canvas, 5, ""), "menu: an Empty slot reads as a bare card name");

  m.panel.lastMoved.slot = kSlotCount;
  m.draw();
  check(rowIs(m.canvas, 3, "") && rowIs(m.canvas, 6, "") && rowIs(m.canvas, 0, "cubevox"),
        "menu: a last-moved slot out of range draws no readout");
}

void checkMenuList() {
  MenuRig m;
  m.step(0, true);
  m.draw();
  for (int r = 0; r < kRowCount; ++r) {
    const std::string want = defaultRow(r, r == kInput);
    check(rowIs(m.canvas, r, want.c_str(), r == kInput), want.c_str());
  }
  check(kRowCount == kDisplayRows - 1 && rowIs(m.canvas, kDisplayRows - 1, ""),
        "menu: seven rows with no title row, row 7 blank");

  m.step(-1, false);
  m.draw();
  check(rowIs(m.canvas, kExit, ">EXIT", true) && rowIs(m.canvas, kInput, " INPUT: MIC"),
        "menu: turning back from 0 wraps to the last row");
  m.step(1, false);
  m.draw();
  check(rowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: turning forward wraps to 0");
}

// Each of the first five rows asks for its action, and applying it flips the row's setting.
void checkMenuClicks() {
  struct Click {
    UiAction action;
    const char* after;  // the row as drawn once the click is applied
    const char* back;   // and once a second click undoes it
  };
  const Click clicks[] = {
      {UiAction::ToggleInputJack, ">INPUT: LINE", ">INPUT: MIC"},
      {UiAction::ToggleOutput, ">OUTPUT: MIC", ">OUTPUT: LINE"},
      {UiAction::CycleTopSwitch, ">TOP SWITCH: BYPASS", ">TOP SWITCH: MUTE"},
      {UiAction::TogglePolishComp, ">POLISH COMP: OFF", ">POLISH COMP: ON"},
      {UiAction::TogglePolishEq, ">POLISH EQ: OFF", ">POLISH EQ: ON"},
  };
  MenuRig m;
  m.step(0, true);
  for (int r = kInput; r <= kPolishEq; ++r) {
    if (r > kInput) m.step(1, false);
    const UiAction a = m.step(0, true);
    check(a == clicks[r].action && ui::applyAction(a, m.settings) == ui::ApplyResult::SettingsChanged, clicks[r].after);
    m.draw();
    check(rowIs(m.canvas, r, clicks[r].after, true), clicks[r].after);
  }
  const Settings flipped = m.settings;
  check(!flipped.polishComp && !flipped.polishEq && flipped.outputLevel == OutputLevel::Mic &&
            flipped.inputJack == InputJack::Line && flipped.topSwitch == TopSwitch::Bypass,
        "menu: five clicks flipped the five settings");

  // What the box saves and plays is what the menu showed.
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

  for (int r = kPolishEq; r >= kInput; --r) {
    if (r < kPolishEq) m.step(-1, false);
    const UiAction a = m.step(0, true);
    ui::applyAction(a, m.settings);
    m.draw();
    check(rowIs(m.canvas, r, clicks[r].back, true), clicks[r].back);
  }
  check(m.settings.polishComp && m.settings.polishEq && m.settings.outputLevel == OutputLevel::Line &&
            m.settings.inputJack == InputJack::Mic && m.settings.topSwitch == TopSwitch::Mute,
        "menu: a second click on each row restores the defaults");
}

void checkMenuWrapAndCombo() {
  MenuRig m;
  m.openListAt(9);  // 9 detents past the cursor wraps to row 2
  m.draw();
  check(rowIs(m.canvas, kTopSwitch, ">TOP SWITCH: MUTE", true), "menu: +9 detents wraps around the list");
  m.step(-9, false);  // back by 9 lands on row 0
  m.draw();
  check(rowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: -9 detents wraps the other way");

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
  check(rowIs(m.canvas, kInput, ">INPUT: MIC", true), "menu: click leaves the chain view for the list");

  m.step(kUpdate, false);
  m.step(0, true);
  m.draw();
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  const std::string have = std::string("You have ") + tag + ".";
  check(rowIs(m.canvas, 0, "Update the firmware?") && rowIs(m.canvas, 2, have.c_str()),
        "menu: confirm screen asks and names the version on the box");
  check(rowIs(m.canvas, 4, "Click to start.") && rowIs(m.canvas, 6, "Turn to cancel."),
        "menu: confirm screen says a click starts and a turn cancels");
  check(rowIs(m.canvas, 1, "") && rowIs(m.canvas, 3, "") && rowIs(m.canvas, 5, "") && rowIs(m.canvas, 7, ""),
        "menu: confirm screen keeps a blank row between sentences");
  m.step(1, false);
  m.draw();
  check(rowIs(m.canvas, kUpdate, ">UPDATE FIRMWARE >", true), "menu: turn cancels the confirm and returns to the list");
  m.step(0, true);
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: click on confirm enters DFU");
}

void checkMenuExit() {
  MenuRig m;
  m.openListAt(kExit);
  m.draw();
  check(rowIs(m.canvas, kExit, ">EXIT", true), "menu: the cursor sits on Exit");
  check(m.step(0, true) == UiAction::None, "menu: Exit click has no action");
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: Exit returns to the idle screen");
}

void checkMenuTimeout() {
  MenuRig m;
  m.step(0, true);
  m.step(0, false, 14000);
  m.draw();
  check(rowIs(m.canvas, 0, ">INPUT: MIC", true), "menu: still open before the timeout");
  m.step(0, false, 1500);
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: 15 s without input returns to idle");

  MenuRig chain;
  chain.step(0, true);
  chain.menu.showChainView();
  chain.step(0, false, 15001);
  chain.draw();
  check(rowIs(chain.canvas, 0, "cubevox"), "menu: the chain view times out to idle");

  MenuRig dfu;
  dfu.openListAt(kUpdate);
  dfu.step(0, true);
  dfu.step(0, false, 15001);
  dfu.draw();
  check(rowIs(dfu.canvas, 0, "cubevox"), "menu: the DFU confirm times out to idle without acting");
}

void checkMenuRedraw() {
  MenuRig m;
  m.draw();
  clearAllDirty(m.canvas);
  m.draw();
  check(pagesClean(m.canvas), "menu: an unchanged frame is not redrawn");
}

void checkMenuInvalidate() {
  MenuRig m;
  m.draw();
  clearAllDirty(m.canvas);
  m.menu.invalidate();
  m.draw();
  check(!pagesClean(m.canvas), "menu: invalidate makes the next draw repaint");
}

// Writes a screen as a binary PGM at 4x, lit pixels white.
void writePgm(const Canvas& canvas, const char* name) {
  constexpr int kScale = 4;
  const std::string path = std::string(CUBEVOX_OUT_DIR) + "/" + name;
  std::FILE* file = std::fopen(path.c_str(), "wb");
  check(file != nullptr, "pgm: output file opens");
  if (!file) return;
  std::fprintf(file, "P5\n%d %d\n255\n", kCanvasWidth * kScale, kCanvasHeight * kScale);
  for (int y = 0; y < kCanvasHeight * kScale; ++y) {
    const int py = y / kScale;
    for (int x = 0; x < kCanvasWidth * kScale; ++x) {
      const bool on = (canvas.page(py / 8)[x / kScale] >> (py % 8)) & 1;
      std::fputc(on ? 0xFF : 0x00, file);
    }
  }
  std::fclose(file);
}

void checkScreenshots() {
  MenuRig m;
  m.panel.lastMoved.slot = 5;
  m.panel.knob[5][0] = 0.6f;
  m.panel.toggle[5] = true;
  m.draw();
  check(rowIs(m.canvas, 3, "Distortion") && rowIs(m.canvas, 4, "Drive") && rowIs(m.canvas, 5, "60 %") && rowIs(m.canvas, 6, "ON"),
        "screens: idle readout rows");
  writePgm(m.canvas, "oled-idle.pgm");

  m.step(0, true);
  m.draw();
  bool listRows = true;
  for (int r = 0; r < kRowCount; ++r) listRows = listRows && rowIs(m.canvas, r, defaultRow(r, r == kInput).c_str(), r == kInput);
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
  checkMenuIdle();
  checkMenuIdleReadout();
  checkMenuList();
  checkMenuClicks();
  checkMenuWrapAndCombo();
  checkMenuScreens();
  checkMenuExit();
  checkMenuTimeout();
  checkMenuRedraw();
  checkMenuInvalidate();
  checkScreenshots();
  checkDisplayGrid();
}
