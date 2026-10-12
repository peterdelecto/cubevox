#include "core/panel_test.h"

#include <math.h>

#include "core/canvas.h"
#include "core/fonts.h"

void PanelTest::reset() {
  for (int i = 0; i < kPanelTestPots; ++i) {
    min_[i] = 1.0f;
    max_[i] = 0.0f;
    printed_[i] = 0.0f;
    printedAtMs_[i] = 0;
    potSeen_[i] = false;
  }
  for (int i = 0; i < kPanelTestToggles; ++i) {
    seenOn_[i] = false;
    seenOff_[i] = false;
    toggleLevel_[i] = false;
    toggleSeen_[i] = false;
  }
}

bool PanelTest::updatePot(int pot, float value, uint32_t nowMs) {
  const bool first = !potSeen_[pot];
  const bool hadLow = min_[pot] < kPanelTestLow;
  const bool hadHigh = max_[pot] > kPanelTestHigh;
  potSeen_[pot] = true;
  if (value < min_[pot]) min_[pot] = value;
  if (value > max_[pot]) max_[pot] = value;

  const bool newEnd = (!hadLow && min_[pot] < kPanelTestLow) || (!hadHigh && max_[pot] > kPanelTestHigh);
  const bool moved = fabsf(value - printed_[pot]) > kPanelTestPrintStep;
  const bool spaced = nowMs - printedAtMs_[pot] >= kPanelTestPrintGapMs;
  if (first) {
    printed_[pot] = value;
    printedAtMs_[pot] = nowMs;
    return false;  // the starting position is not movement
  }
  if (!(newEnd || (moved && spaced))) return false;
  printed_[pot] = value;
  printedAtMs_[pot] = nowMs;
  return true;
}

bool PanelTest::updateToggle(int slot, bool on) {
  const bool first = !toggleSeen_[slot];
  const bool edge = !first && on != toggleLevel_[slot];
  toggleSeen_[slot] = true;
  toggleLevel_[slot] = on;
  (on ? seenOn_ : seenOff_)[slot] = true;
  return edge;
}

bool PanelTest::potDone(int pot) const { return min_[pot] < kPanelTestLow && max_[pot] > kPanelTestHigh; }

int PanelTest::potsDone() const {
  int n = 0;
  for (int i = 0; i < kPanelTestPots; ++i) n += potDone(i) ? 1 : 0;
  return n;
}

int PanelTest::togglesDone() const {
  int n = 0;
  for (int i = 0; i < kPanelTestToggles; ++i) n += toggleDone(i) ? 1 : 0;
  return n;
}

int PanelTest::nextPot() const {
  for (int i = 0; i < kPanelTestPots; ++i) {
    if (!potDone(i)) return i;
  }
  return -1;
}

int PanelTest::nextToggle() const {
  for (int i = 0; i < kPanelTestToggles; ++i) {
    if (!toggleDone(i)) return i;
  }
  return -1;
}

namespace {

constexpr int kCheckerX = 100;
constexpr int kCheckerY = 4;
constexpr int kCheckerCells = 6;  // cells per side
constexpr int kCheckerCell = 4;   // px per cell

void drawChecker(Canvas& canvas) {
  for (int cy = 0; cy < kCheckerCells; ++cy) {
    for (int cx = 0; cx < kCheckerCells; ++cx) {
      if ((cx + cy) % 2 == 0) {
        canvas.fillRect(kCheckerX + cx * kCheckerCell, kCheckerY + cy * kCheckerCell, kCheckerCell,
                        kCheckerCell, true);
      }
    }
  }
}

}  // namespace

void drawPanelTestPattern(Canvas& canvas) {
  canvas.clear();
  canvas.fillRect(0, 0, kCanvasWidth, kCanvasHeight, true);
  canvas.fillRect(1, 1, kCanvasWidth - 2, kCanvasHeight - 2, false);
  drawChecker(canvas);
  canvas.drawText(4, 4, font5x7(), "5X7 OK");
  canvas.drawText(4, 16, font9x12(), "9X12");
  canvas.drawText(4, 34, font16x24(), "16X24");
}
