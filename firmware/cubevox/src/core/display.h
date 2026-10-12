// What the menu and a display backend share. The menu draws into a Canvas and lays text out on a
// grid of 8 rows (one canvas page each) by 16 columns. A backend pushes the canvas to a panel
// or a log.

#pragma once

#include "core/canvas.h"

constexpr int kDisplayRows = 8;
constexpr int kDisplayCols = 16;
constexpr int kDisplayRowPx = 8;

class Display {
 public:
  virtual ~Display() {}

  // Sends at most one dirty page of `canvas` and clears its dirty bit. Called once per loop pass.
  virtual void flushStep(Canvas& canvas) = 0;
};
