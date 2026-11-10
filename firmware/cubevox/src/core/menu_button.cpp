#include "core/menu_button.h"

MenuButtonEvent MenuButton::update(bool down, uint32_t nowMs) {
  MenuButtonEvent ev;
  if (down && !down_) {
    pressedAtMs_ = nowMs;
    spent_ = false;
    ev.press = true;
  } else if (down && !spent_ && nowMs - pressedAtMs_ >= kMenuHoldMs) {
    ev.hold = true;
    spent_ = true;
  } else if (!down && down_ && !spent_) {
    ev.click = true;
  }
  down_ = down;
  return ev;
}
