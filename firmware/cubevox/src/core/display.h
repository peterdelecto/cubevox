// What the menu and a display backend share. The menu draws into a Canvas and lays 5x7 text out on
// a grid of kDisplayRows rows (one canvas page each) by kDisplayCols columns. A backend pushes the
// canvas to the panel, and PanelLink decides when it may.

#pragma once

#include <stdint.h>

#include "core/canvas.h"
#include "core/fonts.h"

constexpr int kDisplayRows = kCanvasPages;
constexpr int kDisplayRowPx = kCanvasHeight / kDisplayRows;
constexpr int kDisplayCols = kCanvasWidth / kFont5x7Advance;

class Display {
 public:
  virtual ~Display() = default;

  // Sends at most one dirty page of `canvas` and clears its dirty bit once the page went through.
  // Called once per loop pass.
  virtual void flushStep(Canvas& canvas) = 0;
};

// While failures go on, the panel gets one restart per window, so a missing panel costs one short
// try a second.
constexpr uint32_t kPanelRetryMs = 1000;

// At boot the panel gets this many starts, kPanelStartGapMs apart, which covers one still powering up.
constexpr int kPanelStartTries = 5;
constexpr uint32_t kPanelStartGapMs = 20;

enum class PanelStep : uint8_t {
  Send,     // write the lowest dirty page, or turn the display on once a frame is in after a start
  Restart,  // recover the bus and send the init sequence
  Wait,     // the panel is down and its retry window has not come round
};

enum class PanelEvent : uint8_t {
  None,
  Lost,  // the first failure of a run
  Back,  // the panel took a whole frame after a run of failures
};

// The backend asks it each pass what to do and reports what each attempt did. The first failure
// restarts the panel on the next pass. A failure after that, before the panel has taken a whole frame
// again, waits for the retry window, so a failing write never holds the loop for long. A run of
// failures reports Lost once and Back once. The code is pure, so the host tests drive it with a fake
// clock.
class PanelLink {
 public:
  PanelStep step(uint32_t nowMs) const {
    if (state_ == State::Up) return PanelStep::Send;
    if (state_ == State::Restart) return PanelStep::Restart;
    return nowMs - triedMs_ >= kPanelRetryMs ? PanelStep::Restart : PanelStep::Wait;
  }

  // A start: bus recovery and the init sequence. After one that answers, the panel needs a whole
  // frame and then the display-on command.
  PanelEvent started(bool ok, uint32_t nowMs) {
    triedMs_ = nowMs;
    state_ = ok ? State::Up : State::Down;
    return ok ? PanelEvent::None : failed();
  }

  // A page write.
  PanelEvent wrote(bool ok, uint32_t nowMs) {
    if (ok) return PanelEvent::None;
    triedMs_ = nowMs;
    state_ = failing_ ? State::Down : State::Restart;
    return failed();
  }

  // The display-on command after the first whole frame since a start.
  PanelEvent shown(bool ok, uint32_t nowMs) {
    if (!ok) return wrote(false, nowMs);
    if (!failing_) return PanelEvent::None;
    failing_ = false;
    return PanelEvent::Back;
  }

  // True while the last start and the last write went through.
  bool up() const { return state_ == State::Up; }

  // Failed starts and writes since the current or last run of failures began.
  uint32_t failures() const { return failures_; }

 private:
  enum class State : uint8_t { Restart, Up, Down };

  PanelEvent failed() {
    if (failing_) {
      ++failures_;
      return PanelEvent::None;
    }
    failing_ = true;
    failures_ = 1;
    return PanelEvent::Lost;
  }

  State state_ = State::Restart;  // the first step starts the panel
  bool failing_ = false;
  uint32_t failures_ = 0;
  uint32_t triedMs_ = 0;
};
