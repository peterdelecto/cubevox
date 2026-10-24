// Small decisions the sketch makes every pass, kept pure so the host test covers them.

#pragma once

#include <stdint.h>

// Decides when a changed layout, setting or panel is handed to the audio interrupt. Before every pot
// has been read the knob values are zero, so a publish waits and stays requested.
class PublishGate {
 public:
  // A publish is needed even though no knob moved (new layout, bench override).
  void markStale() { stale_ = true; }

  // True when the caller should publish now. `panelSeeded` is whether every pot has been read;
  // `changed` is whether the panel or a setting changed this pass.
  bool take(bool panelSeeded, bool changed) {
    if (!changed && !stale_) return false;
    stale_ = !panelSeeded;
    return panelSeeded;
  }

 private:
  bool stale_ = false;
};

// Reports the pass on which a condition stops holding.
class FallingEdge {
 public:
  // `holds` is whether the condition holds this pass. True once, on the first pass after it stops.
  bool update(bool holds) {
    const bool fell = held_ && !holds;
    held_ = holds;
    return fell;
  }

 private:
  bool held_ = false;
};

// Tells the face to repaint when something else that owned the canvas lets go of it. Its update()
// takes whether the other owner holds the canvas this pass.
using CanvasHandoff = FallingEdge;

// A planned reset (board/planned_reset) waits this long at most for the flash stores, counting
// only the time no erase runs.
constexpr uint32_t kResetWaitMs = 3000;

// Ends the wait before a planned reset. The reset goes once both flash stores are idle, or gives up
// after kResetWaitMs. A running erase restarts the count, because a reset would leave its sector
// half erased, and the flash driver bounds every erase.
class ResetWait {
 public:
  enum class Verdict : uint8_t { Wait, Go, GiveUp };

  void start(uint32_t nowMs) { sinceMs_ = nowMs; }

  // `busy` is whether either store has work queued or in flight; `erasing`, whether an erase runs.
  Verdict update(uint32_t nowMs, bool busy, bool erasing) {
    if (!busy) return Verdict::Go;
    if (erasing) {
      sinceMs_ = nowMs;
      return Verdict::Wait;
    }
    return nowMs - sinceMs_ < kResetWaitMs ? Verdict::Wait : Verdict::GiveUp;
  }

 private:
  uint32_t sinceMs_ = 0;
};
