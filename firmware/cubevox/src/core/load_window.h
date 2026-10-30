// The audio load for the DEBUG figure on the voice screen: the worst block of the last whole second
// as a percent of the block budget. Integer only, so the block interrupt can feed it.

#pragma once

#include <stdint.h>

// The figure never reads past this, however far a block overruns.
constexpr int kMaxLoadPercent = 999;

// Collects the worst block time of each window of `span` blocks. The block interrupt calls add().
// The main loop reads worst(), one aligned word, so it needs no lock.
class LoadWindow {
 public:
  // Folds in one block's time in CPU cycles. After the span-th block of a window, the window's worst
  // becomes the figure worst() returns and the next window starts empty.
  void add(uint32_t cycles, uint32_t span) {
    if (cycles > running_) running_ = cycles;
    if (++blocks_ < span) return;
    worst_ = running_;
    running_ = 0;
    blocks_ = 0;
  }

  // The worst block of the last finished window, in cycles. 0 until the first window finishes.
  uint32_t worst() const { return worst_; }

 private:
  uint32_t running_ = 0;
  uint32_t blocks_ = 0;
  volatile uint32_t worst_ = 0;
};

// `cycles` as a rounded percent of `budget`, held to 0..kMaxLoadPercent. A zero budget reads 0.
inline int loadPercent(uint32_t cycles, uint32_t budget) {
  if (budget == 0) return 0;
  const uint64_t percent = (static_cast<uint64_t>(cycles) * 100 + budget / 2) / budget;
  return percent > static_cast<uint64_t>(kMaxLoadPercent) ? kMaxLoadPercent : static_cast<int>(percent);
}
