// The scope: a ring that keeps the newest chain output, and the pure step that turns a read of it
// into the 103 column runs the voice screen draws. The audio block writes one block into the ring
// for every block it runs, and the main loop reads it on each UI tick. The code is pure.
// Spec: docs/specs/2026-11-06-scope-design.md.

#pragma once

#include <stdint.h>

#include <array>
#include <atomic>

#include "core/level.h"
#include "engine/common.h"

// The picture. A column covers kScopeColumnSamples samples, and the frame is kScopeColumns columns.
constexpr int kScopeColumns = 103;
constexpr int kScopeColumnSamples = 8;
constexpr int kScopeFrameSamples = kScopeColumns * kScopeColumnSamples;

// Half the height of the trace, in rows about the horizon. A sample at the top of the scale lies
// this many rows above the horizon.
constexpr int kScopeHalfRows = 22;

// The scale falls this many dB a frame at most, so a quiet frame after a loud one draws at 70 % or
// more of the height. It rises at once.
constexpr float kScopeFallDb = -3.0f;

// A scale under this (-120 dBFS) is silence, and keeps a near-zero peak from blowing up the row
// scale.
constexpr float kScopeMinScale = 1e-6f;

// The trigger arms when the signal has gone under this share of the largest sample in the search
// span.
constexpr float kScopeHysteresis = 0.05f;

// The trigger looks for the start of a cycle in the samples from kScopeFrameSamples back to this
// many back from the newest: one period at 44 Hz, which holds a whole cycle of Octave -12 on G2.
constexpr int kScopeSearchBack = 1100 + kScopeFrameSamples;

// The ring holds kScopeBlocks blocks. A read takes the newest kScopeReadBlocks complete ones and so
// leaves the block in flight, and the frame plus the search span fit in them.
constexpr int kScopeBlocks = 32;
constexpr int kScopeReadBlocks = kScopeBlocks - 1;
constexpr int kScopeReadSamples = kScopeReadBlocks * cv::kBlock;
constexpr int kScopeRingSamples = kScopeBlocks * cv::kBlock;
static_assert(kScopeReadSamples >= kScopeSearchBack, "a read holds the whole search span");

// One column of the trace as rows about the horizon, `lo` the highest row and `hi` the lowest, so
// lo <= hi. A column of zeros is the one pixel on the horizon.
struct ScopeRun {
  int8_t lo = 0;
  int8_t hi = 0;
};

// `scale` is the sample magnitude that lands on the top and bottom rows. It is 0 for the horizon
// alone.
struct ScopeFrame {
  std::array<ScopeRun, kScopeColumns> column{};
  float scale = 0.0f;
};

// The newest chain output, one block at a time. One thread writes (the audio block) and one reads
// (the main loop). A block count orders them. The writer stores a block and then the count with
// release, the reader loads the count with acquire, copies, and loads it again. The slot the writer
// is filling is the oldest one, which a read leaves alone, until the writer has finished a block and
// started the next, which the second count shows. The samples are relaxed atomics, which cost the
// same as plain floats on the box and keep the host's two threads inside the memory model.
class ScopeRing {
 public:
  // Audio side. Stores one block of cv::kBlock samples and its energy. Returns the energy, the sum of
  // the squares of the block.
  float write(const float* block);

  // Audio side. Stores one block of zeros.
  void writeSilence();

  // Main loop. Copies the newest kScopeReadSamples samples, oldest first, into `out`, with zeros
  // ahead of a ring that is not yet full. Returns false when the writer kept pace with three
  // attempts in a row, and `out` is then not to be used.
  bool read(float* out) const;

  // Main loop. The energy of the blocks written since the previous call, the newest kScopeReadBlocks
  // at most. Returns an empty energy, and leaves the blocks for the next call, when three attempts
  // met the writer.
  level::OutputEnergy takeEnergy();

  // Blocks written so far.
  uint32_t blocks() const { return blocks_.load(std::memory_order_acquire); }

 private:
  void store(const float* block, float energy);

  std::array<std::atomic<float>, kScopeRingSamples> samples_{};
  std::array<std::atomic<float>, kScopeBlocks> energy_{};
  std::atomic<uint32_t> blocks_{0};
  uint32_t taken_ = 0;  // the block count at the last takeEnergy(), read by the main loop only
};

// The start of a cycle in `samples`, kScopeReadSamples long and oldest first, as an index. The
// search takes the span from kScopeFrameSamples back to kScopeSearchBack back from the newest sample
// and finds the largest sample in it. It steps back from there to the nearest rising zero crossing
// that follows a dip under -`hysteresis` times that largest sample, and may pass the span's old end
// to find it, so a peak at the old end still locks on its own cycle. A read with no such crossing
// returns the start of the newest kScopeFrameSamples samples, which is the frame free-running.
int scopeStart(const float* samples, float hysteresis);

// The frame for a read: the 103 column runs the voice screen draws. The scale is the larger of the
// frame's peak magnitude and `lastScale` less kScopeFallDb, so the frame's loudest sample lands on
// row 10 or 54. The new scale comes back in the frame; pass it as `lastScale` on the next call, and
// start at 0. A column runs from the lowest to the highest of its samples and is stretched to meet
// the last sample of the column before it, so the trace is one line. A `silent` tick, and a scale
// that is not positive, give the horizon alone and a scale of 0.
ScopeFrame scopeFrame(const float* samples, float lastScale, bool silent);
