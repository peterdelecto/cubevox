#include "audio_block.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace host {

namespace {

// Engine clears per silent block. One per block keeps the largest clear inside a block's budget.
constexpr int kResetsPerSilentBlock = 1;
static_assert(kResetsPerSilentBlock * (1 + kFadeHoldBlocks) >= kEngineCount,
              "the silent blocks must be enough to clear every engine");

constexpr uint32_t kAllEngines = (1u << kEngineCount) - 1u;

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

AudioBlock::AudioBlock() : pitch_(std::make_unique<cv::PitchFx>()), engines_(*pitch_) {}

void AudioBlock::publish(const ChainSnapshot& snapshot) {
  const uint32_t next = published_.load(std::memory_order_relaxed) + 1;
  snapshots_[next & 1] = snapshot;
  published_.store(next, std::memory_order_release);
}

// Picks up a newly published snapshot. The same layout is adopted at once, and a toggle that moved
// crossfades from the slot's fade level; a different layout waits for the fade's zero point. The
// first publish has nothing to fade from and is adopted whole, every slot settled.
void AudioBlock::syncSnapshot() {
  const uint32_t seq = published_.load(std::memory_order_acquire);
  if (seq == seenSeq_) return;
  const ChainSnapshot& pub = snapshots_[seq & 1];
  const bool first = seenSeq_ == 0;
  layoutPending_ = !first && pub.layout != held_.layout;
  seenSeq_ = seq;
  if (!layoutPending_) held_ = pub;
  if (first) slotFades_.settle(held_.slotOn);
}

// At the zero point of the fade: adopts the newest snapshot, resets the engines whose slot changed
// card and settles every slot's toggle, since the output is at zero and nothing needs a crossfade.
void AudioBlock::swapSnapshot() {
  seenSeq_ = published_.load(std::memory_order_acquire);
  const ChainSnapshot& pub = snapshots_[seenSeq_ & 1];
  resetMask_ |= enginesToReset(held_.layout, pub.layout);
  held_ = pub;
  slotFades_.settle(held_.slotOn);
  layoutPending_ = false;
}

// Clears up to `count` of the waiting engines, lowest first.
void AudioBlock::resetWaiting(int count) {
  for (int e = 0; e < kEngineCount && count > 0; ++e) {
    if (!(resetMask_ & (1u << e))) continue;
    resetEngine(engines_, static_cast<Engine>(e));
    resetMask_ &= ~(1u << e);
    --count;
  }
}

// A NaN or an infinity out of the slots was silenced before the polish stages. The output cuts to
// zero and every engine clears in the silent blocks that follow, because the one that produced it
// is not known.
void AudioBlock::discardBlock() {
  resetMask_ = kAllEngines;
  fade_.cut();
  slotFades_.settle(held_.slotOn);
}

void AudioBlock::process(const float* in, float* out, int n) {
  if (n != cv::kBlock) {
    std::memset(out, 0, sizeof(float) * static_cast<size_t>(std::max(n, 0)));
    return;
  }
  syncSnapshot();
  fade_.hold(outputHeld_.load(std::memory_order_relaxed));
  const LayoutFade::Block fade = fade_.next(layoutPending_);
  if (fade.swap) swapSnapshot();

  std::array<float, cv::kBlock> a;
  std::array<float, cv::kBlock> b;
  std::copy(in, in + cv::kBlock, a.begin());
  input_.add(level::sumSquares(a.data(), cv::kBlock), cv::kBlock);
  const std::array<float, cv::kBlock> dry = a;  // the chain works over `a`; BYPASS needs the input
  if (fade.silent) {
    // The output is at zero here. The block skips the chain so the engine clears never land on top
    // of a full chain pass.
    resetWaiting(kResetsPerSilentBlock);
    a.fill(0);
  } else {
    ChainCycles cycles;
    if (!runChain(engines_, slotFades_, a.data(), b.data(), cv::kBlock, held_, NanoClock(), cycles)) discardBlock();
  }
  mixBypass(a.data(), dry.data(), cv::kBlock, bypassOn_.load(std::memory_order_relaxed), bypassPhase_);

  const GainRamp ramp = gain_.next(held_.outputGain, fade.from, fade.to, cv::kBlock);
  float gain = ramp.start;
  float energy = 0;
  for (int i = 0; i < cv::kBlock; ++i) {
    const float v = shapeOutput(a[static_cast<size_t>(i)], gain);
    gain += ramp.step;
    energy += v * v;
    out[i] = v;
  }
  output_.add(energy, cv::kBlock);
}

}  // namespace host
