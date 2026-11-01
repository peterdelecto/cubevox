// The audio load for the DEBUG figure on the voice screen and for the user LED: the worst and the
// mean block of the last whole second as a percent of the block budget. Integer only, so the block
// interrupt can feed it.

#pragma once

#include <stdint.h>

// The figure never reads past this, however far a block overruns.
constexpr int kMaxLoadPercent = 999;

// Collects the worst and mean block time of each window of `span` blocks. The block interrupt calls
// add(). The main loop reads worst() and mean(), one aligned word each, so it needs no lock.
class LoadWindow {
 public:
  // Folds in one block's time in CPU cycles. After the span-th block of a window, the window's worst
  // and mean become the figures worst() and mean() return and the next window starts empty.
  void add(uint32_t cycles, uint32_t span) {
    if (cycles > running_) running_ = cycles;
    sum_ += cycles;
    if (++blocks_ < span) return;
    worst_ = running_;
    mean_ = static_cast<uint32_t>(sum_ / span);
    running_ = 0;
    sum_ = 0;
    blocks_ = 0;
  }

  // The worst block of the last finished window, in cycles. 0 until the first window finishes.
  uint32_t worst() const { return worst_; }

  // The mean block of the last finished window, in cycles. 0 until the first window finishes.
  uint32_t mean() const { return mean_; }

 private:
  uint32_t running_ = 0;
  uint64_t sum_ = 0;  // 64 bits because an overrun block can exceed the budget
  uint32_t blocks_ = 0;
  volatile uint32_t worst_ = 0;
  volatile uint32_t mean_ = 0;
};

// `cycles` as a rounded percent of `budget`, held to 0..kMaxLoadPercent. A zero budget reads 0.
inline int loadPercent(uint32_t cycles, uint32_t budget) {
  if (budget == 0) return 0;
  const uint64_t percent = (static_cast<uint64_t>(cycles) * 100 + budget / 2) / budget;
  return percent > static_cast<uint64_t>(kMaxLoadPercent) ? kMaxLoadPercent : static_cast<int>(percent);
}
