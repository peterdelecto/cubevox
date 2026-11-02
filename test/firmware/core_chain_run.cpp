// Checks the audio block's chain walk, the slot toggle crossfade, the late-block dry pass and the
// BYPASS mix on the host. Covers the card to engine routing, the shared PitchFx call, the
// ping-pong buffers, the NaN guard, the stage timing, the shipped layout over several blocks, the
// slot crossfade's gain law and its engine start and stop, a late block's dry pass through the
// output stage, and the BYPASS crossfade.

#include "support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/chain_run.h"
#include "core/compose.h"
#include "core/late_block.h"
#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/output_stage.h"
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
  SlotFades fades;
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
  run.finite = runChain(rig.engines, rig.fades, run.out.data(), scratch.data(), kN, snap, clock, run.cycles);
  return run;
}

// Runs the chain by hand on `rig`'s own engines, with a fresh buffer per stage and a plain copy in
// between. The front half of POLISH EQ goes first, then the slots, then POLISH COMP and the back
// half of POLISH EQ.
Block byHand(Rig& rig, const ChainSnapshot& snap, const Block& in) {
  Block cur = in;
  Block next{};
  rig.engines.polish.processFront(cur.data(), next.data(), kN, snap.params.eq);
  cur = next;
  bool pitchDone = false;
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = snap.layout.card[static_cast<size_t>(s)];
    if (runStage(rig.engines, c, cur.data(), next.data(), kN, snap.params, pitchDone)) cur = next;
  }
  rig.engines.comp.process(cur.data(), next.data(), kN, snap.params.comp);
  rig.engines.polish.processBack(next.data(), cur.data(), kN, snap.params.eq);
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

