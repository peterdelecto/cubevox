#include "core/block_flow.h"

namespace {

constexpr uint32_t kAllEngines = (1u << kEngineCount) - 1u;

}  // namespace

void BlockFlow::discard(ChainEngines& engines, const ChainSnapshot& held) {
  resetMask_ = kAllEngines;
  fade_.cut();
  slots_.settle(held.slotOn);  // the output is cut and the engines clear, so no slot fades
  engines.comp.reset();
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
