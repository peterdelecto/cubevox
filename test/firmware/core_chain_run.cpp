// Checks the audio block's chain walk and BYPASS mix on the host. Covers the card to engine
// routing, the shared PitchFx call, the ping-pong buffers, the NaN guard, the stage timing, the
// shipped layout over several blocks and the BYPASS crossfade.

#include "support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

#include "core/chain_run.h"
#include "core/compose.h"
#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/panel.h"
#include "core/registry.h"
#include "core/settings.h"

namespace {

constexpr int kN = cv::kBlock;
using Block = std::array<float, kN>;

// Holds the engines of one chain, built the way the board builds them. It is too big for the stack.
struct Rig {
  std::unique_ptr<cv::PitchFx> pitch = std::make_unique<cv::PitchFx>();
  ChainEngines engines{*pitch};
};

std::unique_ptr<Rig> newRig() { return std::make_unique<Rig>(); }

// Stands in for the cycle counter and never advances, for runs that ignore the timing.
struct StillClock {
  uint32_t operator()() { return 0; }
};

// Stands in for the cycle counter. Its k-th read returns k * k, so each pair of reads differs by a
// number of its own.
struct SquareClock {
  uint32_t reads = 0;
  uint32_t operator()() {
    const uint32_t t = reads * reads;
    ++reads;
    return t;
  }
};

// Makes a steady tone that continues across blocks.
Block tone(int block, float amp) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr double kHz = 220.0;
  Block x{};
  for (int i = 0; i < kN; ++i) {
    const double t = static_cast<double>(block * kN + i) / cv::kSampleRate;
    x[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * kHz * t));
  }
  return x;
}

double rms(const Block& x) {
  double sum = 0.0;
  for (float v : x) sum += static_cast<double>(v) * static_cast<double>(v);
  return std::sqrt(sum / kN);
}

float maxAbsDiff(const Block& a, const Block& b) {
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
  return worst;
}

bool allFinite(const Block& x) {
  return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

bool allZero(const Block& x) {
  return std::all_of(x.begin(), x.end(), [](float v) { return v == 0.0f; });
}

// Places the cards in slot order and leaves the rest Empty.
Layout layoutOf(std::initializer_list<Card> cards) {
  Layout layout;
  layout.card.fill(Card::Empty);
  size_t slot = 0;
  for (Card c : cards) layout.card[slot++] = c;
  return layout;
}

// Turns every toggle on, puts every knob at `knob` and sets both polish stages to `polish`.
ChainSnapshot snapshotFor(const Layout& layout, float knob, bool polish) {
  PanelReading panel;
  panel.toggle.fill(true);
  for (auto& slot : panel.knob) slot.fill(knob);
  Settings settings;
  settings.polishComp = polish;
  settings.polishEq = polish;
  return composeSnapshot(layout, panel, settings, BenchOverrides());
}

struct Run {
  Block out;
  bool finite;
  ChainCycles cycles;
};

// Runs one block through runChain on `rig`.
template <typename Clock>
Run runBlock(Rig& rig, const ChainSnapshot& snap, const Block& in, Clock clock) {
  Run run{in, true, {}};
  Block scratch{};
  run.finite = runChain(rig.engines, run.out.data(), scratch.data(), kN, snap, clock, run.cycles);
  return run;
}

// Runs the chain by hand on `rig`'s own engines, with a fresh buffer per stage and a plain copy in
// between. The slots go first, then POLISH COMP and POLISH EQ.
Block byHand(Rig& rig, const ChainSnapshot& snap, const Block& in) {
  Block cur = in;
  Block next{};
  bool pitchDone = false;
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = snap.layout.card[static_cast<size_t>(s)];
    if (runStage(rig.engines, c, cur.data(), next.data(), kN, snap.params, pitchDone)) cur = next;
  }
  rig.engines.comp.process(cur.data(), next.data(), kN, snap.params.comp);
  rig.engines.polish.process(next.data(), cur.data(), kN, snap.params.eq);
  return cur;
}

