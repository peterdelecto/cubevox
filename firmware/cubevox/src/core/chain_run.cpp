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

// The S curve of a slot's fade, 3x^2 - 2x^3 of its progress x: the engine's gain against the dry input.
float fadeCurve(float progress) { return progress * progress * (3.0f - 2.0f * progress); }

// The engine gain of a slot at fade level `level`, 0 for the dry input to kSlotFadeBlocks for the engine.
float slotEngineGain(int level) { return fadeCurve(static_cast<float>(level) / static_cast<float>(kSlotFadeBlocks)); }

// True when any slot whose card runs engine `e` has its toggle on. Only the reverb engine serves two
// cards a layout can hold, so for every other engine this is the toggle of its one slot.
bool stageToggleOn(const ChainSnapshot& snap, Engine e) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (engineOf(snap.layout.card[static_cast<size_t>(s)]) == e && snap.slotOn[static_cast<size_t>(s)]) return true;
  }
  return false;
}

// True when slot `s` runs its engine this block or fades out of it: its toggle is on and it waits for no
// engine clear, or its fade has not reached the dry end.
bool slotLive(const ChainSnapshot& snap, const SlotFades& fades, int s) {
  return (snap.slotOn[static_cast<size_t>(s)] && !fades.waiting(s)) || fades.level(s) > 0;
}

// What the Autotune slot `s` owes the cards behind it. The tracker runs while a reader behind is live: the
// Formant engine directly behind the slot (every slot between holds Empty) while it can be heard, a Crowd
// card or a Growl card anywhere behind. The Formant engine reads the 48 kHz fine pass and the out-of-range
// flag, the Crowd card reads the flag, and Autotune's input goes into the Crowd card's ring while one is
// placed behind. The Formant engine directly behind takes Autotune's input every block the slot runs, on or
// off, which it cuts its grains from, and a live one hands Autotune its grain grid, which a corrected voice
// that starts takes up. At MIX 0 with its fade down the Formant card passes its input and reads no tracker;
// Apraxia pins its MIX at 1.
StageContext autotuneContext(const ChainEngines& engines, const ChainSnapshot& snap, const SlotFades& fades, int s) {
  StageContext ctx;
  bool onlyEmpty = true;
  for (int t = s + 1; t < kSlotCount; ++t) {
    const Card c = snap.layout.card[static_cast<size_t>(t)];
    const Engine e = engineOf(c);
    if (e == kNoEngine) continue;
    const bool live = slotLive(snap, fades, t);
    if (e == Engine::Formant && onlyEmpty) {
      ctx.feedsFormant = true;
      if (live) {
        const bool reads = c == Card::FormantFollower || engines.formant.needsTracker(snap.params.pitchCards.formant);
        ctx.tracker.run = reads;
        ctx.tracker.fine = reads;
        ctx.tracker.outOfRange = reads;
        ctx.groupGrid = engines.formant.gridMarks();
      }
    }
    if (e == Engine::Crowd) {
      ctx.feedsCrowd = true;
      if (live) {
        ctx.tracker.run = true;
        ctx.tracker.outOfRange = true;
      }
    }
    if (e == Engine::Growl && live) ctx.tracker.run = true;
    onlyEmpty = false;
  }
  return ctx;
}

// Runs the Crowd card at its slot. With an Autotune slot ahead, the card reads Autotune's tracker and its
// correction, and the Autotune slot has already written the ring. Otherwise the card writes its own input
// and runs a tracker of its own.
void runCrowd(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p, const PitchPath& path) {
  cv::CrowdPitch pitch;
  if (path.autotuneAhead) pitch = {path.upstream.result, engines.autotune.correctionSemis()};
  engines.crowd.process(in, out, n, p.crowd, pitch);
}

// Runs the Formant engine at its slot. Directly behind the Autotune slot it joins Autotune's grain pass,
// reading Autotune's tracker and ring; anywhere else it runs a tracker and a ring of its own. With Apraxia
// placed, the follower's envelope of `in` sets the SHIFT.
void runFormant(ChainEngines& engines, const float* in, float* out, int n, const ChainParams& p,
                const PitchPath& path) {
  const cv::FormantParams& params = engines.follower.follow(in, n, p.pitchCards.formant, p.follower);
  const bool reuse = path.autotuneAhead && path.adjacent;
  engines.formant.process(in, out, n, params, reuse ? path.upstream : cv::UpstreamPitch{});
}

// Hands the Octave card's share of the pitch on. Its voice moves the pitch only while it carries half the
// power, and delays the audio by its wet share. With no Autotune ahead nothing reads either.
void handOnOctave(const ChainEngines& engines, PitchPath& path) {
  if (!path.autotuneAhead) return;
  if (engines.octave.leads()) path.upstream.shiftSemis += engines.octave.intervalSemis();
  path.upstream.latency += engines.octave.outputLatency();
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
    case Engine::Autotune: engines.autotune.reset(); break;
    case Engine::Octave: engines.octave.reset(); break;
    case Engine::Formant:
      engines.formant.reset();
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
    case Engine::Count: break;
  }
}

