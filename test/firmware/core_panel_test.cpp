// Checks the panel test mode on the host. Covers pot travel, toggle edges, range limits and
// the test pattern drawn on the canvas.

#include "support.h"

#include "core/canvas.h"
#include "core/panel_test.h"
#include "core/slots.h"

namespace {

bool pixelOn(const Canvas& c, int x, int y) { return (c.page(y / 8)[x] >> (y % 8)) & 1; }

void checkPanelTestPots() {
  PanelTest t;
  check(t.potsDone() == 0 && t.togglesDone() == 0 && !t.updatePot(0, 0.5f, 0), "panel test: a new test starts empty");
  t.reset();
  check(!t.updatePot(3, 0.5f, 0), "panel test: the first sample is not movement");
  check(t.updatePot(3, 0.6f, 100), "panel test: a step past the print threshold reports");
  check(!t.updatePot(3, 0.61f, 200), "panel test: a small move stays quiet");
  check(!t.updatePot(3, 0.7f, 105), "panel test: prints are rate limited per pot");
  check(t.updatePot(3, 0.7f, 150), "panel test: a held-back move reports once the gap passes");
  check(t.updatePot(3, 0.02f, 300), "panel test: reaching the low end reports");
  check(!t.potDone(3), "panel test: one end is not a full travel");
  check(t.updatePot(3, 0.99f, 400), "panel test: reaching the high end reports");
  check(t.potDone(3), "panel test: both ends make a full travel");
  for (int i = 0; i < kPanelTestPots; ++i) {
    t.updatePot(i, 0.0f, 0);
    t.updatePot(i, 1.0f, 1000);
  }
  check(t.potsDone() == kPanelTestPots, "panel test: all pots done");
  t.reset();
  check(t.potsDone() == 0, "panel test: reset forgets the travel");
}

void checkPanelTestToggles() {
  PanelTest t;
  check(!t.updateToggle(2, true), "panel test: the first toggle sample is not an edge");
  check(!t.toggleDone(2), "panel test: one state is not both");
  check(!t.updateToggle(2, true), "panel test: a steady toggle has no edge");
  check(t.updateToggle(2, false), "panel test: a toggle change is an edge");
  check(t.toggleDone(2), "panel test: both states seen");
  for (int s = 0; s < kPanelTestToggles; ++s) {
    t.updateToggle(s, true);
    t.updateToggle(s, false);
  }
  check(t.togglesDone() == kPanelTestToggles, "panel test: all toggles done");
}

void checkPanelTestBounds() {
  PanelTest t;
  check(!t.updatePot(-1, 0.5f, 0) && !t.updatePot(kPanelTestPots, 0.5f, 0), "panel test: an out-of-range pot is ignored");
  check(!t.updateToggle(-1, true) && !t.updateToggle(kPanelTestToggles, true), "panel test: an out-of-range toggle is ignored");
  check(!t.potDone(-1) && !t.potDone(kPanelTestPots) && !t.toggleDone(-1) && !t.toggleDone(kPanelTestToggles),
        "panel test: an out-of-range control is never done");
  check(kPanelTestPots == kPotCount && kPanelTestToggles == kSlotCount, "panel test: sizes follow the slot geometry");
}

void checkPanelTestPattern() {
  Canvas c;
  drawPanelTestPattern(c);
  bool border = true;
  for (int x = 0; x < kCanvasWidth; ++x) border = border && pixelOn(c, x, 0) && pixelOn(c, x, 63);
  for (int y = 0; y < kCanvasHeight; ++y) border = border && pixelOn(c, 0, y) && pixelOn(c, 127, y);
  check(border, "panel pattern: 1 px border");
  check(pixelOn(c, 100, 4) && !pixelOn(c, 104, 4) && pixelOn(c, 104, 8), "panel pattern: checkerboard cells alternate");
  auto inked = [&](int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x)
        if (pixelOn(c, x, y)) return true;
    return false;
  };
  check(inked(4, 4, 40, 11), "panel pattern: 5x7 line");
  check(inked(4, 16, 40, 28), "panel pattern: 9x12 line");
  check(inked(4, 34, 90, 58), "panel pattern: 16x24 line");
  check(!inked(60, 59, 120, 63), "panel pattern: nothing below the text");
}

}  // namespace

void checkPanelTest() {
  checkPanelTestPots();
  checkPanelTestToggles();
  checkPanelTestBounds();
  checkPanelTestPattern();
}