void checkRouting() {
  ChainParams p;  // the input gate is on and the panel gate is off
  p.inputGate.thresholdDb = 0.0f;
  p.inputGate.tuning.holdMs = 0.0f;
  p.gate.thresholdDb = 0.0f;
  p.gate.tuning.holdMs = 0.0f;

  // A gate that is on has closed after 66 ms of a -10 dBFS tone against a threshold of 0 dB.
  for (const bool inputGateCard : {true, false}) {
    ChainParams params = p;
    params.inputGate.on = inputGateCard;
    params.gate.on = !inputGateCard;
    for (const Card c : {Card::InputGate, Card::Gate}) {
      auto rig = newRig();
      const bool shouldClose = (c == Card::InputGate) == inputGateCard;
      Block in{};
      Block out{};
      bool pitchDone = false;
      bool copied = true;
      for (int b = 0; b < 50; ++b) {
        in = tone(b, 0.3f);
        const bool wrote = runStage(rig->engines, c, in.data(), out.data(), kN, params, pitchDone);
        check(wrote, "stage: a gate card always writes");
        copied = copied && out == in;
      }
      if (shouldClose) {
        check(rms(out) < 0.5 * rms(in), "stage: a card reads its own gate's settings and closes");
      } else {
        check(copied, "stage: a card whose gate is off passes the block untouched");
      }
    }
  }
}

void checkEmptyAndPitchSharing() {
  auto rig = newRig();
  const ChainParams p;
  const Block in = tone(0, 0.3f);
  Block out;
  out.fill(7.0f);
  bool done = false;
  check(!runStage(rig->engines, Card::Empty, in.data(), out.data(), kN, p, done) && out[0] == 7.0f && !done,
        "stage: an Empty slot writes nothing");
  check(runStage(rig->engines, Card::Autotune, in.data(), out.data(), kN, p, done) && done,
        "stage: the first pitch card runs PitchFx");
  out.fill(7.0f);
  check(!runStage(rig->engines, Card::Octave, in.data(), out.data(), kN, p, done) && out[0] == 7.0f && out[kN - 1] == 7.0f,
        "stage: a second pitch card in the same pass writes nothing");
  done = false;
  check(runStage(rig->engines, Card::Octave, in.data(), out.data(), kN, p, done) && done,
        "stage: Octave alone runs PitchFx too");
}

// The walk equals the chain run by hand, whichever side the last writing stage left the block on.
// The polish stages are on, so a finished block that stayed in the scratch buffer would be the
// compressor's output and differ from the EQ's by much more than the tolerance.
void checkWalkAgainstHand() {
  constexpr float kTolerance = 1e-5f;
  const std::initializer_list<std::initializer_list<Card>> layouts = {
      {},
      {Card::Distortion},
      {Card::Slapback, Card::Distortion},
      {Card::Unison, Card::Empty, Card::Slapback, Card::Distortion},
  };
  for (const auto& cards : layouts) {
    const ChainSnapshot snap = snapshotFor(layoutOf(cards), 0.5f, true);
    auto chain = newRig();
    auto hand = newRig();
    float worst = 0.0f;
    bool finite = true;
    for (int b = 0; b < 4; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      worst = std::max(worst, maxAbsDiff(run.out, byHand(*hand, snap, in)));
      finite = finite && run.finite;
    }
    check(worst < kTolerance, "chain: the finished block lands in the caller's buffer for any number of writing stages");
    check(finite, "chain: a clean pass reports finite");
  }
}

// With nothing in the slots and both polish stages off, the block passes bit for bit.
void checkEmptyChain() {
  const ChainSnapshot snap = snapshotFor(layoutOf({}), 0.5f, false);
  auto rig = newRig();
  bool same = true;
  for (int b = 0; b < 4; ++b) {
    const Block in = tone(b, 0.3f);
    same = same && runBlock(*rig, snap, in, StillClock()).out == in;
  }
  check(same, "chain: an empty layout with the polish stages off passes the block untouched");
}

// Autotune and Octave in one layout make one PitchFx call per block.
void checkPitchCalledOnce() {
  const ChainSnapshot snap = snapshotFor(layoutOf({Card::Autotune, Card::Octave}), 0.5f, false);
  auto chain = newRig();
  auto direct = newRig();
  bool same = true;
  for (int b = 0; b < 6; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*chain, snap, in, StillClock());
    Block want{};
    direct->engines.pitch.process(in.data(), want.data(), kN, snap.params.pitchFx);
    same = same && run.out == want;
  }
  check(same, "chain: two pitch cards make one PitchFx call per block");
}

