#include "audio_block.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace host {

namespace {

constexpr int kWordBits = 32;

// The clock runChain times each stage with, in nanoseconds. Nothing reads the figures; the host
// shows the cost table's percent, not a measurement.
struct NanoClock {
  uint32_t operator()() const {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
};

uint64_t packTally(float sumSquares, uint32_t samples) {
  uint32_t bits = 0;
  std::memcpy(&bits, &sumSquares, sizeof bits);
  return (static_cast<uint64_t>(bits) << kWordBits) | samples;
}

void unpackTally(uint64_t word, float& sumSquares, uint32_t& samples) {
  const uint32_t bits = static_cast<uint32_t>(word >> kWordBits);
  std::memcpy(&sumSquares, &bits, sizeof bits);
  samples = static_cast<uint32_t>(word);
}

}  // namespace

void EnergyTally::add(float sumSquares, uint32_t samples) {
  uint64_t seen = packed_.load(std::memory_order_relaxed);
  uint64_t next = 0;
  do {
    float total = 0;
    uint32_t count = 0;
    unpackTally(seen, total, count);
    next = packTally(total + sumSquares, count + samples);
  } while (!packed_.compare_exchange_weak(seen, next, std::memory_order_acq_rel, std::memory_order_relaxed));
}

level::InputEnergy EnergyTally::take() {
  level::InputEnergy energy;
  unpackTally(packed_.exchange(0, std::memory_order_acq_rel), energy.sumSquares, energy.samples);
  return energy;
}

AudioBlock::AudioBlock()
    : autotune_(std::make_unique<cv::AutotuneFx>()),
      octave_(std::make_unique<cv::OctaveFx>()),
      unison_(std::make_unique<cv::Unison>()),
      distortion_(std::make_unique<cv::Distortion>()),
      engines_(*autotune_, *octave_, *unison_, *distortion_) {}

void AudioBlock::publish(const ChainSnapshot& snapshot) {
  const uint32_t next = published_.load(std::memory_order_relaxed) + 1;
  snapshots_[next & 1] = snapshot;
  published_.store(next, std::memory_order_release);
}

// Picks up a newly published snapshot. The same layout is adopted at once, and a toggle that moved
// crossfades from the slot's fade level. A different layout that cannot change what is heard (cards
// swapped in slots that are off) is adopted at once too; any other waits for the fade's zero point.
// The first publish has nothing to fade from and is adopted whole, every slot settled.
void AudioBlock::syncSnapshot() {
  const uint32_t seq = published_.load(std::memory_order_acquire);
  if (seq == seenSeq_) return;
  const ChainSnapshot& pub = snapshots_[seq & 1];
  const bool first = seenSeq_ == 0;
  const bool differs = !first && pub.layout != held_.layout;
  layoutPending_ = differs && !flow_.adoptAtOnce(held_, pub);
  seenSeq_ = seq;
  if (!layoutPending_) held_ = pub;
  if (first) flow_.settleSlots(held_.slotOn);
}

// At the zero point of the fade: adopts the newest snapshot (BlockFlow::adopt).
void AudioBlock::swapSnapshot() {
  seenSeq_ = published_.load(std::memory_order_acquire);
  const ChainSnapshot& pub = snapshots_[seenSeq_ & 1];
  flow_.adopt(held_, pub);
  held_ = pub;
  layoutPending_ = false;
}

// Raises the gate peak to the Input gate's highest detector level of the block.
void AudioBlock::noteGatePeak() {
  const float peak = engines_.inputGate.takeDetectorPeak();
  float seen = gatePeak_.load(std::memory_order_relaxed);
  while (peak > seen && !gatePeak_.compare_exchange_weak(seen, peak, std::memory_order_acq_rel)) {
  }
}

void AudioBlock::process(const float* in, float* out, int n) {
  if (n != cv::kBlock) {
    std::memset(out, 0, sizeof(float) * static_cast<size_t>(std::max(n, 0)));
    return;
  }
  syncSnapshot();
  const BlockFlow::Start start = flow_.begin(outputHeld_.load(std::memory_order_relaxed), layoutPending_);
  if (start.fade.swap) swapSnapshot();

  std::array<float, cv::kBlock> a;
  std::array<float, cv::kBlock> b;
  std::copy(in, in + cv::kBlock, a.begin());
  input_.add(level::sumSquares(a.data(), cv::kBlock), cv::kBlock);
  const BlockFlow::Report report = flow_.run(engines_, held_, start, a.data(), b.data(),
                                             bypassOn_.load(std::memory_order_relaxed), NanoClock(),
                                             [out](int i, float v) { out[i] = v; });
  if (report.discarded) nanBlocks_.fetch_add(1, std::memory_order_relaxed);
  if (report.chainRan) noteGatePeak();
}

}  // namespace host