// The poisoned blocks enter through the input, which the empty layout passes to the guard as a slot's
// output would arrive. The EQ is off for them, because a NaN at the input would reach the front half's
// filters, and the ADC cannot produce one.
void checkNanGuard() {
  const ChainSnapshot snap = snapshotFor(layoutOf({}), 0.5f, true);
  ChainSnapshot eqOff = snap;
  eqOff.params.eq.on = false;
  auto rig = newRig();
  Block poisoned = tone(0, 0.3f);
  poisoned[17] = NAN;
  Run run = runBlock(*rig, eqOff, poisoned, StillClock());
  check(!run.finite && allZero(run.out), "chain: a NaN out of the slots silences the block and reports it");

  poisoned = tone(1, 0.3f);
  poisoned[3] = INFINITY;
  run = runBlock(*rig, eqOff, poisoned, StillClock());
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

// The clock reads twice around the front half of POLISH EQ, twice around each slot, then once before
// POLISH COMP, once between the comp and the back half and once after it. Each bucket gets its own
// pair of reads, and the EQ bucket adds the front half's pair to the back half's.
void checkStageTiming() {
  const ChainSnapshot snap = snapshotFor(defaultLayout(), 0.5f, true);
  auto rig = newRig();
  const Run run = runBlock(*rig, snap, tone(0, 0.3f), SquareClock());
  bool slots = true;
  for (uint32_t s = 0; s < static_cast<uint32_t>(kSlotCount); ++s) {
    const uint32_t at = 2 + 2 * s;  // read index of the slot's first read
    slots = slots && run.cycles.slot[s] == (at + 1) * (at + 1) - at * at;
  }
  check(slots, "chain: each slot is timed by its own pair of reads");
  const uint32_t first = 2 + 2 * static_cast<uint32_t>(kSlotCount);  // read index of the first polish read
  check(run.cycles.comp == (first + 1) * (first + 1) - first * first, "chain: POLISH COMP is timed alone");
  check(run.cycles.eq == 1 + (first + 2) * (first + 2) - (first + 1) * (first + 1),
        "chain: POLISH EQ is timed by the first pair of reads and the last pair");
}

// The front half of POLISH EQ runs ahead of slot 1 in the caller's buffer. With the slots empty and
// POLISH COMP off, the chain's output is the front half then the back half, and a 40 Hz tone loses
// level to the 70 Hz high-pass while the same tone with POLISH EQ OFF passes untouched.
void checkFrontHead() {
  constexpr double kPi = 3.14159265358979323846;
  const auto lowTone = [&](int block) {
    Block x{};
    for (int i = 0; i < kN; ++i) {
      const double t = static_cast<double>(block * kN + i) / cv::kSampleRate;
      x[static_cast<size_t>(i)] = 0.3f * static_cast<float>(std::sin(2.0 * kPi * 40.0 * t));
    }
    return x;
  };
  PanelReading panel;
  panel.toggle.fill(true);
  Settings eqOnly;
  eqOnly.polishComp = false;
  const ChainSnapshot on = composeSnapshot(layoutOf({}), panel, eqOnly, BenchOverrides());
  const ChainSnapshot off = snapshotFor(layoutOf({}), 0.5f, false);
  auto chain = newRig();
  auto bare = newRig();
  auto plain = newRig();
  bool same = true;
  bool passes = true;
  double outRms = 0.0;
  double inRms = 0.0;
  for (int b = 0; b < 400; ++b) {
    const Block in = lowTone(b);
    Block mid{};
    Block want{};
    bare->engines.polish.processFront(in.data(), mid.data(), kN, on.params.eq);
    bare->engines.polish.processBack(mid.data(), want.data(), kN, on.params.eq);
    const Run run = runBlock(*chain, on, in, StillClock());
    same = same && run.out == want;
    passes = passes && runBlock(*plain, off, in, StillClock()).out == in;
    outRms = rms(run.out);
    inRms = rms(in);
  }
  check(same, "chain: the front half runs ahead of slot 1 and the back half after the comp, in the caller's buffer");
  check(passes, "chain: with POLISH EQ OFF both halves pass the block bit for bit");
  check(outRms < 0.2 * inRms, "chain: the 70 Hz high-pass takes a 40 Hz tone down by more than 14 dB");
}

// Runs the shipped layout with every toggle on, the knobs at mid travel and POLISH EQ on.
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

using Toggles = std::array<bool, kSlotCount>;

Toggles allToggles(bool on) {
  Toggles t;
  t.fill(on);
  return t;
}

// Every knob at `knob` and each slot's toggle as given, with both polish stages off, so a slot's
// block is the only thing between the input and the output.
ChainSnapshot snapshotToggles(const Layout& layout, float knob, const Toggles& on) {
  PanelReading panel;
  panel.toggle = on;
  for (auto& slot : panel.knob) slot.fill(knob);
  Settings settings;
  settings.polishComp = false;
  settings.polishEq = false;
  return composeSnapshot(layout, panel, settings, BenchOverrides());
}

// Appends `b` to `all`.
void append(std::vector<float>& all, const Block& b) { all.insert(all.end(), b.begin(), b.end()); }

float maxStep(const std::vector<float>& x) {
  float worst = 0.0f;
  for (size_t i = 1; i < x.size(); ++i) worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
  return worst;
}

float maxAbs(const std::vector<float>& x) {
  float worst = 0.0f;
  for (const float v : x) worst = std::max(worst, std::fabs(v));
  return worst;
}

// The engine and dry gains mixSlotFade applies over one block that moves the fade from level
// `from` to level `to`. The engine gain comes from a block of ones mixed with zeros, and the dry gain
// from zeros mixed with ones.
struct SlotGains {
  std::vector<float> engine;
  std::vector<float> dry;
};

SlotGains slotGains(int from, int to) {
  const std::vector<float> ones(kN, 1.0f);
  const std::vector<float> zeros(kN, 0.0f);
  SlotGains g{ones, zeros};
  mixSlotFade(g.engine.data(), zeros.data(), kN, from, to);
  mixSlotFade(g.dry.data(), ones.data(), kN, from, to);
  return g;
}

// The gain law of the slot crossfade. One whole fade in is 6 blocks of 64 samples, 8 ms, with the
// engine gain rising and the dry gain falling as the sine and cosine of one phase, so the two keep
// equal power at every sample and meet at 0.7071 in the middle.
void checkSlotFadeLaw() {
  std::vector<float> ge;
  std::vector<float> gd;
  for (int level = 0; level < kSlotFadeBlocks; ++level) {
    const SlotGains g = slotGains(level, level + 1);
    ge.insert(ge.end(), g.engine.begin(), g.engine.end());
    gd.insert(gd.end(), g.dry.begin(), g.dry.end());
  }
  check(ge.size() == 384 && kSlotFadeBlocks == 6 && kN == 64 && ge.size() * 1000 / cv::kSampleRate == 8,
        "slot fade: six blocks of 64 samples, 8 ms");
  float worstPower = 0.0f;
  bool monotone = true;
  for (size_t i = 0; i < ge.size(); ++i) {
    worstPower = std::max(worstPower, std::fabs(ge[i] * ge[i] + gd[i] * gd[i] - 1.0f));
    if (i > 0) monotone = monotone && ge[i] >= ge[i - 1] - 1e-6f && gd[i] <= gd[i - 1] + 1e-6f;
  }
  check(worstPower < 2e-5f, "slot fade: the two gains sum to unity power at every sample");
  check(monotone, "slot fade: the engine gain only rises and the dry gain only falls");

  // The midpoint is the last sample of block 3, 192 samples into the 384.
  const SlotGains mid = slotGains(2, 3);
  check(std::fabs(mid.engine.back() - 0.70710678f) < 1e-5f && std::fabs(mid.dry.back() - 0.70710678f) < 1e-5f &&
            std::fabs(mid.engine.back() * mid.engine.back() + mid.dry.back() * mid.dry.back() - 1.0f) < 2e-5f,
        "slot fade: at the midpoint both gains read 0.7071 and sum to unity power");

  const float step = (cv::kHalfPi) / static_cast<float>(ge.size());
  float worstStep = 0.0f;
  for (size_t i = 1; i < ge.size(); ++i) worstStep = std::max(worstStep, std::fabs(ge[i] - ge[i - 1]));
  check(worstStep < 1.01f * step + 2e-6f && step > 0.0040f && step < 0.0042f,
        "slot fade: no gain moves by more than pi/2 over 384 samples, 0.0041 per sample");
  check(ge.front() < 2.0f * step && ge.back() > 0.99999f && gd.back() == 0.0f && gd.front() > 0.9999f,
        "slot fade: the fade in starts at the dry input and ends at the engine alone");

  // A fade out is the same curve backwards: it runs from the sample before the fade in's last one
  // down to the dry end, which is one sample ahead of the fade in's first.
  std::vector<float> geOut;
  for (int level = kSlotFadeBlocks; level > 0; --level) {
    const SlotGains g = slotGains(level, level - 1);
    geOut.insert(geOut.end(), g.engine.begin(), g.engine.end());
  }
  bool mirrored = geOut.size() == ge.size() && geOut.back() == 0.0f;
  for (size_t i = 0; mirrored && i + 1 < ge.size(); ++i) mirrored = std::fabs(geOut[i] - ge[ge.size() - 2 - i]) < 2e-6f;
  check(mirrored, "slot fade: a fade out is the fade in run backwards");

  // A flip during a fade turns it round from where it is: the gains join one sample apart.
  const SlotGains down = slotGains(3, 2);
  const SlotGains up = slotGains(2, 3);
  check(std::fabs(up.engine.front() - down.engine.back()) < 1.01f * step + 2e-6f &&
            std::fabs(up.dry.front() - down.dry.back()) < 1.01f * step + 2e-6f,
        "slot fade: turning a fade round mid-way joins the gains without a step");
}

void checkSlotFadeLevels() {
  SlotFades f;
  check(f.level(0) == kSlotFadeBlocks && f.level(kSlotCount - 1) == kSlotFadeBlocks, "slot fade: a new chain starts on the engines");
  SlotFades::Step s = f.step(2, false);
  check(s.from == kSlotFadeBlocks && s.to == kSlotFadeBlocks - 1 && f.level(2) == kSlotFadeBlocks - 1 && f.level(1) == kSlotFadeBlocks,
        "slot fade: a block moves one slot one level toward its toggle");
  int blocks = 1;
  while (f.level(2) > 0) {
    f.step(2, false);
    ++blocks;
  }
  s = f.step(2, false);
  check(blocks == kSlotFadeBlocks && s.from == 0 && s.to == 0, "slot fade: a fade out takes 6 blocks and then holds at the dry end");
  s = f.step(2, true);
  check(s.from == 0 && s.to == 1, "slot fade: a flip on starts from the dry end");
  f.step(2, true);
  f.step(2, true);
  s = f.step(2, false);
  check(s.from == 3 && s.to == 2, "slot fade: a flip back mid-fade turns round from where it is");
  Toggles on = allToggles(true);
  on[5] = false;
  f.settle(on);
  check(f.level(5) == 0 && f.level(4) == kSlotFadeBlocks && f.level(2) == kSlotFadeBlocks,
        "slot fade: settle puts each slot at the end its toggle selects");
}

// One slot flips off on a steady tone. The engine runs for exactly the 6 fade blocks and stops, the
// output joins without a click, and the dry input passes bit for bit afterwards.
void checkSlotFlipToOff() {
  const Layout layout = layoutOf({Card::Distortion});
  const ChainSnapshot on = snapshotToggles(layout, 1.0f, allToggles(true));
  const ChainSnapshot off = snapshotToggles(layout, 1.0f, allToggles(false));
  auto flipped = newRig();
  auto steady = newRig();
  constexpr int kWarm = 60;
  constexpr int kAfter = 14;
  std::vector<float> outFlip;
  std::vector<float> outSteady;
  std::vector<float> dry;
  std::vector<int> differs;
  for (int b = 0; b < kWarm + kAfter; ++b) {
    const Block in = tone(b, 0.3f);
    const Run a = runBlock(*flipped, b < kWarm ? on : off, in, StillClock());
    const Run c = runBlock(*steady, on, in, StillClock());
    if (b >= kWarm - 2) {
      append(outFlip, a.out);
      append(outSteady, c.out);
      append(dry, in);
    }
    if (b >= kWarm) differs.push_back(a.out != in ? 1 : 0);
  }
  int engineBlocks = 0;
  for (int b = 0; b < kAfter; ++b) engineBlocks += differs[static_cast<size_t>(b)];
  bool contiguous = true;
  for (int b = 0; b < kSlotFadeBlocks; ++b) contiguous = contiguous && differs[static_cast<size_t>(b)] == 1;
  for (int b = kSlotFadeBlocks; b < kAfter; ++b) contiguous = contiguous && differs[static_cast<size_t>(b)] == 0;
  check(engineBlocks == kSlotFadeBlocks && contiguous,
        "slot flip: after a flip to off the engine runs for exactly the 6 fade blocks, then the dry input passes bit for bit");

  // The step bound. Each gain moves 0.0041 per sample at most, on the difference between the two
  // paths, on top of the paths' own slope. A hard switch would put the whole difference between two
  // samples.
  const float step = cv::kHalfPi / static_cast<float>(kSlotFadeBlocks * kN);
  const float naturalStep = std::max(maxStep(outSteady), maxStep(dry));
  const float allowed = 1.4143f * naturalStep + step * (maxAbs(outSteady) + maxAbs(dry));
  float hardStep = 0.0f;
  for (size_t i = 0; i < dry.size(); ++i) hardStep = std::max(hardStep, std::fabs(outSteady[i] - dry[i]));
  const float worst = maxStep(outFlip);
  check(worst <= allowed, "slot flip: no step in the output is larger than the fade allows");
  check(worst < 0.5f * hardStep, "slot flip: the largest step is well under the step of a hard switch between the two paths");
}

// The engine of a slot that comes back on starts cleared. A Slapback that holds a full delay line,
// bypassed, then flipped on with silent input must stay silent, where an engine that kept running
// would still be playing its echo.
void checkSlotFlipOnClearsEngine() {
  const Layout layout = layoutOf({Card::Slapback});
  const ChainSnapshot on = snapshotToggles(layout, 0.5f, allToggles(true));
  const ChainSnapshot off = snapshotToggles(layout, 0.5f, allToggles(false));
  auto flipped = newRig();
  auto steady = newRig();
  constexpr int kWarm = 400;
  int b = 0;
  for (; b < kWarm; ++b) {
    runBlock(*flipped, on, tone(b, 0.3f), StillClock());
    runBlock(*steady, on, tone(b, 0.3f), StillClock());
  }
  for (; b < kWarm + 12; ++b) {  // bypassed: 6 fade blocks and 6 settled
    runBlock(*flipped, off, tone(b, 0.3f), StillClock());
    runBlock(*steady, on, tone(b, 0.3f), StillClock());
  }
  const Block silence{};
  bool silent = true;
  double steadyEnergy = 0.0;
  for (int i = 0; i < 2 * kSlotFadeBlocks; ++i) {
    const Run a = runBlock(*flipped, on, silence, StillClock());
    const Run c = runBlock(*steady, on, silence, StillClock());
    silent = silent && allZero(a.out);
    steadyEnergy += rms(c.out);
  }
  check(silent, "slot flip: a flip back on starts from a cleared engine, so silent input stays silent through the fade in");
  check(steadyEnergy > 0.01, "slot flip: the same engine left running was still playing its echo, so the clear is what silenced it");
}

// A flip back during the fade out turns the fade round without clearing the running engine.
void checkSlotFlipReverses() {
  const Layout layout = layoutOf({Card::Slapback});
  const ChainSnapshot on = snapshotToggles(layout, 0.5f, allToggles(true));
  const ChainSnapshot off = snapshotToggles(layout, 0.5f, allToggles(false));
  auto rig = newRig();
  auto steady = newRig();
  int b = 0;
  for (; b < 400; ++b) {
    runBlock(*rig, on, tone(b, 0.3f), StillClock());
    runBlock(*steady, on, tone(b, 0.3f), StillClock());
  }
  std::vector<float> out;
  std::vector<float> dry;
  std::vector<float> ref;
  std::vector<int> levels;
  for (int k = 0; k < 8; ++k, ++b) {
    const Block in = tone(b, 0.3f);
    const bool flipOn = k >= 3;  // three blocks of fade out, then the toggle returns
    const Run a = runBlock(*rig, flipOn ? on : off, in, StillClock());
    const Run c = runBlock(*steady, on, in, StillClock());
    append(out, a.out);
    append(ref, c.out);
    append(dry, in);
    levels.push_back(rig->fades.level(0));
  }
  const std::vector<int> want = {5, 4, 3, 4, 5, 6, 6, 6};
  check(levels == want, "slot flip: the fade turns round from level 3 and climbs back to the engine in 3 blocks");
  float echo = 0.0f;
  for (size_t i = 3 * kN; i < out.size(); ++i) echo = std::max(echo, std::fabs(out[i] - dry[i]));
  check(echo > 0.01f, "slot flip: the engine was not cleared by the turn, its echo is still in the output");
  const float step = cv::kHalfPi / static_cast<float>(kSlotFadeBlocks * kN);
  const float allowed = 1.4143f * std::max(maxStep(ref), maxStep(dry)) + step * (maxAbs(ref) + maxAbs(dry));
  check(maxStep(out) <= allowed, "slot flip: the turn leaves no step larger than the fade allows");
}

// The Input gate's engine runs while its slot is off, because its detector feeds the status reply's
// floor_dbfs. A Gate card stops with its slot. Either way the dry input passes bit for bit.
void checkInputGateKeepsDetector() {
  const Block in = tone(0, 0.3f);
  for (const bool inputGate : {true, false}) {
    const Layout layout = layoutOf({inputGate ? Card::InputGate : Card::Gate});
    const ChainSnapshot off = snapshotToggles(layout, 0.5f, allToggles(false));
    auto rig = newRig();
    bool same = true;
    for (int b = 0; b < 30; ++b) {
      const Block x = tone(b, 0.3f);
      const Run run = runBlock(*rig, off, x, StillClock());
      if (b >= 2 * kSlotFadeBlocks) same = same && run.out == x;
    }
    const float peak = inputGate ? rig->engines.inputGate.takeDetectorPeak() : rig->engines.gate.takeDetectorPeak();
    runBlock(*rig, off, in, StillClock());
    const float after = inputGate ? rig->engines.inputGate.takeDetectorPeak() : rig->engines.gate.takeDetectorPeak();
    if (inputGate) {
      check(peak > 0.1f && after > 0.1f && same, "slot flip: the Input gate's detector keeps reading with its toggle off and the block passes untouched");
    } else {
      check(peak > 0.0f && after == 0.0f && same, "slot flip: a Gate card stops with its toggle off and the block passes untouched");
    }
  }
}

// Autotune and Octave share one PitchFx call at the first pitch card's slot. The stage follows either
// toggle, and stops only when both are off.
void checkPitchStageToggles() {
  const Layout layout = layoutOf({Card::Autotune, Card::Octave});
  Toggles bothOff = allToggles(false);
  Toggles octaveOnly = bothOff;
  octaveOnly[1] = true;
  ChainSnapshot snapOctave = snapshotToggles(layout, 0.5f, octaveOnly);
  snapOctave.params.pitchFx.octave.semitones = 12;
  snapOctave.params.pitchFx.octave.mix = 1.0f;
  ChainSnapshot snapOff = snapshotToggles(layout, 0.5f, bothOff);
  snapOff.params.pitchFx.octave.semitones = 12;
  snapOff.params.pitchFx.octave.mix = 1.0f;

  auto rig = newRig();
  bool running = true;
  for (int b = 0; b < 60; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*rig, snapOctave, in, StillClock());
    if (b >= 20) running = running && run.out != in;
  }
  check(running, "slot flip: with Autotune off and Octave on the pitch stage still runs at Autotune's slot");
  check(rig->fades.level(0) == kSlotFadeBlocks, "slot flip: the pitch stage's slot stays on the engine while either toggle is on");

  int engineBlocks = 0;
  bool dryAfter = true;
  for (int b = 60; b < 60 + 14; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*rig, snapOff, in, StillClock());
    if (b < 60 + kSlotFadeBlocks) engineBlocks += run.out != in ? 1 : 0;
    if (b >= 60 + kSlotFadeBlocks) dryAfter = dryAfter && run.out == in;
  }
  check(engineBlocks == kSlotFadeBlocks && dryAfter,
        "slot flip: with both pitch toggles off the stage runs for the 6 fade blocks, then passes the dry input bit for bit");
}

