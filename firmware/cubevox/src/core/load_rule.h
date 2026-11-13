// The load rule (docs/specs/2026-10-31-stage-survival-design.md, section O), pure so the host test
// covers it. When the late window trips, the rule holds the running card whose slot took the most
// cycles in the window's worst block, and the snapshot then turns that card off through its slot's
// fade. Each held card comes back after its own wait once the window holds no late block. A hold also
// ends when the card's toggle goes off and when the layout changes. loop() calls update() once a pass
// and composes the held mask into the snapshot.

#pragma once

#include <stdint.h>
#include <stdio.h>

#include <array>

#include "core/late_window.h"
#include "core/layout.h"
#include "core/registry.h"
#include "core/slots.h"

// Two steps sit at least this far apart, so a step's relief shows in the next window before the rule
// looks again.
constexpr uint32_t kLoadStepSpacingMs = 50;

// A card's first wait before it comes back. Each further hold doubles it up to the cap. A card that
// has stayed in for kLoadResetAfterMs starts again at the first wait.
constexpr uint32_t kLoadWaitFirstMs = 2000;
constexpr uint32_t kLoadWaitCapMs = 16000;
constexpr uint32_t kLoadResetAfterMs = 60000;

// What one loop() pass hands the rule.
struct LoadInput {
  const Layout& layout;
  const std::array<bool, kSlotCount>& toggle;  // the panel's toggle levels, not the held snapshot's
  uint32_t nowMs;
  bool trip = false;         // the late window tripped since the last pass
  int tripLate = 0;          // late blocks in the window at that trip
  SlotCycles worstBlock{};   // the per-slot cycles of the window's worst late block
  int lateInWindow = 0;      // late blocks in the window now
  bool layoutApplied = false;  // a layout apply or a card swap happened this pass
};

// What the pass did.
struct LoadStep {
  int slot = -1;  // the slot the rule held, -1 when the pass held nothing
  Card card = Card::Empty;
  int lateCount = 0;           // late blocks in the window at the trip
  bool layoutCleared = false;  // the pass cleared every hold for a changed layout; the caller empties the window
};

class LoadRule {
 public:
  LoadStep update(const LoadInput& in) {
    LoadStep step;
    if (in.layoutApplied || (haveLayout_ && in.layout != layout_)) {
      clearAll();
      step.layoutCleared = true;
    }
    layout_ = in.layout;
    haveLayout_ = true;
    endForToggles(in.toggle);
    comeBack(in);
    if (in.trip) holdForLoad(in, step);
    return step;
  }

  // Bit s is set while slot s is held.
  uint8_t heldMask() const { return held_; }

  // Cards the rule has held since the box started.
  uint32_t steps() const { return steps_; }

 private:
  void startWaits(int slot) {
    nextWaitMs_[static_cast<size_t>(slot)] = kLoadWaitFirstMs;
    returned_ &= static_cast<uint8_t>(~(1u << slot));
  }

  void clearAll() {
    held_ = 0;
    for (int s = 0; s < kSlotCount; ++s) startWaits(s);
  }

  // A hold ends when its card's toggle goes off, so Adam's panel and the box agree again.
  void endForToggles(const std::array<bool, kSlotCount>& toggle) {
    for (int s = 0; s < kSlotCount; ++s) {
      if ((held_ & (1u << s)) && !toggle[static_cast<size_t>(s)]) {
        held_ &= static_cast<uint8_t>(~(1u << s));
        startWaits(s);
      }
    }
  }

  // A held card comes back when its wait is up and the window holds no late block.
  void comeBack(const LoadInput& in) {
    if (in.lateInWindow > 0) return;
    for (int s = 0; s < kSlotCount; ++s) {
      const size_t i = static_cast<size_t>(s);
      if (!(held_ & (1u << s)) || in.nowMs - heldAtMs_[i] < waitMs_[i]) continue;
      held_ &= static_cast<uint8_t>(~(1u << s));
      returnedAtMs_[i] = in.nowMs;
      returned_ |= static_cast<uint8_t>(1u << s);
    }
  }

