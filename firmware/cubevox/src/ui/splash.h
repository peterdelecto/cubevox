// Boot splash (spec 2026-10-24, OLED in update mode item 3): the version, centred, for kSplashMs
// after setup() ends, so Adam can confirm an update on the box itself.

#pragma once

#include <stdint.h>

#include "core/canvas.h"
#include "core/loop_policy.h"

constexpr uint32_t kSplashMs = 2000;

// True from the start until kSplashMs have passed. The unsigned subtraction stays right across a
// millis() wrap.
constexpr bool splashUp(bool started, uint32_t startMs, uint32_t nowMs) {
  return started && nowMs - startMs < kSplashMs;
}

// Who owns the canvas this pass, in the words of BenchPattern. Up: the splash is showing and the
// caller skips its own draw. Ended: it let go on this pass, so the caller repaints everything.
// Off: the caller draws as usual.
enum class SplashStep : uint8_t { Off, Up, Ended };

class Splash {
 public:
  // Starts the timer. Call once, as setup() ends.
  void start(uint32_t nowMs) {
    startMs_ = nowMs;
    started_ = true;
    painted_ = false;
  }

  // Call every pass before the face draws. Paints `version` once, when the splash comes up.
  SplashStep draw(uint32_t nowMs, const char* version, Canvas& canvas);

 private:
  uint32_t startMs_ = 0;
  bool started_ = false;
  bool painted_ = false;
  CanvasHandoff handoff_;
};