// 200 ms of the tone: the tracker finds its period in about 32 ms and the voices sound at least
// 22 ms after that.
constexpr int kCrowdBlocks = 150;

// The pitch stage, then the Crowd card on its output, on `rig`'s own engines. This is the order the
// chain makes the two calls when nothing else sits between them.
Block pitchThenCrowd(Rig& rig, const ChainSnapshot& snap, const Block& in) {
  Block mid{};
  Block out{};
  rig.engines.pitch.process(in.data(), mid.data(), kN, snap.params.pitchFx);
  rig.engines.crowd.process(mid.data(), out.data(), kN, snap.params.crowd, rig.engines.pitch);
  return out;
}

struct CrowdOutcome {
  bool sameAsHand = true;  // every block equals the pitch call then the Crowd card
  bool heard = false;      // the second half of the run differs from the input
  bool live = false;       // the tracker holds a period and a voice sounds
};

// Every knob at 0.5 except the Crowd card's VOICES, which stands at step 4 so its copies sound. At 0.5 a
// stepped knob lands on its first position, where the Crowd card plays no copy.
ChainSnapshot snapshotCrowd(const Layout& layout, const Toggles& on) {
  ChainSnapshot snap = snapshotToggles(layout, 0.5f, on);
  snap.params.crowd.voicesStep = 4;
  return snap;
}