  // Slots whose cards are running: toggle on, a card placed, not held.
  uint8_t runningMask(const LoadInput& in) const {
    uint8_t mask = 0;
    for (int s = 0; s < kSlotCount; ++s) {
      const bool on = in.toggle[static_cast<size_t>(s)] && !(held_ & (1u << s));
      if (on && in.layout.card[static_cast<size_t>(s)] != Card::Empty) mask |= static_cast<uint8_t>(1u << s);
    }
    return mask;
  }

  // The running card whose slot took the most cycles in the worst block, the later slot on a tie. The
  // Input gate is never a candidate, and Autotune is passed over while a card behind it reads its
  // tracker, because holding it leaves its engine running for that card.
  int heaviestRunning(const LoadInput& in) const {
    const uint8_t running = runningMask(in);
    int best = -1;
    uint32_t bestCycles = 0;
    for (int s = 0; s < kSlotCount; ++s) {
      if (!(running & (1u << s))) continue;
      const Card c = in.layout.card[static_cast<size_t>(s)];
      if (c == Card::InputGate) continue;
      if (c == Card::Autotune && autotuneHasReader(in.layout, s, running)) continue;
      const uint32_t cycles = in.worstBlock[static_cast<size_t>(s)];
      if (best < 0 || cycles >= bestCycles) {
        best = s;
        bestCycles = cycles;
      }
    }
    return best;
  }

  // Starts slot's wait: the first one, or double the last up to the cap. A card that has stayed in for
  // a minute starts again at the first.
  void startHold(const LoadInput& in, int slot) {
    const size_t i = static_cast<size_t>(slot);
    if ((returned_ & (1u << slot)) && in.nowMs - returnedAtMs_[i] >= kLoadResetAfterMs) startWaits(slot);
    waitMs_[i] = nextWaitMs_[i];
    nextWaitMs_[i] = nextWaitMs_[i] * 2 > kLoadWaitCapMs ? kLoadWaitCapMs : nextWaitMs_[i] * 2;
    heldAtMs_[i] = in.nowMs;
    held_ |= static_cast<uint8_t>(1u << slot);
  }

  // A trip inside the spacing is dropped. The window restarts empty, so a load that stays over the
  // limit trips again. With nothing left to hold the rule does nothing.
  void holdForLoad(const LoadInput& in, LoadStep& step) {
    if (stepped_ && in.nowMs - lastStepMs_ < kLoadStepSpacingMs) return;
    const int slot = heaviestRunning(in);
    if (slot < 0) return;
    stepped_ = true;
    lastStepMs_ = in.nowMs;
    startHold(in, slot);
    ++steps_;
    step.slot = slot;
    step.card = in.layout.card[static_cast<size_t>(slot)];
    step.lateCount = in.tripLate;
  }

  Layout layout_ = defaultLayout();
  bool haveLayout_ = false;
  uint8_t held_ = 0;
  uint8_t returned_ = 0;  // bit s set once slot s's card has come back since its waits started
  std::array<uint32_t, kSlotCount> heldAtMs_{};
  std::array<uint32_t, kSlotCount> waitMs_{};
  std::array<uint32_t, kSlotCount> nextWaitMs_ = {kLoadWaitFirstMs, kLoadWaitFirstMs, kLoadWaitFirstMs, kLoadWaitFirstMs,
                                                  kLoadWaitFirstMs, kLoadWaitFirstMs, kLoadWaitFirstMs, kLoadWaitFirstMs};
  std::array<uint32_t, kSlotCount> returnedAtMs_{};
  uint32_t steps_ = 0;
  uint32_t lastStepMs_ = 0;
  bool stepped_ = false;
};

// The DIAG.TXT text of one step, for the audio entry's fault field. Returns the length written.
inline int loadStepText(const LoadStep& step, char* out, size_t size) {
  const int n = snprintf(out, size, "overload: %s out, %d of %d late", cardDesc(step.card).name, step.lateCount,
                         kLateWindowBlocks);
  return n < 0 ? 0 : (static_cast<size_t>(n) < size ? n : static_cast<int>(size) - 1);
}
