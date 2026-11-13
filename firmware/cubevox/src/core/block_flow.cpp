#include "core/block_flow.h"

namespace {

constexpr uint32_t kAllEngines = (1u << kEngineCount) - 1u;

}  // namespace

void BlockFlow::adopt(const ChainSnapshot& from, const ChainSnapshot& to) {
  uint32_t mask = enginesToReset(from.layout, to.layout);
  for (int s = 0; s < kSlotCount; ++s) {
    const Engine e = engineOf(to.layout.card[static_cast<size_t>(s)]);
    if (e != kNoEngine && slots_.level(s) == 0 && to.slotOn[static_cast<size_t>(s)]) {
      mask |= 1u << static_cast<int>(e);
    }
  }
  queueReset(mask);
  slots_.settle(to.slotOn);
}

bool BlockFlow::adoptAtOnce(const ChainSnapshot& from, const ChainSnapshot& to) {
  uint8_t live = 0;
  for (int s = 0; s < kSlotCount; ++s) {
    const size_t i = static_cast<size_t>(s);
    if (from.slotOn[i] || to.slotOn[i] || slots_.level(s) > 0) live = static_cast<uint8_t>(live | (1u << s));
  }
  const uint32_t clearing = resetMask_ | enginesToReset(from.layout, to.layout);
  if (changeHeard(from.layout, to.layout, live, clearing)) return false;
  queueReset(clearing);
  return true;
}

void BlockFlow::clearWhileRunning(ChainEngines& engines, const Layout& layout) {
  if (resetMask_ == 0) {
    slots_.setWaiting(0);
    return;
  }
  resetWaiting(engines, kResetsPerChainBlock);
  uint8_t waiting = 0;
  for (int s = 0; s < kSlotCount; ++s) {
    if (enginesReadBy(layout, s) & resetMask_) waiting = static_cast<uint8_t>(waiting | (1u << s));
  }
  slots_.setWaiting(waiting);
}

void BlockFlow::discard(ChainEngines& engines, const ChainSnapshot& held) {
  resetMask_ = kAllEngines;
  fade_.cut();
  slots_.settle(held.slotOn);  // the output is cut and the engines clear, so no slot fades
  engines.polish.reset();
}

void BlockFlow::resetWaiting(ChainEngines& engines, int count) {
  for (int e = 0; e < kEngineCount && count > 0; ++e) {
    if (!(resetMask_ & (1u << e))) continue;
    resetEngine(engines, static_cast<Engine>(e));
    resetMask_ &= ~(1u << e);
    --count;
  }
}

void BlockFlow::blendJoin(float* a, bool dry) {
  if (dry || afterDry_) {
    const float from = lastOut_;
    for (int i = 0; i < kLateBlendSamples; ++i) {
      const float t = static_cast<float>(i + 1) / static_cast<float>(kLateBlendSamples);
      a[i] = from + (a[i] - from) * t;
    }
  }
  afterDry_ = dry;
  lastOut_ = a[cv::kBlock - 1];
}
