// What the menu and a display backend share. The menu draws into a Canvas and lays 5x7 text out on
// a grid of kDisplayRows rows (one canvas page each) by kDisplayCols columns. A backend pushes the
// canvas to a panel or a log.

#pragma once

#include "core/canvas.h"
#include "core/fonts.h"

constexpr int kDisplayRows = kCanvasPages;
constexpr int kDisplayRowPx = kCanvasHeight / kDisplayRows;
constexpr int kDisplayCols = kCanvasWidth / kFont5x7Advance;

class Display {
 public:
  virtual ~Display() = default;

  // Sends at most one dirty page of `canvas` and clears its dirty bit. Called once per loop pass.
  virtual void flushStep(Canvas& canvas) = 0;
};
