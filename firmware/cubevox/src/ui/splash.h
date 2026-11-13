// Boot splash (spec 2026-10-24, OLED in update mode item 3): the version, centred, for at least
// kSplashMs after setup() ends and until the power-on wait ends, so Adam can confirm an update on
// the box itself and never sees MUTED while the box starts. After an audio fault reset the box, two
// lines above the version explain the error and a line below gives its code.

#pragma once

#include <stdint.h>

#include "core/audio_fault_cause.h"
#include "core/audio_recovery.h"
#include "core/canvas.h"
#include "core/display.h"
#include "core/loop_policy.h"

constexpr uint32_t kSplashMs = 2000;

// After a warm start that followed an audio error the splash stays this long, so Adam can read the
// error and its code, and ends early when he touches a knob, a toggle or MENU.
constexpr uint32_t kSplashWarmErrorMs = 6000;

// True from the start until `durationMs` have passed. The unsigned subtraction stays right across a
// millis() wrap.
constexpr bool splashUp(bool started, uint32_t startMs, uint32_t nowMs, uint32_t durationMs = kSplashMs) {
  return started && nowMs - startMs < durationMs;
}

// Who owns the canvas this pass, in the words of BenchPattern. Up: the splash is showing and the
// caller skips its own draw. Ended: it let go on this pass, so the caller repaints everything.
// Off: the caller draws as usual.
enum class SplashStep : uint8_t { Off, Up, Ended };

// What the splash says after an audio fault, in 5x7 capitals, each line centred: the explanation
// on text rows 0 and 2, above the version, and the code on row 6, below it.
struct SplashFaultText {
  const char* explanation[2];
  const char* code;
};

// A1 to A4, C1 and W1, in the order of AudioFaultCause.
constexpr SplashFaultText kSplashFaultText[] = {
    {{"THE AUDIO LINK FAILED", "4 TIMES IN A MINUTE."}, "ERROR A1"},
    {{"THE AUDIO LINK DID", "NOT START AGAIN."}, "ERROR A2"},
    {{"THE AUDIO STREAM", "STOPPED MOVING."}, "ERROR A3"},
    {{"THE AUDIO DID NOT", "START. TRYING AGAIN."}, "ERROR A4"},
    {{"THE BOX CRASHED AND", "RESTARTED ITSELF."}, "ERROR C1"},
    {{"THE BOX FROZE AND", "RESTARTED ITSELF."}, "ERROR W1"},
};
static_assert(sizeof(kSplashFaultText) / sizeof(kSplashFaultText[0]) == 6, "one text per code");

// A1 is the fault past the restart budget, the one after kRestartsPerWindow restarts.
static_assert(kRestartsPerWindow + 1 == 4 && kRestartWindowMs == 60000, "A1's text says 4 times in a minute");

constexpr int splashLineLength(const char* text) { return *text ? 1 + splashLineLength(text + 1) : 0; }

constexpr bool splashFaultTextFits() {
  for (const SplashFaultText& t : kSplashFaultText) {
    if (splashLineLength(t.explanation[0]) > kDisplayCols || splashLineLength(t.explanation[1]) > kDisplayCols ||
        splashLineLength(t.code) > kDisplayCols) {
      return false;
    }
  }
  return true;
}
static_assert(splashFaultTextFits(), "every splash fault line fits the 21 columns of a 5x7 row");

// The text for `cause`, or nullptr for None.
constexpr const SplashFaultText* splashFaultText(AudioFaultCause cause) {
  return cause == AudioFaultCause::None ? nullptr : &kSplashFaultText[static_cast<int>(cause) - 1];
}

class Splash {
 public:
  // Starts the timer for kSplashMs. Call once, as setup() ends.
  void start(uint32_t nowMs) {
    startMs_ = nowMs;
    durationMs_ = kSplashMs;
    endsOnTouch_ = false;
    touched_ = false;
    started_ = true;
    painted_ = false;
  }

  // Starts the timer for a warm start that followed an audio error: kSplashWarmErrorMs, ended early
  // by noteTouch().
  void startAfterWarmError(uint32_t nowMs) {
    start(nowMs);
    durationMs_ = kSplashWarmErrorMs;
    endsOnTouch_ = true;
  }

  // A knob, a toggle or MENU was touched. Ends a splash started by startAfterWarmError(), the
  // power-on wait or not; any other splash ignores it.
  void noteTouch() {
    if (endsOnTouch_) touched_ = true;
  }

  // Call every pass before the face draws. Paints `version`, and the text of `cause` when there is
  // one, once, when the splash comes up. The splash stays up after kSplashMs while `hold` is true
  // (the power-on wait).
  SplashStep draw(uint32_t nowMs, const char* version, Canvas& canvas, bool hold,
                  AudioFaultCause cause = AudioFaultCause::None);

 private:
  uint32_t startMs_ = 0;
  uint32_t durationMs_ = kSplashMs;
  bool endsOnTouch_ = false;
  bool touched_ = false;
  bool started_ = false;
  bool painted_ = false;
  CanvasHandoff handoff_;
};