CrowdOutcome runCrowdLayout(const Layout& layout, const Toggles& on) {
  const ChainSnapshot snap = snapshotCrowd(layout, on);
  auto chain = newRig();
  auto hand = newRig();
  CrowdOutcome o;
  for (int b = 0; b < kCrowdBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*chain, snap, in, StillClock());
    o.sameAsHand = o.sameAsHand && run.out == pitchThenCrowd(*hand, snap, in);
    if (b >= kCrowdBlocks / 2) o.heard = o.heard || run.out != in;
  }
  o.live = chain->engines.pitch.pitch().period > 0.0f && chain->engines.crowd.activeVoices() > 0;
  return o;
}

Toggles togglesOn(std::initializer_list<int> slots) {
  Toggles t = allToggles(false);
  for (const int s : slots) t[static_cast<size_t>(s)] = true;
  return t;
}

void checkCrowdStage() {
  auto rig = newRig();
  ChainParams p;
  const Block in = tone(0, 0.3f);
  Block out;
  out.fill(7.0f);
  bool done = false;
  check(runStage(rig->engines, Card::Crowd, in.data(), out.data(), kN, p, done) && done && out == in,
        "stage: a Crowd card that is off makes the pitch call and passes the block untouched");

  // With the pitch stage already run this pass, the Crowd card runs alone and nothing feeds the tracker.
  // With no period the card holds its lead at 1 and its copies at 0, so every block comes out as it went in.
  p.crowd.on = true;
  p.pitchFx.crowdOn = true;
  bool wrote = true;
  bool unity = true;
  for (int b = 0; b < 20; ++b) {
    done = true;
    wrote = wrote && runStage(rig->engines, Card::Crowd, tone(b, 0.3f).data(), out.data(), kN, p, done);
    unity = unity && out == tone(b, 0.3f);
  }
  check(wrote && unity && rig->engines.pitch.pitch().period == 0.0f && rig->engines.crowd.leadGain() == 1.0f,
        "stage: a Crowd card after the pitch stage ran runs only its own engine");
}

