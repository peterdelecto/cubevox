// The late-block policy, pure so the host test covers it. A block that runs past its cycle budget
// starts the next one late too, and the DMA can overrun. The late block raises a flag. The next block
// skips the chain and passes the dry input through the output stage, which lets the load fall back
// under the budget. The engines' state goes one block stale, which is inaudible against the overrun
// it prevents.

#pragma once

#include <stdint.h>

class LateBlocks {
 public:
  // Called where a block is counted late.
  void markLate() { pending_ = true; }

  // Called once at the start of every block. Takes the flag. Returns true when this block must pass
  // the dry input in place of the chain, and counts it. `chainSkipped` is true for a block that
  // skips the chain anyway (the layout fade's silent blocks); it clears the flag and is not counted.
  bool takeDry(bool chainSkipped) {
    const bool wasPending = pending_;
    pending_ = false;
    if (!wasPending || chainSkipped) return false;
    ++dryBlocks_;
    return true;
  }

  // Blocks that passed the dry input since the last clearCount().
  uint32_t dryBlocks() const { return dryBlocks_; }

  void clearCount() { dryBlocks_ = 0; }

 private:
  bool pending_ = false;
  uint32_t dryBlocks_ = 0;
};