void checkNanGuard() {
  const ChainSnapshot snap = snapshotFor(layoutOf({}), 0.5f, true);
  auto rig = newRig();
  Block poisoned = tone(0, 0.3f);
  poisoned[17] = NAN;
  Run run = runBlock(*rig, snap, poisoned, StillClock());
  check(!run.finite && allZero(run.out), "chain: a NaN out of the slots silences the block and reports it");

  poisoned = tone(1, 0.3f);
  poisoned[3] = INFINITY;
  run = runBlock(*rig, snap, poisoned, StillClock());
  check(!run.finite && allZero(run.out), "chain: an infinity is caught the same way");

  bool sound = false;
  bool clean = true;
  for (int b = 2; b < 12; ++b) {
    run = runBlock(*rig, snap, tone(b, 0.3f), StillClock());
    clean = clean && run.finite && allFinite(run.out);
    sound = sound || !allZero(run.out);
  }
  check(clean && sound, "chain: the polish stages play on after a discarded block");
}

// The clock reads twice around each slot, then once before POLISH COMP, once between the polish
// stages and once after POLISH EQ. Each bucket gets its own pair of reads.
void checkStageTiming() {
  const ChainSnapshot snap = snapshotFor(defaultLayout(), 0.5f, true);
  auto rig = newRig();
  const Run run = runBlock(*rig, snap, tone(0, 0.3f), SquareClock());
  bool slots = true;
  for (uint32_t s = 0; s < static_cast<uint32_t>(kSlotCount); ++s) {
    slots = slots && run.cycles.slot[s] == (2 * s + 1) * (2 * s + 1) - (2 * s) * (2 * s);
  }
  check(slots, "chain: each slot is timed by its own pair of reads");
  const uint32_t first = 2 * static_cast<uint32_t>(kSlotCount);  // read index of the first polish read
  check(run.cycles.comp == (first + 1) * (first + 1) - first * first, "chain: POLISH COMP is timed alone");
  check(run.cycles.eq == (first + 2) * (first + 2) - (first + 1) * (first + 1), "chain: POLISH EQ is timed alone");
}

// Runs the shipped layout with every toggle on and the knobs at mid travel.
void checkDefaultChain() {
  const ChainSnapshot snap = snapshotFor(defaultLayout(), 0.5f, true);
  auto first = newRig();
  auto second = newRig();
  bool finite = true;
  bool same = true;
  double lastRms = 0.0;
  for (int b = 0; b < 24; ++b) {
    const Block in = tone(b, 0.3f);
    const Run one = runBlock(*first, snap, in, StillClock());
    const Run two = runBlock(*second, snap, in, StillClock());
    finite = finite && one.finite && allFinite(one.out);
    same = same && one.out == two.out;
    lastRms = rms(one.out);
  }
  check(finite, "chain: the shipped layout stays finite");
  check(lastRms > 0.01, "chain: the shipped layout plays");
  check(same, "chain: two chains on the same input give the same output, so the walk keeps no state of its own");
}

// Holds the gains mixBypass applies over `n` samples from `phase`. The chain gain comes from a block
// of ones mixed with zeros, and the dry gain from zeros mixed with ones.
struct Gains {
  std::vector<float> chain;
  std::vector<float> dry;
};

Gains gainsOver(uint32_t phase, bool bypassOn, int n) {
  const std::vector<float> zeros(static_cast<size_t>(n), 0.0f);
  const std::vector<float> ones(static_cast<size_t>(n), 1.0f);
  Gains g{ones, zeros};
  uint32_t at = phase;
  mixBypass(g.chain.data(), zeros.data(), n, bypassOn, at);
  at = phase;
  mixBypass(g.dry.data(), ones.data(), n, bypassOn, at);
  return g;
}

