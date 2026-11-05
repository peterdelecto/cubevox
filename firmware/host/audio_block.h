// The box's audio block path on the host: the board's processHalf without the SAI rings and the
// cycle counter, running the same BlockFlow. The main thread publishes snapshots and the mute state,
// and the audio thread (or a headless loop) calls process() once per block.

#pragma once

#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>

#include "core/block_flow.h"
#include "core/chain_run.h"
#include "core/compose.h"
#include "core/level.h"
#include "engine/common.h"

namespace host {

// A sum of squares and a sample count that the audio side adds to and the main thread takes, packed
// in one word so a take never sees one without the other.
class EnergyTally {
 public:
  void add(float sumSquares, uint32_t samples);
  level::InputEnergy take();

 private:
  std::atomic<uint64_t> packed_{0};
};

class AudioBlock {
 public:
  AudioBlock();
  AudioBlock(const AudioBlock&) = delete;
  AudioBlock& operator=(const AudioBlock&) = delete;

  // Main thread. The board's audioPublish: two slots alternate by sequence number, and the audio
  // side reads the slot the last sequence names.
  void publish(const ChainSnapshot& snapshot);

  // Main thread. The mute sequence holds the output at zero until the relay has closed, and the
  // BYPASS role crossfades to the dry input.
  void setOutputHeld(bool held) { outputHeld_.store(held, std::memory_order_relaxed); }
  void setBypassOn(bool on) { bypassOn_.store(on, std::memory_order_relaxed); }

  // Audio thread. One block of n mono samples in and n out, where n is cv::kBlock. Any other n
  // writes silence, because the chain, the fades and the cost table all count whole blocks.
  void process(const float* in, float* out, int n);

  // Main thread. The energy of the blocks since the last take, for the ribbon.
  level::InputEnergy takeInputEnergy() { return input_.take(); }
  level::OutputEnergy takeOutputEnergy() { return output_.take(); }

  // Audio thread, or the headless loop that calls process(). A block counted late, as the board's
  // block interrupt counts one. The next block passes the dry input, and dryBlocks() counts it.
  void markLate() { flow_.markLate(); }
  uint32_t dryBlocks() const { return flow_.dryBlocks(); }

  // Main thread. The blocks discarded for a NaN, an infinity or a runaway sample since the start.
  uint32_t nanBlocks() const { return nanBlocks_.load(std::memory_order_relaxed); }

  // Main thread. The Input gate's highest detector level since the last take.
  float takeGatePeak() { return gatePeak_.exchange(0.0f, std::memory_order_acq_rel); }

 private:
  void syncSnapshot();
  void swapSnapshot();
  void noteGatePeak();

  std::unique_ptr<cv::AutotuneFx> autotune_;  // about 18 KB, so on the heap
  std::unique_ptr<cv::OctaveFx> octave_;      // about 16 KB
  std::unique_ptr<cv::Unison> unison_;
  std::unique_ptr<cv::Distortion> distortion_;
  ChainEngines engines_;

  std::array<ChainSnapshot, 2> snapshots_ = {};
  std::atomic<uint32_t> published_{0};
  std::atomic<bool> outputHeld_{true};
  std::atomic<bool> bypassOn_{false};

  ChainSnapshot held_ = {};
  uint32_t seenSeq_ = 0;
  bool layoutPending_ = false;
  BlockFlow flow_;
  std::atomic<uint32_t> nanBlocks_{0};
  std::atomic<float> gatePeak_{0.0f};

  EnergyTally input_;
  EnergyTally output_;
};

}  // namespace host
