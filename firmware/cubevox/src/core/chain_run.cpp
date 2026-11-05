#include "core/chain_run.h"

#include <algorithm>

#include "core/layout_fade.h"
#include "engine/common.h"
#include "engine/fastmath.h"

namespace {

// The BYPASS phase moves this far a sample.
constexpr uint32_t kBypassStep = kBypassFull / (kFadeMs * cv::kSampleRate / 1000);

// A gate's own on/off fade ends inside the slot fade, so a switched-off gate is a plain copy by the
// time the slot passes the engine's output alone.
static_assert(cv::kGateFadeMs * cv::kSmpPerMs <= static_cast<float>(kSlotFadeBlocks * cv::kBlock),
              "the gate's off fade outlasts the slot fade");

// The two gains of the BYPASS mix at a phase.
struct BypassGains {
  float chain;
  float dry;
};

BypassGains gainsAt(uint32_t phase) { return {cv::sinTurns(kBypassFull - phase), cv::sinTurns(phase)}; }

// Mixes the dry input into up to kBlock chain samples while the phase moves one step a sample toward
// `goal`, and leaves the phase where the chunk ends. The samples that move a whole step follow the line
// from the gains at the chunk's start to the gains at the last of them, which stays within 2.2e-4 of
// the sine. The sample that lands on the goal with a part step and every sample after it take the
// gains of the phase they sit at, so a chunk's last sample is exact and the next chunk starts from it.
void fadeChunk(float* a, const float* dry, int n, uint32_t goal, uint32_t& phase) {
  const bool up = phase < goal;
  const uint32_t away = up ? goal - phase : phase - goal;
  const uint32_t toGo = (away + kBypassStep - 1) / kBypassStep;
  const int steps = toGo < static_cast<uint32_t>(n) ? static_cast<int>(toGo) : n;
  const bool lands = static_cast<uint32_t>(steps) == toGo;
  const int whole = lands && away % kBypassStep != 0 ? steps - 1 : steps;
  const uint32_t wholeMoved = static_cast<uint32_t>(whole) * kBypassStep;
  const uint32_t wholeAt = up ? phase + wholeMoved : phase - wholeMoved;
  const uint32_t at = lands ? goal : wholeAt;
  const BypassGains g0 = gainsAt(phase);
  const BypassGains gWhole = gainsAt(wholeAt);
  const BypassGains gEnd = gainsAt(at);
  if (whole > 0) {
    const float chainSlope = (gWhole.chain - g0.chain) / static_cast<float>(whole);
    const float drySlope = (gWhole.dry - g0.dry) / static_cast<float>(whole);
    for (int i = 0; i < whole - 1; ++i) {
      const float k = static_cast<float>(i + 1);
      a[i] = a[i] * (g0.chain + chainSlope * k) + dry[i] * (g0.dry + drySlope * k);
    }
    a[whole - 1] = a[whole - 1] * gWhole.chain + dry[whole - 1] * gWhole.dry;
  }
  for (int i = whole; i < n; ++i) a[i] = a[i] * gEnd.chain + dry[i] * gEnd.dry;
  phase = at;
}

// True when any slot whose card runs engine `e` has its toggle on. Only the reverb engine serves two
// cards a layout can hold, so for every other engine this is the toggle of its one slot.
bool stageToggleOn(const ChainSnapshot& snap, Engine e) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (engineOf(snap.layout.card[static_cast<size_t>(s)]) == e && snap.slotOn[static_cast<size_t>(s)]) return true;
  }
  return false;
}

// True when any pitch-group card (Autotune, Octave, Formant, Crowd, Growl, Ring mod) has its toggle on. The shared
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
    engines.pitch.process(in, out, n, engines.follower.follow(in, n, p.pitchFx, p.follower));
    in = out;
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

// Runs the Ring mod card at its slot, making the shared pitch stage's call first when no pitch-group card
// ahead of it has, as runGrowl does. The card reads the tracker's period, not the ring. It snaps to the notes
// of Autotune's KEY when Autotune is placed (`p.ringMod.keyed`), and to every semitone otherwise.
void runRingMod(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p, bool& pitchDone) {
  if (!pitchDone) {
    pitchDone = true;
    engines.pitch.process(in, out, n, p.pitchFx);
    in = out;
  }
  engines.ringMod.process(in, out, n, p.ringMod, engines.pitch, p.ringMod.keyed ? p.pitchFx.autotune.key : -1);
}

// Runs the Sympath card (engine Strings). It tracks the held note itself and reads no Autotune key.
void runStrings(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p) {
  engines.strings.process(in, out, n, p.strings);
}

// Runs the Chatter card. With an Input gate placed (`p.bloom.keyFromGate`), the gate's detector state is
// the word key for the block; the gate's engine keeps its detector live while its own slot is off.
// Otherwise the card listens to its own input and ignores the state. Its slices and its dry both come
// from `in`, the block leaving the slot ahead of it.
void runBloom(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p) {
  engines.bloom.process(in, out, n, p.bloom, engines.inputGate.open());
}

}  // namespace

void resetEngine(ChainEngines& engines, Engine e) {
  switch (e) {
    case Engine::InputGate: engines.inputGate.reset(); break;
    case Engine::Pitch:
      engines.pitch.reset();
      engines.follower.reset();
      break;
    case Engine::Unison: engines.unison.reset(); break;
    case Engine::Slapback: engines.slapback.reset(); break;
    case Engine::Distortion: engines.distortion.reset(); break;
    case Engine::Gate: engines.gate.reset(); break;
    case Engine::Reverb: engines.reverb.reset(); break;
    case Engine::Crowd: engines.crowd.reset(); break;
    case Engine::Lofi: engines.lofi.reset(); break;
    case Engine::Bloom: engines.bloom.reset(); break;
    case Engine::Growl: engines.growl.reset(); break;
    case Engine::RingMod: engines.ringMod.reset(); break;
    case Engine::Strings: engines.strings.reset(); break;
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
      engines.pitch.process(in, out, n, engines.follower.follow(in, n, p.pitchFx, p.follower));
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
    case Engine::RingMod: runRingMod(engines, in, out, n, p, pitchDone); return true;
    case Engine::Strings: runStrings(engines, in, out, n, p); return true;
    case Engine::Count: break;  // Empty runs no engine
  }
  return false;
}

// The engine gain follows an S curve, 3x^2 - 2x^3 of the fade's progress, and the two gains sum to 1.
// A card whose output carries its input then holds its level across the fade, where an equal-power
// pair would swell it by 3 dB.
void mixSlotFade(float* wet, const float* dry, int n, int from, int to) {
  const float total = static_cast<float>(kSlotFadeBlocks * n);
  const int dir = (to > from) - (to < from);
  int pos = from * n;  // samples of the fade toward the engine reached before this block
  for (int i = 0; i < n; ++i) {
    pos += dir;
    const float lin = pos <= 0 ? 0.0f : static_cast<float>(pos) / total;
    const float toEngine = lin * lin * (3.0f - 2.0f * lin);
    wet[i] = wet[i] * toEngine + dry[i] * (1.0f - toEngine);
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
    if (carriesPitch) {
      pitchDone = true;  // the stage is stopped, so no later pitch-group card has a ring to read
      engines.pitch.rest();
    }
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
  for (int i = 0; i < n; i += cv::kBlock) fadeChunk(a + i, dry + i, std::min(n - i, cv::kBlock), goal, phase);
}