void checkBypassEnds() {
  const Block wet = tone(0, 0.3f);
  Block dry = tone(1, 0.2f);
  Block a = wet;
  uint32_t phase = 0;
  mixBypass(a.data(), dry.data(), kN, false, phase);
  check(a == wet && phase == 0, "bypass: off and settled leaves the chain output untouched");

  phase = kBypassFull;
  mixBypass(a.data(), dry.data(), kN, true, phase);
  check(a == dry && phase == kBypassFull, "bypass: on and settled plays exactly the dry input");

  // The shipped chain's own block meets both ends too.
  auto rig = newRig();
  const ChainSnapshot snap = snapshotFor(defaultLayout(), 0.5f, true);
  Run run = runBlock(*rig, snap, dry, StillClock());
  const Block chainOut = run.out;
  phase = 0;
  mixBypass(run.out.data(), dry.data(), kN, false, phase);
  check(run.out == chainOut, "bypass: the shipped chain's block passes untouched with BYPASS off");
  phase = kBypassFull;
  mixBypass(run.out.data(), dry.data(), kN, true, phase);
  check(run.out == dry, "bypass: with BYPASS on the shipped chain's block is replaced by the dry input");
}

// Runs one crossfade, sample by sample, from the chain to the dry input.
void checkBypassFade() {
  const int fadeSamples = kFadeMs * cv::kSampleRate / 1000;
  int steps = 0;
  uint32_t phase = 0;
  float one = 1.0f;
  const float zero = 0.0f;
  while (phase != kBypassFull && steps < 2 * fadeSamples) {
    mixBypass(&one, &zero, 1, true, phase);
    ++steps;
  }
  check(phase == kBypassFull && steps >= fadeSamples && steps <= fadeSamples + 2,
        "bypass: the crossfade lasts kFadeMs and lands on the dry end");

  // sinTurns holds 1e-6 absolute, which is all the flat ends of each gain can be trusted to.
  constexpr float kSinTolerance = 1e-6f;
  const Gains g = gainsOver(0, true, steps);
  bool equalPower = true;
  bool monotone = true;
  for (size_t i = 0; i < g.chain.size(); ++i) {
    equalPower = equalPower && std::fabs(g.chain[i] * g.chain[i] + g.dry[i] * g.dry[i] - 1.0f) < 2e-5f;
    if (i == 0) continue;
    monotone = monotone && g.chain[i] <= g.chain[i - 1] + kSinTolerance && g.dry[i] >= g.dry[i - 1] - kSinTolerance;
  }
  check(equalPower, "bypass: the two gains keep equal power all the way through");
  check(monotone, "bypass: the chain gain only falls and the dry gain only rises");
  check(g.chain.front() > 0.999f && g.dry.front() < 0.001f && g.chain.back() == 0.0f && std::fabs(g.dry.back() - 1.0f) < 1e-6f,
        "bypass: the fade starts at the chain and ends at the dry input");
}

void checkBypassMidAndReversal() {
  const Block wet = tone(0, 0.3f);
  const Block dry = tone(3, 0.2f);
  const uint32_t half = kBypassFull / 2;
  const Gains g = gainsOver(half, true, kN);
  Block mixed = wet;
  uint32_t phase = half;
  mixBypass(mixed.data(), dry.data(), kN, true, phase);
  bool match = true;
  for (size_t i = 0; i < mixed.size(); ++i) {
    match = match && std::fabs(mixed[i] - (wet[i] * g.chain[i] + dry[i] * g.dry[i])) < 1e-6f;
  }
  check(match && phase > half, "bypass: a block mid-fade is the chain and the dry input weighted by the two gains");

  // Switching back mid-fade turns the phase around from where it is. The first gains after the turn
  // sit one sample's step from the last gains before it (at most pi/2 of 2400 samples).
  constexpr float kOneSampleStep = 8e-4f;
  const Gains back = gainsOver(phase, false, 1);
  uint32_t turned = phase;
  Block again = wet;
  mixBypass(again.data(), dry.data(), kN, false, turned);
  check(turned < phase, "bypass: BYPASS off mid-fade moves the phase back toward the chain");
  check(std::fabs(back.chain[0] - g.chain.back()) < kOneSampleStep && std::fabs(back.dry[0] - g.dry.back()) < kOneSampleStep,
        "bypass: reversing mid-fade steps the gains smoothly");
}

}  // namespace

void checkChainRun() {
  checkRouting();
  checkEmptyAndPitchSharing();
  checkWalkAgainstHand();
  checkEmptyChain();
  checkPitchCalledOnce();
  checkNanGuard();
  checkStageTiming();
  checkDefaultChain();
  checkBypassEnds();
  checkBypassFade();
  checkBypassMidAndReversal();
}
