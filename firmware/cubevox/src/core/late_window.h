// The load rule's window, pure so the host test can step it. It keeps, for the last 48 blocks that ran
// the chain (64 ms), whether each was late and how many cycles each slot took. A dry block and a layout
// fade block never reach it. 12 late blocks inside the window raise a trip, which loop() takes once.
// Integer only, so the block interrupt can feed it.

#pragma once

#include <stdint.h>

#include <array>

#include "core/slots.h"

constexpr int kLateWindowBlocks = 48;
constexpr int kLateTripCount = 12;

using SlotCycles = std::array<uint32_t, kSlotCount>;

// What the window holds when it trips: how many of its blocks were late, and the per-slot cycles of
// the late block with the highest total.
struct LateTrip {
  int lateCount = 0;
  SlotCycles worstBlock{};
};

// The block interrupt calls add(). loop() calls take() and clear() with that interrupt masked, because
// the class holds no lock of its own.
class LateWindow {
 public:
  // Folds in one block that ran the chain. `late` is the block's verdict, `cycles` its total and `slot`
  // each slot's share. The oldest block leaves the window. The trip stays up until take() or clear().
  void add(bool late, uint32_t cycles, const SlotCycles& slot) {
    Entry& e = entry_[static_cast<size_t>(pos_)];
    if (e.late) --count_;
    e.late = late;
    e.total = cycles;
    e.slot = slot;
    if (late) ++count_;
    pos_ = (pos_ + 1) % kLateWindowBlocks;
    lateWords_ = count_;
    if (count_ >= kLateTripCount) tripped_ = true;
  }

  // One word read each, so loop() may test them without masking the interrupt.
  bool tripPending() const { return tripped_; }
  int lateCount() const { return lateWords_; }

  // True when a trip is up. Fills `out` and empties the window, so the next trip needs 12 new late
  // blocks.
  bool take(LateTrip& out) {
    if (!tripped_) return false;
    out.lateCount = count_;
    out.worstBlock = worstLate();
    clear();
    return true;
  }

  // Empties the window and drops a trip that was up.
  void clear() {
    for (Entry& e : entry_) e = Entry{};
    count_ = 0;
    lateWords_ = 0;
    pos_ = 0;
    tripped_ = false;
  }

 private:
  struct Entry {
    bool late = false;
    uint32_t total = 0;
    SlotCycles slot{};
  };

  SlotCycles worstLate() const {
    const Entry* worst = nullptr;
    for (const Entry& e : entry_) {
      if (e.late && (worst == nullptr || e.total > worst->total)) worst = &e;
    }
    return worst ? worst->slot : SlotCycles{};
  }

  std::array<Entry, kLateWindowBlocks> entry_{};
  int pos_ = 0;
  int count_ = 0;
  volatile int lateWords_ = 0;
  volatile bool tripped_ = false;
};