// Wherever the Crowd card sits against Autotune and Octave, the pitch stage runs at the first of the
// three, while the Crowd card is on, even with both pitch toggles off, and the Crowd card works on its output.
void checkCrowdPitchStage() {
  const CrowdOutcome alone = runCrowdLayout(layoutOf({Card::Crowd}), togglesOn({0}));
  check(alone.sameAsHand && alone.heard && alone.live, "crowd: alone in a layout it gets a live ring and tracker");

  const CrowdOutcome ahead = runCrowdLayout(layoutOf({Card::Crowd, Card::Autotune}), togglesOn({0}));
  check(ahead.sameAsHand && ahead.heard && ahead.live,
        "crowd: ahead of Autotune with Autotune off it still gets a live ring and tracker");

  const CrowdOutcome aheadBoth = runCrowdLayout(layoutOf({Card::Crowd, Card::Autotune}), togglesOn({0, 1}));
  check(aheadBoth.sameAsHand && aheadBoth.heard && aheadBoth.live,
        "crowd: ahead of Autotune with both on, the pitch stage runs at the Crowd card's slot and feeds it");

  const CrowdOutcome behind =
      runCrowdLayout(layoutOf({Card::Autotune, Card::Octave, Card::Crowd}), togglesOn({2}));
  check(behind.sameAsHand && behind.heard && behind.live,
        "crowd: behind both pitch cards with both off, the pitch stage still runs and feeds it");

  const CrowdOutcome all = runCrowdLayout(layoutOf({Card::Autotune, Card::Crowd}), togglesOn({0, 1}));
  check(all.sameAsHand && all.heard && all.live, "crowd: behind Autotune with both on, one pitch call per block");

  auto rig = newRig();
  const ChainSnapshot snap = snapshotCrowd(layoutOf({Card::Autotune, Card::Octave, Card::Crowd}), togglesOn({2}));
  for (int b = 0; b < kCrowdBlocks; ++b) runBlock(*rig, snap, tone(b, 0.3f), StillClock());
  check(rig->fades.level(0) == kSlotFadeBlocks,
        "crowd: the pitch stage's slot stays on the engine while only the Crowd card's toggle is on");
}

// The Crowd's toggle off fades its slot out, and the pitch stage with it. After the fade the dry
// input passes bit for bit.
void checkCrowdToggleOff() {
  const Layout layout = layoutOf({Card::Crowd});
  const ChainSnapshot on = snapshotCrowd(layout, togglesOn({0}));
  const ChainSnapshot off = snapshotCrowd(layout, allToggles(false));
  auto rig = newRig();
  for (int b = 0; b < kCrowdBlocks; ++b) runBlock(*rig, on, tone(b, 0.3f), StillClock());
  bool dryAfter = true;
  for (int b = kCrowdBlocks; b < kCrowdBlocks + 14; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*rig, off, in, StillClock());
    if (b >= kCrowdBlocks + kSlotFadeBlocks) dryAfter = dryAfter && run.out == in;
  }
  check(dryAfter, "crowd: with its toggle off the slot fades out in 6 blocks, then passes the dry input bit for bit");
  check(rig->engines.crowd.leadGain() == 1.0f, "crowd: the engine ramped back to the lead alone before its slot stopped");
}

// Slot 3 is the third slot, index 2, where Formant stands in for Octave. Slot 7 is the seventh, index 6,
// where Lo-fi and Chatter stand in for Gate.
constexpr int kSlot3 = 2;
constexpr int kSlot7 = 6;

// 200 ms of the tone, as for the Crowd card: the tracker finds its period in about 32 ms, the Formant
// voice sounds 22 ms after that, and the Chatter card's first capture lands at 40 to 50 ms and loops by about 110 ms.
constexpr int kSlot7Blocks = 150;

// A layout with `c` in its slot, slot 3 for Formant and slot 7 for the others, and every other slot empty.
Layout slot7Only(Card c) {
  Layout layout = layoutOf({});
  layout.card[c == Card::Formant ? kSlot3 : kSlot7] = c;
  return layout;
}

// The engine of the card called by hand on `rig`'s own engines with the snapshot's params.
Block slot7Direct(Rig& rig, Card c, const ChainSnapshot& snap, const Block& in) {
  Block out{};
  switch (c) {
    case Card::Formant: rig.engines.pitch.process(in.data(), out.data(), kN, snap.params.pitchFx); break;
    case Card::Lofi: rig.engines.lofi.process(in.data(), out.data(), kN, snap.params.lofi); break;
    case Card::Bloom: rig.engines.bloom.process(in.data(), out.data(), kN, snap.params.bloom); break;
    default: break;
  }
  return out;
}

