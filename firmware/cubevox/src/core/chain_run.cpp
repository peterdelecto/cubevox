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

// True when any slot whose card runs engine `e` has its toggle on. Only the pitch engine serves two
// cards, so for every other engine this is the toggle of its one slot.
bool stageToggleOn(const ChainSnapshot& snap, Engine e) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (engineOf(snap.layout.card[static_cast<size_t>(s)]) == e && snap.slotOn[static_cast<size_t>(s)]) return true;
  }
  return false;
}

}  // namespace

void resetEngine(ChainEngines& engines, Engine e) {
  switch (e) {
    case Engine::InputGate: engines.inputGate.reset(); break;
    case Engine::Pitch: engines.pitch.reset(); break;
    case Engine::Unison: engines.unison.reset(); break;
    case Engine::Slapback: engines.slapback.reset(); break;
    case Engine::Distortion: engines.distortion.reset(); break;
    case Engine::Gate: engines.gate.reset(); break;
    case Engine::Reverb: engines.reverb.reset(); break;
    case Engine::Count: break;
  }
}

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

void mixSlotFade(float* wet, const float* dry, int n, int from, int to) {
  const int total = kSlotFadeBlocks * n;
  const uint32_t step = kBypassFull / static_cast<uint32_t>(total);
  const int dir = (to > from) - (to < from);
  int pos = from * n;  // samples of the fade toward the engine reached before this block
  for (int i = 0; i < n; ++i) {
    pos += dir;
    const uint32_t phase = pos <= 0 ? kBypassFull : static_cast<uint32_t>(total - pos) * step;
    wet[i] = wet[i] * cv::sinTurns(kBypassFull - phase) + dry[i] * cv::sinTurns(phase);
  }
}

bool runSlot(ChainEngines& engines, SlotFades& fades, int s, const ChainSnapshot& snap, const float* in, float* out,
             int n, bool& pitchDone) {
  const Card c = snap.layout.card[static_cast<size_t>(s)];
  const Engine e = engineOf(c);
  if (e == kNoEngine || (e == Engine::Pitch && pitchDone)) return false;

  const SlotFades::Step step = fades.step(s, stageToggleOn(snap, e));
  const bool detectorLive = e == Engine::InputGate;
  if (step.from == 0 && step.to == 0 && !detectorLive) {
    if (e == Engine::Pitch) pitchDone = true;  // the stage is stopped, so the second pitch card has nothing to run
    return false;
  }
  if (step.from == 0 && step.to > 0 && !detectorLive) resetEngine(engines, e);
  const bool wrote = runStage(engines, c, in, out, n, snap.params, pitchDone);
  if (wrote && step.from != step.to) mixSlotFade(out, in, n, step.from, step.to);
  return wrote;
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
