// Checks the OLED menu on the host. Covers the idle screen, the list, the chain and update
// screens, the timeout, redraw, the screenshot files and the text grid.

#include "support.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/canvas.h"
#include "core/display.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/registry.h"
#include "core/settings.h"
#include "core/version.h"
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

  // Opens the list and turns to `item` (0 = Out, 1 = Input, 2 = Chain order, 3 = Update, 4 = Exit).
  void openListAt(int item) {
    step(0, true);
    step(item, false);
  }
};

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
  check(rowIs(m.canvas, 0, "MENU"), "menu: click opens the list");
  check(rowIs(m.canvas, 1, ">Out LINE", true) && rowIs(m.canvas, 2, " Input MIC"), "menu: cursor 0 row inverted, input row text");

  m.step(-1, false);
  m.draw();
  check(rowIs(m.canvas, 5, ">Exit", true), "menu: turning back from 0 wraps to the last item");
  m.step(1, false);
  m.draw();
  check(rowIs(m.canvas, 1, ">Out LINE", true), "menu: turning forward wraps to 0");

  check(m.step(0, true) == UiAction::ToggleOutput, "menu: Out click toggles output");
  m.step(1, false);
  check(m.step(0, true) == UiAction::ToggleInputJack, "menu: Input click toggles the jack");
  m.settings.inputJack = InputJack::Pedal;
  m.draw();
  check(rowIs(m.canvas, 2, ">Input PEDAL", true), "menu: row reads the settings");
}

void checkMenuWrapAndCombo() {
  MenuRig m;
  m.openListAt(7);  // 7 detents past the cursor wraps to item 2
  m.draw();
  check(rowIs(m.canvas, 3, ">Chain order", true), "menu: +7 detents wraps around the list");
  m.step(-7, false);  // back by 7 lands on item 0
  m.draw();
  check(rowIs(m.canvas, 1, ">Out LINE", true), "menu: -7 detents wraps the other way");

  MenuRig n;
  n.openListAt(0);
  check(n.step(1, true) == UiAction::ToggleInputJack, "menu: a turn and a click in one pass act on the new cursor");
}

void checkMenuScreens() {
  MenuRig m;
  m.openListAt(2);
  check(m.step(0, true) == UiAction::None, "menu: Chain order click has no action");
  m.draw();
  check(rowIs(m.canvas, 0, chainRow(m.layout, 0).c_str()), "menu: chain view starts at slot 1");
  check(rowIs(m.canvas, kDisplayRows - 1, chainRow(m.layout, 7).c_str()), "menu: chain view shows slot 8");
  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: click leaves the chain view");

  m.step(1, false);
  m.step(0, true);
  m.draw();
  char tag[kDisplayCols + 1];
  versionTag(kFirmwareVersion, tag, sizeof(tag));
  const std::string installed = std::string("Installed ") + tag;
  check(rowIs(m.canvas, 0, "UPDATE FIRMWARE") && rowIs(m.canvas, 2, installed.c_str()), "menu: confirm screen names the installed version");
  check(rowIs(m.canvas, 4, "Click to enter") && rowIs(m.canvas, 5, "update mode.") && rowIs(m.canvas, 7, "Turn to cancel."),
        "menu: confirm screen says a click enters update mode and a turn cancels");
  check(rowIs(m.canvas, 1, "") && rowIs(m.canvas, 3, "") && rowIs(m.canvas, 6, ""), "menu: confirm screen keeps its spacer rows blank");
  m.step(1, false);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: turn cancels the confirm");
  m.step(0, true);
  m.step(0, true);
  check(m.step(0, true) == UiAction::EnterDfu, "menu: click on confirm enters DFU");
}

void checkMenuExit() {
  MenuRig m;
  m.openListAt(4);
  m.draw();
  check(rowIs(m.canvas, 5, ">Exit", true), "menu: the cursor sits on Exit");
  check(m.step(0, true) == UiAction::None, "menu: Exit click has no action");
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: Exit returns to the idle screen");
}

void checkMenuTimeout() {
  MenuRig m;
  m.step(0, true);
  m.step(0, false, 14000);
  m.draw();
  check(rowIs(m.canvas, 0, "MENU"), "menu: still open before the timeout");
  m.step(0, false, 1500);
  m.draw();
  check(rowIs(m.canvas, 0, "cubevox"), "menu: 15 s without input returns to idle");

  MenuRig chain;
  chain.openListAt(2);
  chain.step(0, true);
  chain.step(0, false, 15001);
  chain.draw();
  check(rowIs(chain.canvas, 0, "cubevox"), "menu: the chain view times out to idle");

  MenuRig dfu;
  dfu.openListAt(3);
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
  check(rowIs(m.canvas, 0, "MENU") && rowIs(m.canvas, 1, ">Out LINE", true), "screens: menu rows");
  writePgm(m.canvas, "oled-menu.pgm");

  m.step(2, false);
  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, chainRow(m.layout, 0).c_str()) && rowIs(m.canvas, 7, chainRow(m.layout, 7).c_str()),
        "screens: chain rows");
  writePgm(m.canvas, "oled-chain.pgm");

  m.step(0, true);
  m.step(1, false);
  m.step(0, true);
  m.draw();
  check(rowIs(m.canvas, 0, "UPDATE FIRMWARE") && rowIs(m.canvas, 7, "Turn to cancel."), "screens: confirm rows");
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
  checkMenuWrapAndCombo();
  checkMenuScreens();
  checkMenuExit();
  checkMenuTimeout();
  checkMenuRedraw();
  checkMenuInvalidate();
  checkScreenshots();
  checkDisplayGrid();
}