// Formant in slot 3 and Lo-fi and Chatter in slot 7 each run their engine, change the tone, and pass the
// dry input bit for bit once their toggle has been off for the fade. A reset clears the Lo-fi and
// Chatter engines.
void checkSlot7CardsRun() {
  for (const Card c : {Card::Formant, Card::Lofi, Card::Bloom}) {
    const std::string name = cardDesc(c).name;
    const ChainSnapshot snap = snapshotToggles(slot7Only(c), 0.5f, allToggles(true));
    auto chain = newRig();
    auto hand = newRig();
    bool same = true;
    bool finite = true;
    bool heard = false;
    for (int b = 0; b < kSlot7Blocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      same = same && run.out == slot7Direct(*hand, c, snap, in);
      finite = finite && run.finite && allFinite(run.out);
      if (b >= kSlot7Blocks / 2) heard = heard || run.out != in;
    }
    check(same, (name + " runs its engine over the block in its slot").c_str());
    check(finite && heard, (name + " stays finite and changes the tone in its slot").c_str());

    const ChainSnapshot off = snapshotToggles(slot7Only(c), 0.5f, allToggles(false));
    auto settled = newRig();
    bool dry = true;
    for (int b = 0; b < 3 * kSlotFadeBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*settled, off, in, StillClock());
      if (b >= kSlotFadeBlocks) dry = dry && run.out == in;
    }
    check(dry, (name + " with its toggle off passes the dry input bit for bit after the fade").c_str());
  }

  const ChainSnapshot bloom = snapshotToggles(slot7Only(Card::Bloom), 0.5f, allToggles(true));
  auto rig = newRig();
  for (int b = 0; b < kSlot7Blocks; ++b) runBlock(*rig, bloom, tone(b, 0.3f), StillClock());
  check(rig->engines.bloom.inWord() && rig->engines.bloom.looping(),
        "Chatter with no Input gate placed keys on its own input, so a word opens and its slice loops");
  resetEngine(rig->engines, Engine::Bloom);
  check(!rig->engines.bloom.inWord() && !rig->engines.bloom.looping() && rig->engines.bloom.wetGain() == 0.0f,
        "resetEngine clears the Chatter card's word and its loop");

  const ChainSnapshot lofi = snapshotToggles(slot7Only(Card::Lofi), 0.5f, allToggles(true));
  auto lofiRig = newRig();
  for (int b = 0; b < 40; ++b) runBlock(*lofiRig, lofi, tone(b, 0.3f), StillClock());
  resetEngine(lofiRig->engines, Engine::Lofi);
  check(allZero(runBlock(*lofiRig, lofi, Block{}, StillClock()).out), "resetEngine clears the Lo-fi engine, so silence in is silence out");
}

// Formant acts at the first pitch-group card's slot, wherever its own card sits. Its toggle alone keeps
// the stage and its tracker running: with Autotune off, in slot 1 or in slot 3, the pitch call goes
// through at the first of the two and a Formant card behind Autotune writes nothing of its own.
void checkFormantPitchStage() {
  Layout behind = layoutOf({Card::Autotune});
  behind.card[kSlot3] = Card::Formant;
  Layout ahead = layoutOf({Card::Formant});
  ahead.card[kSlot3] = Card::Autotune;
  const std::initializer_list<std::pair<const char*, Layout>> layouts = {
      {"Formant in slot 3 behind Autotune in slot 1", behind}, {"Formant in slot 1 ahead of Autotune in slot 3", ahead}};
  for (const auto& entry : layouts) {
    const bool formantBehind = entry.second.card[kSlot3] == Card::Formant;
    const ChainSnapshot snap = snapshotToggles(entry.second, 0.5f, togglesOn({formantBehind ? kSlot3 : 0}));
    auto chain = newRig();
    auto hand = newRig();
    bool same = true;
    bool heard = false;
    for (int b = 0; b < kSlot7Blocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      Block want{};
      hand->engines.pitch.process(in.data(), want.data(), kN, snap.params.pitchFx);
      same = same && run.out == want;
      if (b >= kSlot7Blocks / 2) heard = heard || run.out != in;
    }
    const std::string name = entry.first;
    check(snap.params.pitchFx.formant.on && !snap.params.pitchFx.autotune.on,
          (name + ": only Formant's toggle is on").c_str());
    check(same && heard, (name + ": the stage runs on Formant's toggle alone and the chain equals the pitch call").c_str());
    check(chain->engines.pitch.pitch().period > 0.0f, (name + ": the tracker holds a period").c_str());
    check(chain->fades.level(0) == kSlotFadeBlocks,
          (name + ": the stage's slot, the first pitch-group slot, stays on the engine while only Formant's toggle is on")
              .c_str());
  }
}

// The Chatter card keys on the Input gate's detector when the layout places that card, whether or not
// its slot is on, and on its own input when it does not. A gate held shut by a 0 dB threshold keeps the
// card out of a word the tone would open on its own detector at -45 dBFS.
void checkBloomKey() {
  Layout layout = layoutOf({Card::InputGate});
  layout.card[kSlot7] = Card::Bloom;
  const Toggles bloomOnly = togglesOn({kSlot7});
  struct Outcome {
    bool same = true;      // every block equals the gate's call, then the Chatter card keyed by its state
    bool inWord = false;   // the card holds a word at the end
    bool looping = false;  // a slice loops at the end
    float residual = 0.0f;  // the last block's largest distance from the input under its best single gain
    float gain = 0.0f;      // that best single gain
  };
  const auto play = [&](float gateDb, bool gatePlaced) {
    Layout l = layout;
    if (!gatePlaced) l.card[0] = Card::Empty;
    ChainSnapshot snap = snapshotToggles(l, 0.5f, bloomOnly);
    snap.params.inputGate.thresholdDb = gateDb;
    auto chain = newRig();
    chain->fades.settle(snap.slotOn);  // the Input gate's slot starts settled off, so it passes the block untouched
    auto hand = newRig();
    Outcome o;
    for (int b = 0; b < kSlot7Blocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      Block gated{};
      hand->engines.inputGate.process(in.data(), gated.data(), kN, snap.params.inputGate);
      cv::BloomParams keyed = snap.params.bloom;
      keyed.keyFromGate = gatePlaced;
      Block want{};
      hand->engines.bloom.process(in.data(), want.data(), kN, keyed, gatePlaced && hand->engines.inputGate.open());
      o.same = o.same && run.out == want;
      if (b == kSlot7Blocks - 1) {
        // Outside a word the block is the dry input, one gain. A loop adds to it.
        double dot = 0.0;
        double energy = 0.0;
        for (size_t i = 0; i < in.size(); ++i) {
          dot += static_cast<double>(run.out[i]) * static_cast<double>(in[i]);
          energy += static_cast<double>(in[i]) * static_cast<double>(in[i]);
        }
        const float gain = static_cast<float>(dot / energy);
        o.gain = gain;
        for (size_t i = 0; i < in.size(); ++i) o.residual = std::max(o.residual, std::fabs(run.out[i] - gain * in[i]));
      }
    }
    o.inWord = chain->engines.bloom.inWord();
    o.looping = chain->engines.bloom.looping();
    return o;
  };

  // A loop on a steady tone can land in phase with it, so a loop shows as a residual off the best single
  // gain or as that gain moving off the dry's own, which the shut gate's run gives.
  const Outcome shut = play(0.0f, true);
  const auto addsLoop = [&](const Outcome& o) { return o.residual > 0.01f || std::fabs(o.gain - shut.gain) > 0.05f; };
  const Outcome open = play(-55.0f, true);
  check(open.same && open.inWord && open.looping && addsLoop(open),
        "Chatter: an open Input gate keys a word, and the chain equals the card keyed by the gate's state");
  check(shut.same && !shut.inWord && !shut.looping && shut.residual < 1e-5f,
        "Chatter: a shut Input gate keeps the card out of a word the tone would open on its own detector");
  const Outcome own = play(0.0f, false);
  check(own.same && own.inWord && own.looping && addsLoop(own),
        "Chatter: with no Input gate placed it keys on its own input, whatever the gate's threshold");
}

