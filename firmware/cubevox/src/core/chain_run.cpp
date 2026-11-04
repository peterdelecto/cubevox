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

// True when any slot whose card runs engine `e` has its toggle on. Only the reverb engine serves two
// cards a layout can hold, so for every other engine this is the toggle of its one slot.
bool stageToggleOn(const ChainSnapshot& snap, Engine e) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (engineOf(snap.layout.card[static_cast<size_t>(s)]) == e && snap.slotOn[static_cast<size_t>(s)]) return true;
  }
  return false;
}

// True when any pitch-group card (Autotune, Octave, Formant, Crowd, Growl) has its toggle on. The shared
// pitch stage runs while it is.
bool pitchGroupOn(const ChainSnapshot& snap) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (cardDesc(snap.layout.card[static_cast<size_t>(s)]).group == Group::Pitch && snap.slotOn[static_cast<size_t>(s)]) {
      return true;
    }
  }
  return false;
}

// Runs the Crowd card at its slot. A Crowd card ahead of every other pitch-group card also makes the shared
// pitch stage's call, so the ring it reads is written this block. The stage's output is then its
// lead. Both write into `out`, which the Crowd card reads sample by sample ahead of its own write.
void runCrowd(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p, bool& pitchDone) {
  if (!pitchDone) {
    pitchDone = true;
    engines.pitch.process(in, out, n, p.pitchFx);
    in = out;

    // The Chatter card cuts from the stage's output, not from the voices Crowd adds to it.
    if (engines.bloomLead) captureBloomSource(engines, out, n);
  }
  engines.crowd.process(in, out, n, p.crowd, engines.pitch);
}

// Runs the Growl card at its slot, making the shared pitch stage's call first when no pitch-group card
// ahead of it has, as runCrowd does.
void runGrowl(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p, bool& pitchDone) {
  if (!pitchDone) {
    pitchDone = true;
    engines.pitch.process(in, out, n, p.pitchFx);
    in = out;
  }
  engines.growl.process(in, out, n, p.growl, engines.pitch);
}

// Runs the Chatter card. With an Input gate placed (`p.bloom.keyFromGate`), the gate's detector state is
// the word key for the block; the gate's engine keeps its detector live while its own slot is off.
// Otherwise the card listens to its own input and ignores the state. Its slices come from the chain's
// tap when runChain made one.
void runBloom(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p) {
  const float* const src = engines.bloomSrcLive ? engines.bloomSrc.data() : in;
  engines.bloom.process(in, src, out, n, p.bloom, engines.inputGate.open());
}

bool holdsPitchStage(Card c) { return c == Card::Autotune || c == Card::Octave || c == Card::Formant; }

}  // namespace

void captureBloomSource(ChainEngines& engines, const float* block, int n) {
  for (int i = 0; i < n; ++i) engines.bloomSrc[static_cast<size_t>(i)] = block[i];
  engines.bloomSrcLive = true;
}

BloomTap bloomTap(const Layout& layout) {
  int bloom = -1;
  int tap = -1;
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = layout.card[static_cast<size_t>(s)];
    if (c == Card::Bloom) bloom = s;
    if (holdsPitchStage(c)) tap = s;
  }
  const bool pitchCard = tap >= 0;
  if (tap < 0) tap = 0;
  return {bloom > tap ? tap : -1, pitchCard};
}

void resetEngine(ChainEngines& engines, Engine e) {
  switch (e) {
    case Engine::InputGate: engines.inputGate.reset(); break;
    case Engine::Pitch: engines.pitch.reset(); break;
    case Engine::Unison: engines.unison.reset(); break;
    case Engine::Slapback: engines.slapback.reset(); break;
    case Engine::Distortion: engines.distortion.reset(); break;
    case Engine::Gate: engines.gate.reset(); break;
    case Engine::Reverb: engines.reverb.reset(); break;
    case Engine::Crowd: engines.crowd.reset(); break;
    case Engine::Lofi: engines.lofi.reset(); break;
    case Engine::Bloom: engines.bloom.reset(); break;
    case Engine::Growl: engines.growl.reset(); break;
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
    case Engine::Crowd: runCrowd(engines, in, out, n, p, pitchDone); return true;
    case Engine::Lofi: engines.lofi.process(in, out, n, p.lofi); return true;
    case Engine::Bloom: runBloom(engines, in, out, n, p); return true;
    case Engine::Growl: runGrowl(engines, in, out, n, p, pitchDone); return true;
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
  if (e == kNoEngine) return false;

  // The first pitch-group card's slot carries the shared pitch stage and fades on every pitch-group
  // toggle. Autotune, Octave and Formant have no engine of their own, so after the stage ran they do nothing.
  const bool carriesPitch = !pitchDone && cardDesc(c).group == Group::Pitch;
  if (e == Engine::Pitch && !carriesPitch) return false;

  const SlotFades::Step step = fades.step(s, carriesPitch ? pitchGroupOn(snap) : stageToggleOn(snap, e));
  const bool detectorLive = e == Engine::InputGate;
  if (step.from == 0 && step.to == 0 && !detectorLive) {
    if (carriesPitch) pitchDone = true;  // the stage is stopped, so no later pitch-group card has a ring to read
    return false;
  }
  if (step.from == 0 && step.to > 0 && !detectorLive) {
    if (carriesPitch) resetEngine(engines, Engine::Pitch);
    if (e != Engine::Pitch) resetEngine(engines, e);
  }
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
