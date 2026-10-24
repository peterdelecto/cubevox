// The audio stall watch, pure. While the stream runs, the audio interrupt must finish a block at
// least every kAudioStallMs. board/fault.cpp feeds it the clock, the stream state and the block
// counter.

#pragma once

#include <stdint.h>

// Longest the block counter may sit still while the stream runs, about 75 blocks of 1.33 ms.
constexpr uint32_t kAudioStallMs = 100;

class AudioStallWatch {
 public:
  // One look per loop pass. `running` is false before the stream starts and once the audio module
  // has latched its own mute. `restarts` counts the stream's restarts. The watch arms on the first
  // look at each start, so neither the time setup() spends before the stream starts nor the gap a
  // restart leaves counts as a stall. Returns true on the one look that finds the counter still for
  // kAudioStallMs, and the watch stays stalled from then on.
  bool update(uint32_t nowMs, bool running, uint32_t restarts, uint32_t blocks) {
    if (stalled_) return false;
    if (!running) {
      armed_ = false;
      return false;
    }
    if (!armed_ || restarts != restarts_ || blocks != blocks_) {
      armed_ = true;
      restarts_ = restarts;
      blocks_ = blocks;
      sinceMs_ = nowMs;
      return false;
    }
    if (nowMs - sinceMs_ < kAudioStallMs) return false;
    stalled_ = true;
    return true;
  }

  bool stalled() const { return stalled_; }

  // The block counter as the watch last saw it move.
  uint32_t blocks() const { return blocks_; }

 private:
  bool armed_ = false;
  bool stalled_ = false;
  uint32_t restarts_ = 0;
  uint32_t blocks_ = 0;
  uint32_t sinceMs_ = 0;
};
