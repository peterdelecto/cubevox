#include "core/chain_run.h"

#include "core/layout_fade.h"
#include "engine/common.h"
#include "engine/fastmath.h"

namespace {

constexpr uint32_t kBypassStep = kBypassFull / (kFadeMs * cv::kSampleRate / 1000);

// One step toward `goal`, never past it.
uint32_t stepPhase(uint32_t phase, uint32_t goal) {
  if (phase < goal) return goal - phase < kBypassStep ? goal : phase + kBypassStep;
  return phase - goal < kBypassStep ? goal : phase - kBypassStep;
}

}  // namespace

bool runStage(ChainEngines& engines, Card c, const float* in, float* out, int n, const ChainParams& p,
              bool& pitchDone) {
  switch (engineOf(c)) {
    case Engine::InputGate: engines.inputGate.process(in, out, n, p.inputGate); return true;
    case Engine::Pitch:
      if (pitchDone) return false;
      pitchDone = true;
      engines.pitch.process(in, out, n, p.pitchFx);
      return true;
    case Engine::Unison: engines.unison.process(in, out, n, p.unison); return true;
    case Engine::Slapback: engines.slapback.process(in, out, n, p.slapback); return true;
    case Engine::Distortion: engines.distortion.process(in, out, n, p.distortion); return true;
    case Engine::Gate: engines.gate.process(in, out, n, p.gate); return true;
    case Engine::Reverb: engines.reverb.process(in, out, n, p.reverb); return true;
    case Engine::Count: break;  // Empty runs no engine
  }
  return false;
}

void mixBypass(float* a, const float* dry, int n, bool bypassOn, uint32_t& phase) {
  const uint32_t goal = bypassOn ? kBypassFull : 0;
  if (phase == goal) {
    if (bypassOn) {
      for (int i = 0; i < n; ++i) a[i] = dry[i];
    }
    return;
  }
  uint32_t at = phase;
  for (int i = 0; i < n; ++i) {
    at = stepPhase(at, goal);
    const float toDry = cv::sinTurns(at);
    const float toChain = cv::sinTurns(kBypassFull - at);
    a[i] = a[i] * toChain + dry[i] * toDry;
  }
  phase = at;
}
