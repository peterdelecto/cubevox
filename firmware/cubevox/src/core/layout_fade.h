// Fade sequencer for a live layout change: the output ramps to zero, the audio block swaps in the new
// snapshot, and the output ramps back. Pure, called once per audio block (spec 2026-10-12 serial protocol).

#pragma once

#include <stdint.h>

#include "core/layout.h"
#include "core/registry.h"
#include "engine/common.h"

constexpr int kFadeMs = 50;
constexpr int kFadeBlocks = (kFadeMs * cv::kSampleRate / 1000 + cv::kBlock - 1) / cv::kBlock;

// Extra silent blocks after the swap block, one fewer than the engine count, so each engine clears
// in its own block. Clearing every engine's state in a single block would overrun that block's
// cycle budget.
constexpr int kFadeHoldBlocks = kEngineCount - 1;

class LayoutFade {
 public:
  // The gain ramps linearly from `from` at the block's first sample to `to` at its last.
  // `swap` is true on the first block after the output reached zero: the caller adopts the newest
  // snapshot and resets the engines before it renders that block.
  struct Block {
    float from;
    float to;
    bool swap;
  };

  // `differs` is true while the newest published layout is not the one the block runs. A layout that
  // arrives mid-fade is taken at the next zero point; one that returns to the running layout turns
  // the fade around.
  Block next(bool differs);

  // Drops the gain to zero at once, then runs the silent hold and the fade-in, for a block the
  // caller had to discard. The hold runs one block longer than a swap's, because no swap block comes
  // first, so the caller gets as many silent blocks for its engine clears.
  void cut();

  // `holdBlocks` silent blocks follow the swap block before the fade-in starts. They give the
  // caller room to spread engine clears over several blocks.
  explicit LayoutFade(int holdBlocks = kFadeHoldBlocks) : hold_(holdBlocks) {}

 private:
  enum class Phase : uint8_t { Idle, Out, Hold, In };

  Block ramp(bool swap);

  int hold_;
  int holdLeft_ = 0;
  Phase phase_ = Phase::Idle;
  int level_ = kFadeBlocks;  // 0..kFadeBlocks, the gain at the end of the last block in steps
};

// Bit per Engine for every engine that serves a card in a slot whose card differs between the layouts.
uint32_t enginesToReset(const Layout& from, const Layout& to);
