#include "core/scope.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp.h"

namespace {

// A read that meets the writer tries again this many times in all.
constexpr int kReadTries = 3;

constexpr uint32_t kSlotMask = kScopeBlocks - 1;
static_assert((kScopeBlocks & (kScopeBlocks - 1)) == 0, "the slot index is a mask");

// The newest block count the reader may take, from a count of `blocks` written and `taken` already
// taken. A count that wrapped still subtracts correctly because the slot mask divides 2^32.
uint32_t blocksToTake(uint32_t blocks, uint32_t taken) {
  return std::min<uint32_t>(blocks - taken, kScopeReadBlocks);
}

// The rows a sample sits above the horizon, in -kScopeHalfRows..kScopeHalfRows, for `rowsPerUnit`
// rows per unit of sample. The sum with the half-height is positive, so the cast truncates down.
int rowsAbove(float sample, float rowsPerUnit) {
  const float rows = cv::clampf(sample * rowsPerUnit, -static_cast<float>(kScopeHalfRows),
                                static_cast<float>(kScopeHalfRows));
  return static_cast<int>(rows + (static_cast<float>(kScopeHalfRows) + 0.5f)) - kScopeHalfRows;
}

// True when the negative run that ends just before `at` goes under -`depth`.
bool armedBefore(const float* samples, int at, float depth) {
  float lowest = 0.0f;
  for (int k = at - 1; k >= 0 && samples[k] < 0.0f; --k) lowest = std::min(lowest, samples[k]);
  return lowest < -depth;
}

// The largest sample magnitude in the kScopeFrameSamples samples from `start`.
float framePeak(const float* samples, int start) {
  float peak = 0.0f;
  for (int i = start; i < start + kScopeFrameSamples; ++i) peak = std::max(peak, std::fabs(samples[i]));
  return peak;
}

}  // namespace

void ScopeRing::store(const float* block, float energy) {
  const uint32_t count = blocks_.load(std::memory_order_relaxed);
  const size_t slot = count & kSlotMask;

  // The previous count store must reach a reader before these stores do, or a reader that sees a
  // new sample could still read the old count.
  std::atomic_thread_fence(std::memory_order_release);
  std::atomic<float>* const out = samples_.data() + slot * cv::kBlock;
  for (int i = 0; i < cv::kBlock; ++i) out[i].store(block[i], std::memory_order_relaxed);
  energy_[slot].store(energy, std::memory_order_relaxed);
  blocks_.store(count + 1, std::memory_order_release);
}

float ScopeRing::write(const float* block) {
  const float energy = level::sumSquares(block, cv::kBlock);
  store(block, energy);
  return energy;
}

void ScopeRing::writeSilence() {
  static const std::array<float, cv::kBlock> kZeros{};
  store(kZeros.data(), 0.0f);
}

bool ScopeRing::read(float* out) const {
  for (int attempt = 0; attempt < kReadTries; ++attempt) {
    const uint32_t count = blocks_.load(std::memory_order_acquire);
    const uint32_t have = std::min<uint32_t>(count, kScopeReadBlocks);
    const int missing = kScopeReadBlocks - static_cast<int>(have);
    std::fill(out, out + missing * cv::kBlock, 0.0f);
    for (uint32_t j = 0; j < have; ++j) {
      const std::atomic<float>* const in = samples_.data() + ((count - have + j) & kSlotMask) * cv::kBlock;
      float* const to = out + (static_cast<uint32_t>(missing) + j) * cv::kBlock;
      for (int i = 0; i < cv::kBlock; ++i) to[i] = in[i].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (blocks_.load(std::memory_order_relaxed) == count) return true;
  }
  return false;
}

level::OutputEnergy ScopeRing::takeEnergy() {
  for (int attempt = 0; attempt < kReadTries; ++attempt) {
    const uint32_t count = blocks_.load(std::memory_order_acquire);
    const uint32_t take = blocksToTake(count, taken_);
    float sum = 0.0f;
    for (uint32_t j = 0; j < take; ++j) sum += energy_[(count - take + j) & kSlotMask].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (blocks_.load(std::memory_order_relaxed) != count) continue;
    taken_ = count;
    level::OutputEnergy energy;
    energy.sumSquares = sum;
    energy.samples = take * cv::kBlock;
    return energy;
  }
  return level::OutputEnergy();
}

int scopeStart(const float* samples, float hysteresis) {
  const int newestFrame = kScopeReadSamples - kScopeFrameSamples;
  const int oldest = kScopeReadSamples - kScopeSearchBack;
  int peak = oldest;
  for (int i = oldest + 1; i <= newestFrame; ++i) {
    if (samples[i] > samples[peak]) peak = i;
  }
  const float depth = hysteresis * std::max(samples[peak], 0.0f);
  for (int i = peak; i >= 1; --i) {
    if (samples[i] >= 0.0f && samples[i - 1] < 0.0f && armedBefore(samples, i, depth)) return i;
  }
  return newestFrame;
}

ScopeFrame scopeFrame(const float* samples, float lastScale, bool silent) {
  ScopeFrame frame;
  if (silent) return frame;

  const int start = scopeStart(samples, kScopeHysteresis);
  const float scale = std::max(framePeak(samples, start), lastScale * cv::dbToGain(kScopeFallDb));
  if (!(scale >= kScopeMinScale)) return frame;

  frame.scale = scale;
  const float rowsPerUnit = static_cast<float>(kScopeHalfRows) / scale;
  int before = rowsAbove(samples[start], rowsPerUnit);  // the first column meets its own first sample
  for (int c = 0; c < kScopeColumns; ++c) {
    const float* const in = samples + start + c * kScopeColumnSamples;
    int highest = before;
    int lowest = before;
    for (int i = 0; i < kScopeColumnSamples; ++i) {
      const int rows = rowsAbove(in[i], rowsPerUnit);
      highest = std::max(highest, rows);
      lowest = std::min(lowest, rows);
      before = rows;
    }
    frame.column[static_cast<size_t>(c)].lo = static_cast<int8_t>(-highest);
    frame.column[static_cast<size_t>(c)].hi = static_cast<int8_t>(-lowest);
  }
  return frame;
}