// What the Chatter card cut from a run of the chain: how many finished slices it holds, whether every
// one equals the pitch stage's output at its cut clock, and how many of their samples differ from the
// Crowd card's output.
struct SourceRun {
  int slices = 0;
  bool matchesLead = true;
  int differs = 0;
  int samples = 0;
  bool tapIdle = false;  // the tap buffer was never written
};

// Runs `layout` through the chain, and the pitch stage and the Crowd card by hand beside it.
SourceRun runSource(const Layout& layout, const Toggles& toggles) {
  constexpr int kBlocks = 250;
  const ChainSnapshot snap = snapshotCrowd(layout, toggles);
  auto chain = newRig();
  auto hand = newRig();
  chain->fades.settle(snap.slotOn);
  std::vector<float> lead;
  std::vector<float> voiced;
  for (int b = 0; b < kBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    runBlock(*chain, snap, in, StillClock());
    Block front{};
    hand->engines.polish.processFront(in.data(), front.data(), kN, snap.params.eq);
    Block p{};
    hand->engines.pitch.process(front.data(), p.data(), kN, snap.params.pitchFx);
    Block m{};
    hand->engines.crowd.process(p.data(), m.data(), kN, snap.params.crowd, hand->engines.pitch);
    append(lead, p);
    append(voiced, m);
  }
  SourceRun r;
  const cv::Bloom& bloom = chain->engines.bloom;
  for (int slot = 0; slot < cv::Bloom::kSlices; ++slot) {
    if (!bloom.sliceValid(slot)) continue;
    ++r.slices;
    const size_t cut = bloom.sliceCutAt(slot);

    // A slice is the pitch stage's output through the card's 11 kHz low-pass, started clear at its cut.
    // Its first and last 300 samples carry the pass window (278 samples) and are left out.
    constexpr int kEdge = 300;
    cv::Biquad lp;
    lp.setLowPass(cv::Bloom::kLowpassHz, cv::Bloom::kLowpassQ);
    for (int j = 0; j < bloom.sliceLen(slot); ++j) {
      const float want = lp.run(lead[cut + static_cast<size_t>(j)]);
      if (j < kEdge || j >= bloom.sliceLen(slot) - kEdge) continue;
      const float s = bloom.sliceData(slot)[j];
      if (std::fabs(s - want) > 1e-5f) r.matchesLead = false;
      if (std::fabs(s - voiced[cut + static_cast<size_t>(j)]) > 1e-3f) ++r.differs;
      ++r.samples;
    }
  }
  r.tapIdle = std::all_of(chain->engines.bloomSrc.begin(), chain->engines.bloomSrc.end(), [](float v) { return v == 0.0f; });
  return r;
}

// The Chatter card cuts its slices from the block leaving the last Autotune, Octave or Formant slot (slot
// 1 when the layout has none), and its dry path is slot 7's own input. With MOB in slot 4 the slices
// match the pitch stage's output and differ from MOB's; with MOB ahead of Octave they still do; with no
// Chatter card placed, or the card ahead of the tap, nothing is copied.
void checkBloomSource() {
  Layout def = defaultLayout();
  def.card[kSlot7] = Card::Bloom;
  const BloomTap defTap = bloomTap(def);
  check(defTap.slot == 2 && defTap.pitchCard, "chatter tap: the default layout with Chatter in slot 7 taps the block leaving Octave in slot 3");

  Layout formant = layoutOf({Card::Autotune, Card::Empty, Card::Formant});
  formant.card[kSlot7] = Card::Bloom;
  check(bloomTap(formant).slot == 2, "chatter tap: the last of Autotune, Octave and Formant sets the slot");

  Layout none = layoutOf({Card::InputGate, Card::Unison});
  none.card[kSlot7] = Card::Bloom;
  const BloomTap noneTap = bloomTap(none);
  check(noneTap.slot == 0 && !noneTap.pitchCard, "chatter tap: a layout with no pitch card taps the block leaving slot 1");

  check(bloomTap(defaultLayout()).slot == -1, "chatter tap: a layout with no Chatter card copies nothing");
  const BloomTap ahead = bloomTap(layoutOf({Card::Bloom, Card::Empty, Card::Octave}));
  check(ahead.slot == -1, "chatter tap: the card ahead of the tap slot cuts from its own input");

  Layout behind = layoutOf({Card::InputGate, Card::Autotune, Card::Octave, Card::Crowd});
  behind.card[kSlot7] = Card::Bloom;
  const SourceRun a = runSource(behind, togglesOn({1, 2, 3, kSlot7}));
  check(a.slices >= 2 && a.matchesLead && a.samples > 0 && a.differs > a.samples / 2,
        "chatter source: with MOB in slot 4 the captured slices equal the pitch stage's output and differ from MOB's");

  Layout crowdFirst = layoutOf({Card::InputGate, Card::Crowd, Card::Octave});
  crowdFirst.card[kSlot7] = Card::Bloom;
  const SourceRun c = runSource(crowdFirst, togglesOn({1, 2, kSlot7}));
  check(c.slices >= 2 && c.matchesLead && c.differs > c.samples / 2,
        "chatter source: with MOB ahead of Octave and making the pitch call, the slices still equal the pitch stage's output");

  const SourceRun plain = runSource(none, togglesOn({1, kSlot7}));
  check(plain.slices >= 2 && !plain.tapIdle,
        "chatter source: with no pitch card the slices come from the block leaving slot 1");

  const SourceRun absent = runSource(defaultLayout(), allToggles(true));
  check(absent.tapIdle, "chatter source: a layout with no Chatter card never writes the tap");
  Layout early = layoutOf({Card::Bloom, Card::Empty, Card::Octave});
  const SourceRun first = runSource(early, togglesOn({0, 2}));
  check(first.tapIdle && first.slices >= 2, "chatter source: the card ahead of the tap slot writes no tap and cuts from its own input");
}