bool runStage(ChainEngines& engines, Card c, const float* in, float* out, int n, const ChainParams& p,
              PitchPath& path, const StageContext& ctx) {
  const Engine e = engineOf(c);
  switch (e) {
    case Engine::InputGate: engines.inputGate.process(in, out, n, p.inputGate); break;
    case Engine::Autotune:
      if (ctx.feedsCrowd) engines.crowd.write(in, n);
      if (ctx.feedsFormant) engines.formant.feed(in, n);
      engines.autotune.process(in, out, n, p.pitchCards.autotune, ctx.tracker, ctx.groupGrid);
      break;
    case Engine::Octave: engines.octave.process(in, out, n, p.pitchCards.octave); break;
    case Engine::Formant: runFormant(engines, in, out, n, p, path); break;
    case Engine::Unison: engines.unison.process(in, out, n, p.unison); break;
    case Engine::Slapback: engines.slapback.process(in, out, n, p.slapback); break;
    case Engine::Distortion: engines.distortion.process(in, out, n, p.distortion); break;
    case Engine::Gate: engines.gate.process(in, out, n, p.gate); break;
    case Engine::Reverb: engines.reverb.process(in, out, n, p.reverb); break;
    case Engine::Crowd: runCrowd(engines, in, out, n, p, path); break;
    case Engine::Lofi: engines.lofi.process(in, out, n, p.lofi); break;
    case Engine::Bloom: runBloom(engines, in, out, n, p); break;
    case Engine::Growl:
      engines.growl.process(in, out, n, p.growl, path.autotuneAhead ? path.upstream : cv::UpstreamPitch{});
      break;
    case Engine::Count: return false;  // Empty runs no engine
  }

  // The card hands its share of the pitch on to the cards behind it.
  switch (e) {
    case Engine::Autotune:
      path.autotuneAhead = true;
      path.adjacent = true;
      path.upstream = {&engines.autotune.pitch(), engines.autotune.correctionSemis(), engines.autotune.outputLatency(),
                       &engines.autotune, ctx.slotGain0, ctx.slotGain1};
      break;
    case Engine::Octave:
      path.adjacent = false;
      handOnOctave(engines, path);
      break;
    case Engine::Formant:
      path.adjacent = false;
      if (path.autotuneAhead) path.upstream.latency += ctx.slotShare * engines.formant.outputLatency();
      break;
    default: path.adjacent = false; break;
  }
  return true;
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
    const float toEngine = fadeCurve(lin);
    wet[i] = wet[i] * toEngine + dry[i] * (1.0f - toEngine);
  }
}

namespace {

// A slot settled off with nothing live behind it. The pitch cards keep writing the slot's input into their
// ring, so a switch-on finds the history; Mob and Deepthroat clear theirs a slice a block. The walk hands no
// pitch on, except that a settled Autotune still stands in the path.
void restSlot(ChainEngines& engines, Engine e, const float* in, int n, PitchPath& path) {
  switch (e) {
    case Engine::Autotune:
      engines.autotune.idle(in, n);
      path.autotuneAhead = true;
      path.adjacent = true;
      path.upstream = {&engines.autotune.pitch(), 0.0f, 0.0f, nullptr, 0.0f, 0.0f};
      return;
    case Engine::Octave: engines.octave.idle(in, n); break;
    case Engine::Formant: engines.formant.idle(in, n); break;
    case Engine::Crowd: engines.crowd.rest(); break;
    case Engine::Growl: engines.growl.rest(); break;  // the card keeps a ring of its own input
    default: break;
  }
  path.adjacent = false;
}

// The first block of a slot that sat settled off. Autotune, Octave and the Formant engine keep the ring
// they wrote while off and settled their crossfade when they stopped.
void startSlot(ChainEngines& engines, Engine e) {
  switch (e) {
    case Engine::Autotune:
    case Engine::Octave: break;
    case Engine::Formant: engines.follower.reset(); break;
    case Engine::Crowd: engines.crowd.switchOn(); break;  // keeps a ring the Autotune slot fed this block
    default: resetEngine(engines, e); break;
  }
}

}  // namespace

bool runSlot(ChainEngines& engines, SlotFades& fades, int s, const ChainSnapshot& snap, const float* in, float* out,
             int n, PitchPath& path) {
  const Card c = snap.layout.card[static_cast<size_t>(s)];
  const Engine e = engineOf(c);
  if (e == kNoEngine) return false;

  // The Autotune engine and the Input gate's engine keep running with their slot settled off, the gate for
  // its detector and Autotune for the readers of its tracker, unless the slot waits for its engine to clear.
  StageContext ctx;
  if (e == Engine::Autotune) ctx = autotuneContext(engines, snap, fades, s);
  const bool waiting = fades.waiting(s);
  const bool detectorLive = e == Engine::InputGate && !waiting;
  const bool trackerLive = ctx.tracker.run && !waiting;
  const SlotFades::Step step = fades.step(s, stageToggleOn(snap, e));
  if (step.from == 0 && step.to == 0 && !detectorLive && !trackerLive) {
    restSlot(engines, e, in, n, path);
    return false;
  }
  if (step.from == 0 && step.to > 0 && !detectorLive && !trackerLive) startSlot(engines, e);
  ctx.slotShare = static_cast<float>(step.to) / static_cast<float>(kSlotFadeBlocks);
  ctx.slotGain0 = slotEngineGain(step.from);
  ctx.slotGain1 = slotEngineGain(step.to);
  const bool wrote = runStage(engines, c, in, out, n, snap.params, path, ctx);
  if (trackerLive && step.from == 0 && step.to == 0) {
    // The engine runs for its readers and the slot passes the dry input, which they now hear.
    path.upstream.shiftSemis = 0.0f;
    path.upstream.latency = 0.0f;
    return false;
  }
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
