// Tells a click from a hold on the MENU button. A press that ends before kMenuHoldMs is a click,
// reported on the release. A press that reaches kMenuHoldMs is a hold, reported once at that moment,
// and its release reports nothing.

#pragma once

#include <stdint.h>

#include "core/timing.h"

struct MenuButtonEvent {
  bool click = false;
  bool hold = false;
};

class MenuButton {
 public:
  // Sets the level the button starts at. A button already down, as after a power-on hold, reports
  // nothing until it is released and pressed again.
  void reset(bool down) {
    down_ = down;
    spent_ = down;
  }

  // Takes the debounced level, true while the button is down, and the time of this pass.
  MenuButtonEvent update(bool down, uint32_t nowMs);

  // Drops the press now under way, so it reports neither a click nor a hold.
  void cancel() { spent_ = true; }

 private:
  bool down_ = false;
  bool spent_ = false;  // this press already reported a hold, or was cancelled
  uint32_t pressedAtMs_ = 0;
};