// The shipped layout with every toggle on is the plain chain: the crossfade changes nothing while no
// toggle moves, and a fresh chain given a settled-off slot never starts its engine.
void checkSettledSlotsAreInert() {
  const Layout layout = layoutOf({Card::Slapback, Card::Distortion});
  Toggles first = allToggles(true);
  first[0] = false;
  const ChainSnapshot snap = snapshotToggles(layout, 0.5f, first);
  auto rig = newRig();
  rig->fades.settle(snap.slotOn);
  auto hand = newRig();
  float worst = 0.0f;
  for (int b = 0; b < 6; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*rig, snap, in, StillClock());
    // By hand: slot 0 is off and stopped, slot 1 runs its engine on the input.
    Block next{};
    bool pitchDone = false;
    runStage(hand->engines, Card::Distortion, in.data(), next.data(), kN, snap.params, pitchDone);
    worst = std::max(worst, maxAbsDiff(run.out, next));
  }
  check(worst == 0.0f, "slot flip: a slot settled off runs no engine and the next slot gets the dry input");
}

// A late block raises the flag. The next block skips the chain and plays the dry input through the
// output stage, the dry counter reads 1, and the block after runs the chain again.
void checkLateBlockPassesDry() {
  const Layout layout = layoutOf({Card::Distortion});
  const ChainSnapshot snap = snapshotToggles(layout, 1.0f, allToggles(true));
  auto rig = newRig();
  auto reference = newRig();
  auto other = newRig();
  LateBlocks late;
  OutputGain gain;
  const float mic = std::pow(10.0f, kMicTrimDb / 20.0f);
  gain.next(mic, 1.0f, 1.0f, kN);  // the trim has settled on MIC, so every block's ramp is flat at `mic`
  uint32_t atFull = kBypassFull;

  // The output stage over a block, as writeOutput applies it.
  const auto throughStage = [&](const Block& x) {
    const GainRamp ramp = gain.next(mic, 1.0f, 1.0f, kN);
    Block shaped{};
    float g = ramp.start;
    for (int i = 0; i < kN; ++i) {
      shaped[static_cast<size_t>(i)] = shapeOutput(x[static_cast<size_t>(i)], g);
      g += ramp.step;
    }
    return shaped;
  };
  const auto runOn = [&](Rig& r, Block x) {
    Block scratch{};
    ChainCycles cycles;
    runChain(r.engines, r.fades, x.data(), scratch.data(), kN, snap, StillClock(), cycles);
    return x;
  };

  // One block as processHalf does it: take the flag, then the chain or the dry pass by the BYPASS
  // mix settled at full dry, then the output stage.
  bool lastWasDry = false;
  const auto processBlock = [&](const Block& in) {
    lastWasDry = late.takeDry(false);
    Block a = in;
    if (lastWasDry) {
      mixBypass(a.data(), in.data(), kN, true, atFull);
    } else {
      a = runOn(*rig, in);
    }
    return throughStage(a);
  };

  // Loud input, so the ceiling shows in the comparison.
  const Block in0 = tone(0, 1.5f);
  const Block in1 = tone(1, 1.5f);
  const Block in2 = tone(2, 1.5f);
  const Block out0 = processBlock(in0);
  check(!lastWasDry && late.dryBlocks() == 0 && out0 == throughStage(runOn(*reference, in0)),
        "late block: with no flag the block runs the chain");
  late.markLate();
  const Block out1 = processBlock(in1);
  check(lastWasDry && late.dryBlocks() == 1, "late block: the block after a late one passes the dry input and counts 1");
  check(out1 == throughStage(in1), "late block: its output is the dry input through the output stage, trim and ceiling");
  check(out1 != throughStage(runOn(*other, in1)), "late block: the dry block is the input, not the chain's output");
  const Block out2 = processBlock(in2);
  check(!lastWasDry && late.dryBlocks() == 1, "late block: the flag clears, so the block after the dry one runs the chain");

  // The reference ran blocks 0 and 2 only, so it has the engine history the dry block left behind.
  check(out2 == throughStage(runOn(*reference, in2)),
        "late block: the block after the dry one is the chain's output, the engines one block stale");

  // A block that skips the chain anyway takes the flag without counting it.
  late.markLate();
  check(!late.takeDry(true) && late.dryBlocks() == 1 && !late.takeDry(false),
        "late block: a silent block clears the flag and is not counted");
  late.markLate();
  late.clearCount();
  check(late.takeDry(false) && late.dryBlocks() == 1, "late block: the count restarts from clearCount");
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
  checkFrontHead();
  checkDefaultChain();
  checkBypassEnds();
  checkBypassFade();
  checkBypassMidAndReversal();
  checkSlotFadeLaw();
  checkSlotFadeLevels();
  checkSlotFlipToOff();
  checkSlotFlipOnClearsEngine();
  checkSlotFlipReverses();
  checkInputGateKeepsDetector();
  checkPitchStageToggles();
  checkCrowdStage();
  checkCrowdPitchStage();
  checkCrowdToggleOff();
  checkSlot7CardsRun();
  checkFormantPitchStage();
  checkBloomKey();
  checkBloomSource();
  checkSettledSlotsAreInert();
  checkLateBlockPassesDry();
}
