// Checks the audio block's chain walk, the slot toggle crossfade and the BYPASS mix on the host.
// Covers the card to engine routing, the pitch path between slots, the ping-pong buffers, the NaN guard,
// the stage timing, the shipped layout over several blocks, the slot crossfade's gain law and its
// engine start and stop, and the BYPASS crossfade. The late block's dry pass is in core_block_tail.cpp.

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
  std::unique_ptr<cv::AutotuneFx> autotune = std::make_unique<cv::AutotuneFx>();
  std::unique_ptr<cv::OctaveFx> octave = std::make_unique<cv::OctaveFx>();
  std::unique_ptr<cv::Unison> unison = std::make_unique<cv::Unison>();
  std::unique_ptr<cv::Distortion> distortion = std::make_unique<cv::Distortion>();
  ChainEngines engines{*autotune, *octave, *unison, *distortion};
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
  settings.voxEq = polish ? VoxEq::Cuts : VoxEq::Off;
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
// between. The front half of VOX EQ goes first, then the slots, then POLISH COMP and the back
// half of VOX EQ.
Block byHand(Rig& rig, const ChainSnapshot& snap, const Block& in) {
  Block cur = in;
  Block next{};
  rig.engines.polish.processFront(cur.data(), next.data(), kN, snap.params.eq);
  cur = next;
  PitchPath path;
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = snap.layout.card[static_cast<size_t>(s)];
    if (runStage(rig.engines, c, cur.data(), next.data(), kN, snap.params, path)) cur = next;
  }
  rig.engines.comp.process(cur.data(), next.data(), kN, snap.params.comp);
  rig.engines.polish.processBack(next.data(), cur.data(), kN, snap.params.eq);
  return cur;
}

void checkRouting() {
  ChainParams p;  // the Input gate is on and the Gate is off
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
      PitchPath path;
      bool copied = true;
      for (int b = 0; b < 50; ++b) {
        in = tone(b, 0.3f);
        const bool wrote = runStage(rig->engines, c, in.data(), out.data(), kN, params, path);
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

void checkEmptyAndPitchCards() {
  auto rig = newRig();
  const ChainParams p;
  const Block in = tone(0, 0.3f);
  Block out;
  out.fill(7.0f);
  PitchPath path;
  check(!runStage(rig->engines, Card::Empty, in.data(), out.data(), kN, p, path) && out[0] == 7.0f && !path.autotuneAhead,
        "stage: an Empty slot writes nothing");
  check(runStage(rig->engines, Card::Autotune, in.data(), out.data(), kN, p, path) && path.autotuneAhead && path.adjacent &&
            path.upstream.result == &rig->engines.autotune.pitch(),
        "stage: Autotune runs its own engine and hands its tracker on");
  out.fill(7.0f);
  check(runStage(rig->engines, Card::Octave, in.data(), out.data(), kN, p, path) && !path.adjacent && out[0] != 7.0f,
        "stage: a second pitch card in the same pass runs its own engine");
  PitchPath octaveOnly;
  check(runStage(rig->engines, Card::Octave, in.data(), out.data(), kN, p, octaveOnly) && !octaveOnly.autotuneAhead,
        "stage: Octave alone runs, and hands nothing on with no Autotune ahead");
  check(runStage(rig->engines, Card::Formant, in.data(), out.data(), kN, p, octaveOnly),
        "stage: Formant runs its own engine");
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

// Autotune and Octave in one layout each run their own engine on the block the slot ahead hands them: the
// chain's output is Autotune's engine run by hand, then Octave's on its output.
void checkPitchCardsInSeries() {
  const ChainSnapshot snap = snapshotFor(layoutOf({Card::Autotune, Card::Octave}), 0.5f, false);
  auto chain = newRig();
  auto direct = newRig();
  bool same = true;
  bool moved = false;
  for (int b = 0; b < 200; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*chain, snap, in, StillClock());
    Block tuned{};
    Block want{};
    direct->engines.autotune.process(in.data(), tuned.data(), kN, snap.params.pitchFx.autotune);
    direct->engines.octave.process(tuned.data(), want.data(), kN, snap.params.pitchFx.octave);
    same = same && run.out == want;
    moved = moved || maxAbsDiff(run.out, in) > 0.01f;
  }
  check(same && moved, "chain: Autotune and Octave each run their own engine, Octave on Autotune's output");
}

// A Growl card keeps a ring of its own input. While its slot sits settled off, each block clears a slice of
// it (1 KB at most), so the ring is clear 16 blocks after the fade-out ends and the reset at the next
// switch-on has nothing left to clear.
void checkGrowlRingRests() {
  constexpr int kSlot = 5;
  ChainSnapshot snap = snapshotFor(layoutOf({Card::Empty, Card::Empty, Card::Empty, Card::Empty, Card::Empty, Card::Growl}), 0.5f, false);
  auto rig = newRig();
  rig->fades.settle(snap.slotOn);
  int b = 0;
  for (; b < 300; ++b) runBlock(*rig, snap, tone(b, 0.3f), StillClock());
  const bool dirty = !rig->engines.growl.ringIsZero();
  snap.slotOn[kSlot] = false;
  int worstStep = 0;
  for (int k = 0; k < kSlotFadeBlocks + 1; ++k, ++b) runBlock(*rig, snap, tone(b, 0.3f), StillClock());
  int cleared = rig->engines.growl.ringClearedTo();
  for (int k = 0; k < 20; ++k, ++b) {
    runBlock(*rig, snap, tone(b, 0.3f), StillClock());
    worstStep = std::max(worstStep, rig->engines.growl.ringClearedTo() - cleared);
    cleared = rig->engines.growl.ringClearedTo();
  }
  check(dirty && rig->engines.growl.ringIsZero() && cleared == cv::growl_detail::kRingLen && worstStep <= cv::growl_detail::kRestClearFloats,
        "chain: a stopped Growl card clears its ring a slice a block, 1 KB at most");
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

// The clock reads twice around the front half of VOX EQ, twice around each slot, then once before
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
        "chain: VOX EQ is timed by the first pair of reads and the last pair");
}

// The front half of VOX EQ runs ahead of slot 1 in the caller's buffer. With the slots empty and
// POLISH COMP off, the chain's output is the front half then the back half, and a 40 Hz tone loses
// level to the 70 Hz high-pass while the same tone with VOX EQ OFF passes untouched.
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
  eqOnly.voxEq = VoxEq::Cuts;
  Settings noCuts = eqOnly;
  noCuts.voxEq = VoxEq::On;
  const ChainSnapshot on = composeSnapshot(layoutOf({}), panel, eqOnly, BenchOverrides());
  const ChainSnapshot onNoCuts = composeSnapshot(layoutOf({}), panel, noCuts, BenchOverrides());
  const ChainSnapshot off = snapshotFor(layoutOf({}), 0.5f, false);
  auto chain = newRig();
  auto uncut = newRig();
  auto bare = newRig();
  auto plain = newRig();
  bool same = true;
  bool passes = true;
  double outRms = 0.0;
  double uncutRms = 0.0;
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
    uncutRms = rms(runBlock(*uncut, onNoCuts, in, StillClock()).out);
    inRms = rms(in);
  }
  check(same, "chain: the front half runs ahead of slot 1 and the back half after the comp, in the caller's buffer");
  check(passes, "chain: with VOX EQ OFF both halves pass the block bit for bit");
  check(outRms < 0.2 * inRms, "chain: at ON+CUTS the 70 Hz high-pass takes a 40 Hz tone down by more than 14 dB");
  check(uncutRms > 0.7 * inRms && uncutRms > 3.0 * outRms,
        "chain: at ON the 40 Hz tone loses only the subsonic high-pass's 3 dB, and stands well above its ON+CUTS level");
}

// Runs the shipped layout with every toggle on, the knobs at mid travel and VOX EQ at ON+CUTS.
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

  // sinTurns holds 1e-6 absolute, which is all the flat ends of each gain can be trusted to. Inside a
  // block the gains follow the chord between the block's end points, which sits up to 2.2e-4 under the
  // sine and takes the sum of squares up to 4.4e-4 under 1.
  constexpr float kSinTolerance = 1e-6f;
  constexpr float kChordTolerance = 2.3e-4f;
  const Gains g = gainsOver(0, true, steps);
  const double phaseStep = static_cast<double>(kBypassFull / static_cast<uint32_t>(fadeSamples));
  bool equalPower = true;
  bool onSine = true;
  bool monotone = true;
  for (size_t i = 0; i < g.chain.size(); ++i) {
    const double at = std::fmin(static_cast<double>(i + 1) * phaseStep, static_cast<double>(kBypassFull));
    const double theta = std::acos(-1.0) / 2.0 * at / static_cast<double>(kBypassFull);
    equalPower = equalPower && std::fabs(g.chain[i] * g.chain[i] + g.dry[i] * g.dry[i] - 1.0f) < 4.6e-4f;
    onSine = onSine && std::fabs(static_cast<double>(g.chain[i]) - std::cos(theta)) < static_cast<double>(kChordTolerance) &&
             std::fabs(static_cast<double>(g.dry[i]) - std::sin(theta)) < static_cast<double>(kChordTolerance);
    if (i == 0) continue;
    monotone = monotone && g.chain[i] <= g.chain[i - 1] + kSinTolerance && g.dry[i] >= g.dry[i - 1] - kSinTolerance;
  }
  check(equalPower, "bypass: the two gains keep equal power all the way through");
  check(onSine, "bypass: each gain stays within 2.3e-4 of the sine law");
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
  settings.voxEq = VoxEq::Off;
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
// engine gain rising and the dry gain falling in a straight line, so the two sum to 1 at every sample
// and meet at 0.5 in the middle. A card whose output equals its input then holds its level.
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
  float worstSum = 0.0f;
  bool monotone = true;
  for (size_t i = 0; i < ge.size(); ++i) {
    worstSum = std::max(worstSum, std::fabs(ge[i] + gd[i] - 1.0f));
    if (i > 0) monotone = monotone && ge[i] >= ge[i - 1] - 1e-6f && gd[i] <= gd[i - 1] + 1e-6f;
  }
  check(worstSum < 2e-7f, "slot fade: the two gains sum to unity gain at every sample");
  check(monotone, "slot fade: the engine gain only rises and the dry gain only falls");

  // The midpoint is the last sample of block 3, 192 samples into the 384.
  const SlotGains mid = slotGains(2, 3);
  check(std::fabs(mid.engine.back() - 0.5f) < 1e-6f && std::fabs(mid.dry.back() - 0.5f) < 1e-6f,
        "slot fade: at the midpoint both gains read 0.5");

  // The S curve is steepest at its middle, 1.5 times the straight line's 1/384 a sample, and flat at
  // both ends.
  const float step = 1.0f / static_cast<float>(ge.size());
  const float steepest = 1.5f * step;
  float worstStep = 0.0f;
  for (size_t i = 1; i < ge.size(); ++i) worstStep = std::max(worstStep, std::fabs(ge[i] - ge[i - 1]));
  const float firstStep = ge[1] - ge[0];
  const float lastStep = ge[ge.size() - 1] - ge[ge.size() - 2];
  check(worstStep < 1.01f * steepest + 2e-7f && step > 0.0026f && step < 0.0027f,
        "slot fade: no gain moves more than 1.5 / 384 a sample, at the middle");
  check(firstStep < 0.05f * step && lastStep < 0.05f * step,
        "slot fade: the gains leave and reach their ends flat");
  check(ge.front() < 2.0f * step && ge.back() == 1.0f && gd.back() == 0.0f && gd.front() > 0.9970f,
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
  check(std::fabs(up.engine.front() - down.engine.back()) < 1.01f * steepest + 2e-6f &&
            std::fabs(up.dry.front() - down.dry.back()) < 1.01f * steepest + 2e-6f,
        "slot fade: turning a fade round mid-way joins the gains without a step");
}

// A card whose output equals its input (an open gate, Unison at dry unity, Lo-fi's first blocks) crosses
// the fade without a swell or a dip: the level of every block, fading in and out, stays within 0.1 dB.
void checkSlotFadeHoldsLevel() {
  float worstDb = 0.0f;
  float worstDiff = 0.0f;
  for (const bool fadeIn : {true, false}) {
    for (int k = 0; k < kSlotFadeBlocks; ++k) {
      const int from = fadeIn ? k : kSlotFadeBlocks - k;
      const int to = fadeIn ? k + 1 : kSlotFadeBlocks - k - 1;
      std::vector<float> dry(kN);
      for (int i = 0; i < kN; ++i) dry[static_cast<size_t>(i)] = 0.5f * std::sin(0.31f * static_cast<float>(k * kN + i));
      std::vector<float> wet = dry;
      mixSlotFade(wet.data(), dry.data(), kN, from, to);
      double in2 = 0.0, out2 = 0.0;
      for (size_t i = 0; i < dry.size(); ++i) {
        in2 += static_cast<double>(dry[i]) * static_cast<double>(dry[i]);
        out2 += static_cast<double>(wet[i]) * static_cast<double>(wet[i]);
        worstDiff = std::max(worstDiff, std::fabs(wet[i] - dry[i]));
      }
      worstDb = std::max(worstDb, static_cast<float>(std::fabs(10.0 * std::log10(out2 / in2))));
    }
  }
  check(worstDb < 0.1f && worstDiff < 1e-6f, "slot fade: a card whose output equals its input holds its level within 0.1 dB across the fade");
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

// The engine of a slot that comes back on starts cleared. An Echo that holds a full delay line,
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

// Unison's output carries its input at unity, and right after the reset it equals its input exactly. A
// flip on then fades two equal signals across the slot fade, which holds the level where an equal-power
// pair would swell it by 3 dB. The wet stays out until the line holds live audio, so the chain's output
// equals the chain with Unison off through the fade and for the hold after it. A flip off keeps each
// block's level between the off and on levels.
void checkUnisonToggle() {
  const Layout layout = layoutOf({Card::Unison});
  const ChainSnapshot on = snapshotToggles(layout, 0.5f, allToggles(true));
  const ChainSnapshot off = snapshotToggles(layout, 0.5f, allToggles(false));
  auto flipped = newRig();
  auto steadyOff = newRig();
  constexpr int kWarm = 80;
  constexpr int kOnBlocks = 60;
  constexpr int kHoldBlocks = 20;  // 1,280 samples, inside the 1,347-sample hold
  float worstOnDiff = 0.0f;
  bool wetAfterHold = false;
  for (int b = 0; b < kWarm + kOnBlocks; ++b) {
    const bool isOn = b >= kWarm;
    const Block in = tone(b, 0.3f);
    const Run a = runBlock(*flipped, isOn ? on : off, in, StillClock());
    const Run c = runBlock(*steadyOff, off, in, StillClock());
    if (b >= kWarm && b < kWarm + kHoldBlocks) worstOnDiff = std::max(worstOnDiff, maxAbsDiff(a.out, c.out));
    if (b >= kWarm + kHoldBlocks + 12) wetAfterHold = wetAfterHold || maxAbsDiff(a.out, c.out) > 1e-3f;
  }
  check(worstOnDiff < 1e-6f, "Unison toggle: through the fade in and the hold after it, the output equals the chain with Unison off");
  check(wetAfterHold, "Unison toggle: the wet arrives after the hold");

  auto down = newRig();
  auto lo = newRig();
  auto hi = newRig();
  double worstRatio = 0.0;
  for (int b = 0; b < kWarm + 2 * kSlotFadeBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    const Run a = runBlock(*down, b < kWarm ? on : off, in, StillClock());
    const Run l = runBlock(*lo, off, in, StillClock());
    const Run h = runBlock(*hi, on, in, StillClock());
    if (b >= kWarm) worstRatio = std::max(worstRatio, rms(a.out) / std::max(rms(l.out), rms(h.out)));
  }
  check(worstRatio < 1.03, "Unison toggle: a flip off keeps each block's level at or under the larger of the off and on levels");
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
// floor_dbfs and keys the Chatter card. A Gate card stops with its slot. Either way the dry input passes
// bit for bit.
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

// The Input gate switched off in a pause, with the room floor 9 dB under its threshold and the gate
// shut, brings the floor up to the dry level inside the slot fade and holds it there. The gate's own off
// fade ends with the slot fade, so the floor does not cut again and swell back after the slot passes the
// engine's output alone.
void checkInputGateOffInPause() {
  const Layout layout = layoutOf({Card::InputGate});
  const ChainSnapshot on = snapshotToggles(layout, 0.5f, allToggles(true));
  const ChainSnapshot off = snapshotToggles(layout, 0.5f, allToggles(false));
  constexpr float kFloorAmp = 0.0006f;  // -64 dBFS against the default threshold of -55 dBFS
  auto rig = newRig();
  for (int b = 0; b < 300; ++b) runBlock(*rig, on, tone(b, kFloorAmp), StillClock());
  const bool shut = rig->engines.inputGate.gainDb() < -60.0f;
  std::vector<double> gainDb;
  for (int b = 300; b < 330; ++b) {
    const Block in = tone(b, kFloorAmp);
    const Run run = runBlock(*rig, off, in, StillClock());
    gainDb.push_back(10.0 * std::log10(rms(run.out) * rms(run.out) / (rms(in) * rms(in))));
  }
  bool climbs = true;
  for (int b = 1; b < kSlotFadeBlocks; ++b) climbs = climbs && gainDb[static_cast<size_t>(b)] >= gainDb[static_cast<size_t>(b - 1)] - 0.05;
  double worstAfter = 0.0;
  double highest = -200.0;
  for (size_t b = 0; b < gainDb.size(); ++b) {
    highest = std::max(highest, gainDb[b]);
    if (b >= static_cast<size_t>(kSlotFadeBlocks)) worstAfter = std::max(worstAfter, std::fabs(gainDb[b]));
  }
  check(shut && climbs && worstAfter < 1.0 && highest < 0.1,
        "slot flip: the Input gate switched off in a pause brings the floor up inside the fade and holds it within 1 dB");
}

// Each pitch card fades its own slot. With Autotune off and Octave on, Autotune's slot sits on the dry
// input and Octave's runs its engine. When Octave's toggle goes off its slot runs the 6 fade blocks and
// then passes the dry input bit for bit, with Autotune's slot untouched.
void checkPitchCardToggles() {
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
  check(running, "slot flip: with Autotune off and Octave on, Octave's engine runs in its own slot");
  check(rig->fades.level(0) == 0 && rig->fades.level(1) == kSlotFadeBlocks,
        "slot flip: Octave's toggle fades Octave's slot only, and Autotune's slot sits on the dry input");

  int engineBlocks = 0;
  bool dryAfter = true;
  for (int b = 60; b < 60 + 14; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*rig, snapOff, in, StillClock());
    if (b < 60 + kSlotFadeBlocks) engineBlocks += run.out != in ? 1 : 0;
    if (b >= 60 + kSlotFadeBlocks) dryAfter = dryAfter && run.out == in;
  }
  check(engineBlocks == kSlotFadeBlocks && dryAfter,
        "slot flip: with Octave's toggle off its slot runs for the 6 fade blocks, then passes the dry input bit for bit");
}

// A toggle fades only its own card's slot: Autotune, Octave and Formant side by side, with each toggle flipped
// in turn, move the level of the one slot and leave the other two where they were.
void checkPitchTogglesFadeOwnSlot() {
  const Layout layout = layoutOf({Card::Autotune, Card::Octave, Card::Formant});
  auto rig = newRig();
  Toggles t = allToggles(false);
  rig->fades.settle(t);
  int b = 0;
  const auto play = [&](int blocks) {
    const ChainSnapshot snap = snapshotToggles(layout, 0.5f, t);
    for (int k = 0; k < blocks; ++k, ++b) runBlock(*rig, snap, tone(b, 0.3f), StillClock());
  };
  bool own = true;
  for (int flip = 0; flip < 3; ++flip) {
    t[static_cast<size_t>(flip)] = true;
    play(kSlotFadeBlocks + 2);
    for (int s = 0; s < 3; ++s) own = own && rig->fades.level(s) == (s <= flip ? kSlotFadeBlocks : 0);
  }
  for (int flip = 0; flip < 3; ++flip) {
    t[static_cast<size_t>(flip)] = false;
    play(kSlotFadeBlocks + 2);
    for (int s = 0; s < 3; ++s) own = own && rig->fades.level(s) == (s > flip ? kSlotFadeBlocks : 0);
  }
  check(own, "slot flip: each pitch card's toggle fades its own slot and leaves the other pitch slots settled");
}

// 200 ms of the tone: the tracker finds its period in about 32 ms and the voices sound at least
// 22 ms after that.
constexpr int kCrowdBlocks = 150;

// The Crowd card on `rig`'s own engines, by hand. With an Autotune slot ahead, the Autotune slot writes the
// card's ring, Autotune's engine runs with its tracker kept alive for the card, and the card reads that
// tracker and Autotune's correction. With none ahead the card writes its own ring and runs its own tracker.
Block crowdByHand(Rig& rig, const ChainSnapshot& snap, const Block& in, bool autotuneAhead, bool autotuneBehind = false) {
  Block mid = in;
  Block out{};
  cv::CrowdPitch pitch;
  if (autotuneAhead) {
    cv::TrackerNeeds need;
    need.run = true;
    need.outOfRange = true;
    rig.engines.crowd.write(in.data(), kN);
    rig.engines.autotune.process(in.data(), mid.data(), kN, snap.params.pitchFx.autotune, need);
    pitch = {&rig.engines.autotune.pitch(), rig.engines.autotune.correctionSemis()};
    if (!snap.params.pitchFx.autotune.on) mid = in;  // the Autotune slot passes the dry input while its toggle is off
  }
  rig.engines.crowd.process(mid.data(), out.data(), kN, snap.params.crowd, pitch);
  if (autotuneBehind) {
    Block tuned{};
    rig.engines.autotune.process(out.data(), tuned.data(), kN, snap.params.pitchFx.autotune);
    return tuned;
  }
  return out;
}

struct CrowdOutcome {
  bool sameAsHand = true;  // every block equals the Crowd card run by hand
  bool heard = false;      // the second half of the run differs from the input
  bool live = false;       // a voice sounds
};

// Every knob at 0.5 except the Crowd card's VOICES, which stands at step 4 so its copies sound. At 0.5 a
// stepped knob lands on its first position, where the Crowd card plays no copy.
ChainSnapshot snapshotCrowd(const Layout& layout, const Toggles& on) {
  ChainSnapshot snap = snapshotToggles(layout, 0.5f, on);
  snap.params.crowd.voicesStep = 4;
  return snap;
}

CrowdOutcome runCrowdLayout(const Layout& layout, const Toggles& on, bool autotuneAhead, bool autotuneBehind = false) {
  const ChainSnapshot snap = snapshotCrowd(layout, on);
  auto chain = newRig();
  auto hand = newRig();
  chain->fades.settle(snap.slotOn);
  hand->fades.settle(snap.slotOn);
  CrowdOutcome o;
  for (int b = 0; b < kCrowdBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*chain, snap, in, StillClock());
    o.sameAsHand = o.sameAsHand && run.out == crowdByHand(*hand, snap, in, autotuneAhead, autotuneBehind);
    if (b >= kCrowdBlocks / 2) o.heard = o.heard || run.out != in;
  }
  o.live = chain->engines.crowd.activeVoices() > 0;
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
  PitchPath none;
  check(runStage(rig->engines, Card::Crowd, in.data(), out.data(), kN, p, none) && out == in &&
            rig->engines.crowd.ringForTest().head() == kN,
        "stage: a Crowd card that is off with no Autotune ahead writes its own ring and passes the block untouched");

  // Behind an Autotune slot that fed no tracker, the Crowd card has no period. It holds its lead at 1 and
  // its copies at 0, so every block comes out as it went in.
  auto behind = newRig();
  p.crowd.on = true;
  PitchPath ahead;
  ahead.autotuneAhead = true;
  ahead.upstream.result = &behind->engines.autotune.pitch();
  bool wrote = true;
  bool unity = true;
  for (int b = 0; b < 20; ++b) {
    wrote = wrote && runStage(behind->engines, Card::Crowd, tone(b, 0.3f).data(), out.data(), kN, p, ahead);
    unity = unity && out == tone(b, 0.3f);
  }
  check(wrote && unity && behind->engines.autotune.pitch().period == 0.0f && behind->engines.crowd.leadGain() == 1.0f,
        "stage: a Crowd card behind an Autotune slot with no tracker runs only its own engine");
}

// Mob is exempt from the chain order. With an Autotune slot ahead of it, wherever the other cards sit, the
// Autotune slot writes Mob's ring from Autotune's input and keeps its tracker alive for Mob even with its
// own toggle off, and Mob reads that tracker and Autotune's correction. With none ahead, Mob writes its own
// ring and runs its own tracker, whatever sits behind it.
void checkCrowdPitchStage() {
  const CrowdOutcome alone = runCrowdLayout(layoutOf({Card::Crowd}), togglesOn({0}), false);
  check(alone.sameAsHand && alone.heard && alone.live, "crowd: alone in a layout it gets a live ring and tracker of its own");

  const CrowdOutcome ahead = runCrowdLayout(layoutOf({Card::Crowd, Card::Autotune}), togglesOn({0}), false);
  check(ahead.sameAsHand && ahead.heard && ahead.live,
        "crowd: ahead of Autotune it runs its own ring and tracker, whatever Autotune does");

  const CrowdOutcome aheadBoth = runCrowdLayout(layoutOf({Card::Crowd, Card::Autotune}), togglesOn({0, 1}), false, true);
  check(aheadBoth.sameAsHand && aheadBoth.heard && aheadBoth.live,
        "crowd: ahead of Autotune with both on, Autotune plays no part in Mob's ring or tracker");

  const CrowdOutcome behind =
      runCrowdLayout(layoutOf({Card::Autotune, Card::Octave, Card::Crowd}), togglesOn({2}), true);
  check(behind.sameAsHand && behind.heard && behind.live,
        "crowd: behind Autotune and Octave with only Mob on, Autotune's slot feeds its ring and tracker");

  const CrowdOutcome all = runCrowdLayout(layoutOf({Card::Autotune, Card::Crowd}), togglesOn({0, 1}), true);
  check(all.sameAsHand && all.heard && all.live, "crowd: behind Autotune with both on, it reads Autotune's tracker and correction");

  auto rig = newRig();
  const ChainSnapshot snap = snapshotCrowd(layoutOf({Card::Autotune, Card::Octave, Card::Crowd}), togglesOn({2}));
  rig->fades.settle(snap.slotOn);
  for (int b = 0; b < kCrowdBlocks; ++b) runBlock(*rig, snap, tone(b, 0.3f), StillClock());
  check(rig->fades.level(0) == 0 && rig->engines.autotune.pitch().period > 0.0f,
        "crowd: Autotune's slot stays on the dry input while its tracker runs for Mob");
}

// The Crowd's toggle off fades its slot out. After the fade the dry input passes bit for bit.
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
constexpr int kCardRunBlocks = 150;

// A layout with `c` in its slot, slot 3 for Formant and the Formant follower and slot 7 for the others,
// and every other slot empty.
Layout soloLayout(Card c) {
  Layout layout = layoutOf({});
  layout.card[c == Card::Formant || c == Card::FormantFollower ? kSlot3 : kSlot7] = c;
  return layout;
}

// The engine of the card called by hand on `rig`'s own engines with the snapshot's params.
Block soloEngine(Rig& rig, Card c, const ChainSnapshot& snap, const Block& in) {
  Block out{};
  switch (c) {
    case Card::Formant:
      rig.engines.formant.process(in.data(), out.data(), kN, snap.params.pitchFx.formant, cv::UpstreamPitch{});
      break;
    case Card::FormantFollower:
      rig.engines.formant.process(
          in.data(), out.data(), kN,
          rig.engines.follower.follow(in.data(), kN, snap.params.pitchFx.formant, snap.params.follower),
          cv::UpstreamPitch{});
      break;
    case Card::Lofi: rig.engines.lofi.process(in.data(), out.data(), kN, snap.params.lofi); break;
    case Card::Bloom: rig.engines.bloom.process(in.data(), out.data(), kN, snap.params.bloom); break;
    default: break;
  }
  return out;
}

// Formant and the Formant follower in slot 3 and Lo-fi and Chatter in slot 7 each run their engine, change
// the tone, and pass the dry input bit for bit once their toggle has been off for the fade. A reset clears
// the Lo-fi and Chatter engines.
void checkAppendedCardsRun() {
  for (const Card c : {Card::Formant, Card::FormantFollower, Card::Lofi, Card::Bloom}) {
    const std::string name = cardDesc(c).name;
    const ChainSnapshot snap = snapshotToggles(soloLayout(c), 0.5f, allToggles(true));
    auto chain = newRig();
    auto hand = newRig();
    bool same = true;
    bool finite = true;
    bool heard = false;
    for (int b = 0; b < kCardRunBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      same = same && run.out == soloEngine(*hand, c, snap, in);
      finite = finite && run.finite && allFinite(run.out);
      if (b >= kCardRunBlocks / 2) heard = heard || run.out != in;
    }
    check(same, (name + " runs its engine over the block in its slot").c_str());
    check(finite && heard, (name + " stays finite and changes the tone in its slot").c_str());

    const ChainSnapshot off = snapshotToggles(soloLayout(c), 0.5f, allToggles(false));
    auto settled = newRig();
    bool dry = true;
    for (int b = 0; b < 3 * kSlotFadeBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*settled, off, in, StillClock());
      if (b >= kSlotFadeBlocks) dry = dry && run.out == in;
    }
    check(dry, (name + " with its toggle off passes the dry input bit for bit after the fade").c_str());
  }

  const ChainSnapshot bloom = snapshotToggles(soloLayout(Card::Bloom), 0.5f, allToggles(true));
  auto rig = newRig();
  for (int b = 0; b < kCardRunBlocks; ++b) runBlock(*rig, bloom, tone(b, 0.3f), StillClock());
  check(rig->engines.bloom.inWord() && rig->engines.bloom.looping(),
        "Chatter with no Input gate placed keys on its own input, so a word opens and its slice loops");
  resetEngine(rig->engines, Engine::Bloom);
  check(!rig->engines.bloom.inWord() && !rig->engines.bloom.looping() && rig->engines.bloom.wetGain() == 0.0f,
        "resetEngine clears the Chatter card's word and its loop");

  const ChainSnapshot follower = snapshotToggles(soloLayout(Card::FormantFollower), 0.5f, allToggles(true));
  auto followerRig = newRig();
  for (int b = 0; b < 40; ++b) runBlock(*followerRig, follower, tone(b, 0.3f), StillClock());
  const bool driven = followerRig->engines.follower.drive() > 0.5f;
  resetEngine(followerRig->engines, Engine::Formant);
  check(driven && followerRig->engines.follower.drive() == 0.0f,
        "the Apraxia follower's envelope follows the tone, and resetEngine on the Formant engine clears it");

  const ChainSnapshot lofi = snapshotToggles(soloLayout(Card::Lofi), 0.5f, allToggles(true));
  auto lofiRig = newRig();
  for (int b = 0; b < 40; ++b) runBlock(*lofiRig, lofi, tone(b, 0.3f), StillClock());
  resetEngine(lofiRig->engines, Engine::Lofi);
  check(allZero(runBlock(*lofiRig, lofi, Block{}, StillClock()).out), "resetEngine clears the Lo-fi engine, so silence in is silence out");
}

// The slots of `snap` run by hand on `rig`'s own engines, each card's engine called on the block the slot
// ahead hands it. The pitch path is kept as spec section 2 lays it out. It covers Autotune (with a Formant
// directly behind it asking for its tracker), Octave, Formant, Mob with no Autotune ahead, and Distortion.
Block walkSlotsByHand(Rig& rig, const ChainSnapshot& snap, const Block& in) {
  Block cur = in;
  bool autotuneAhead = false;
  bool adjacent = false;
  cv::UpstreamPitch up;
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = snap.layout.card[static_cast<size_t>(s)];
    if (c == Card::Empty) continue;
    Block next{};
    switch (c) {
      case Card::Autotune: {
        cv::TrackerNeeds need;
        cv::GridMarks grid;
        for (int t = s + 1; t < kSlotCount; ++t) {
          const Card behind = snap.layout.card[static_cast<size_t>(t)];
          if (behind == Card::Empty) continue;
          if (behind == Card::Formant) {
            need = {true, true, true};
            grid = rig.engines.formant.gridMarks();
            rig.engines.formant.feed(cur.data(), kN);
          }
          break;
        }
        rig.engines.autotune.process(cur.data(), next.data(), kN, snap.params.pitchFx.autotune, need, grid);
        autotuneAhead = true;
        adjacent = true;
        up = {&rig.engines.autotune.pitch(), rig.engines.autotune.correctionSemis(), rig.engines.autotune.outputLatency(),
              &rig.engines.autotune, 1.0f, 1.0f};
        break;
      }
      case Card::Octave:
        rig.engines.octave.process(cur.data(), next.data(), kN, snap.params.pitchFx.octave);
        adjacent = false;
        if (autotuneAhead) {
          if (rig.engines.octave.leads()) up.shiftSemis += rig.engines.octave.intervalSemis();
          up.latency += rig.engines.octave.outputLatency();
        }
        break;
      case Card::Formant:
        rig.engines.formant.process(cur.data(), next.data(), kN, snap.params.pitchFx.formant,
                                    autotuneAhead && adjacent ? up : cv::UpstreamPitch{});
        adjacent = false;
        if (autotuneAhead) up.latency += rig.engines.formant.outputLatency();
        break;
      case Card::Crowd: rig.engines.crowd.process(cur.data(), next.data(), kN, snap.params.crowd, cv::CrowdPitch{}); break;
      case Card::Distortion:
        rig.engines.distortion.process(cur.data(), next.data(), kN, snap.params.distortion);
        adjacent = false;
        break;
      default: break;
    }
    cur = next;
  }
  return cur;
}

// Each card's slot output equals its engine run by hand on that slot's input. The layouts are the audit's
// 4a (Autotune 2, Distortion 3, Octave 4), 4b (Mob 2, Octave 3), 4c (Octave 2, Autotune 3) and 4d (Autotune 2,
// Formant 3, Octave 4), where Formant sits directly behind Autotune and reads its tracker.
void checkSlotsEqualEngines() {
  struct Case {
    const char* name;
    Layout layout;
  };
  const Case cases[] = {
      {"4a Autotune, Distortion, Octave", layoutOf({Card::Empty, Card::Autotune, Card::Distortion, Card::Octave})},
      {"4b Mob, Octave", layoutOf({Card::Empty, Card::Crowd, Card::Octave})},
      {"4c Octave, Autotune", layoutOf({Card::Empty, Card::Octave, Card::Autotune})},
      {"4d Autotune, Formant, Octave", layoutOf({Card::Empty, Card::Autotune, Card::Formant, Card::Octave})},
  };
  for (const Case& k : cases) {
    const ChainSnapshot snap = snapshotToggles(k.layout, 0.5f, allToggles(true));
    auto chain = newRig();
    auto hand = newRig();
    chain->fades.settle(snap.slotOn);
    hand->fades.settle(snap.slotOn);
    bool same = true;
    bool heard = false;
    for (int b = 0; b < kCardRunBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      same = same && run.out == walkSlotsByHand(*hand, snap, in);
      if (b >= kCardRunBlocks / 2) heard = heard || run.out != in;
    }
    check(same && heard, (std::string("slots: ") + k.name + ": every slot equals its engine run by hand on its input").c_str());
  }
}

// Formant joins Autotune's grain pass when it sits directly behind it: it reads Autotune's tracker, so its own
// tracker stays idle, and cuts its grains from Autotune's input. Anywhere else (a card between, Autotune behind
// it, no Autotune) it runs a tracker and a ring of its own, and the chain still equals the engines run by hand.
void checkFormantReusesTracker() {
  struct Case {
    const char* name;
    Layout layout;
    bool reuses;
  };
  const Case cases[] = {
      {"Formant directly behind Autotune", layoutOf({Card::Autotune, Card::Formant}), true},
      {"Formant behind Autotune with an Empty slot between", layoutOf({Card::Autotune, Card::Empty, Card::Formant}), true},
      {"Formant behind Autotune and Octave", layoutOf({Card::Autotune, Card::Octave, Card::Formant}), false},
      {"Formant ahead of Autotune", layoutOf({Card::Formant, Card::Autotune}), false},
      {"Formant alone", layoutOf({Card::Formant}), false},
  };
  for (const Case& k : cases) {
    const ChainSnapshot snap = snapshotToggles(k.layout, 0.5f, allToggles(true));
    auto chain = newRig();
    auto hand = newRig();
    chain->fades.settle(snap.slotOn);
    hand->fades.settle(snap.slotOn);
    bool same = true;
    for (int b = 0; b < kCardRunBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      same = same && runBlock(*chain, snap, in, StillClock()).out == walkSlotsByHand(*hand, snap, in);
    }
    const bool ownRuns = chain->engines.formant.ownPitch().period > 0.0f;
    check(same, (std::string(k.name) + ": the chain equals the engines run by hand").c_str());
    check(ownRuns == !k.reuses,
          (std::string(k.name) + (k.reuses ? ": Formant's own tracker stays idle" : ": Formant runs its own tracker")).c_str());
    check(chain->engines.formant.groupedForTest() == k.reuses,
          (std::string(k.name) + (k.reuses ? ": Formant cuts from Autotune's input" : ": Formant cuts from its own ring")).c_str());
  }
}

// Apraxia runs the Formant engine, so directly behind Autotune it joins the pass too: its sweep sets the rate
// of the render that cuts from Autotune's input, and its own tracker stays idle.
void checkApraxiaJoinsAutotunePass() {
  const ChainSnapshot snap = snapshotFor(layoutOf({Card::Autotune, Card::FormantFollower}), 0.5f, false);
  auto chain = newRig();
  chain->fades.settle(snap.slotOn);
  bool heard = false;
  for (int b = 0; b < kCardRunBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*chain, snap, in, StillClock());
    if (b >= kCardRunBlocks / 2) heard = heard || run.out != in;
  }
  check(heard && chain->engines.formant.groupedForTest() && chain->engines.formant.ownPitch().period == 0.0f,
        "Apraxia directly behind Autotune cuts from Autotune's input and runs no tracker of its own");
}

// A 220 Hz voice of ten harmonics at 1/k that continues across blocks, peaking near `amp`. A sine would do
// for a card that passes it, but the Formant card moves its one harmonic and the mix cancels it.
Block harmonics(int block, float amp) {
  constexpr double kPi = 3.14159265358979323846;
  Block x{};
  for (int i = 0; i < kN; ++i) {
    const double phase = 2.0 * kPi * 220.0 * static_cast<double>(block * kN + i) / cv::kSampleRate;
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    x[static_cast<size_t>(i)] = amp * static_cast<float>(s / 1.9);
  }
  return x;
}

// The Autotune slot flipped on and off under a Formant card directly behind it, through the slot's fade and
// Autotune's own crossfade. The card plays the share of Autotune's input and the share of its corrected voice
// at one delay, so for 300 ms after each flip the output's largest step stays at the settled outputs'.
void checkAutotuneToggleUnderFormant() {
  const Layout layout = layoutOf({Card::Autotune, Card::Formant});
  const ChainSnapshot off = snapshotToggles(layout, 0.5f, togglesOn({1}));
  const ChainSnapshot on = snapshotToggles(layout, 0.5f, togglesOn({0, 1}));
  constexpr int kWarm = 300;
  constexpr int kHold = 300;
  constexpr int kWindow = 225;  // 300 ms
  auto toggled = newRig();
  auto settledOff = newRig();
  auto settledOn = newRig();
  toggled->fades.settle(off.slotOn);
  settledOff->fades.settle(off.slotOn);
  settledOn->fades.settle(on.slotOn);
  std::vector<float> t;
  std::vector<float> lo;
  std::vector<float> hi;
  for (int b = 0; b < kWarm + 2 * kHold; ++b) {
    const bool between = b >= kWarm && b < kWarm + kHold;
    const Block in = harmonics(b, 0.3f);
    append(t, runBlock(*toggled, between ? on : off, in, StillClock()).out);
    append(lo, runBlock(*settledOff, off, in, StillClock()).out);
    append(hi, runBlock(*settledOn, on, in, StillClock()).out);
  }
  float worst = 0.0f;
  for (const int edge : {kWarm, kWarm + kHold}) {
    const auto from = static_cast<std::ptrdiff_t>(edge) * kN;
    const auto to = from + static_cast<std::ptrdiff_t>(kWindow) * kN;
    auto windowStep = [&](const std::vector<float>& x) {
      return maxStep(std::vector<float>(x.begin() + from, x.begin() + to));
    };
    worst = std::max(worst, windowStep(t) / std::max(windowStep(lo), windowStep(hi)));
  }
  check(worst < 1.1f, "Autotune slot flipped under Formant: the largest step in 300 ms stays within 10 % of the settled outputs'");
}

// A Formant engine that joins Autotune's grain pass waits for the corrected voice once, so the delay the path
// hands on to the cards behind equals Autotune's own latency. Behind a card in between, or ahead of nothing, it
// cuts from its own ring and adds its whole lag on top.
void checkGroupedFormantLatency() {
  struct Case {
    const char* name;
    Layout layout;
    bool grouped;
  };
  const Case cases[] = {
      {"Formant directly behind Autotune", layoutOf({Card::Autotune, Card::Formant}), true},
      {"Formant behind Autotune with an Empty slot between", layoutOf({Card::Autotune, Card::Empty, Card::Formant}), true},
      {"Formant behind Autotune and Echo", layoutOf({Card::Autotune, Card::Slapback, Card::Formant}), false},
  };
  for (const Case& k : cases) {
    const ChainSnapshot snap = snapshotFor(k.layout, 0.5f, false);
    auto withFormant = newRig();
    auto autotuneOnly = newRig();
    float behind = 0.0f;
    float alone = 0.0f;
    for (int b = 0; b < kCardRunBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      Block mid{};
      Block out{};
      PitchPath soloPath;
      runStage(autotuneOnly->engines, Card::Autotune, in.data(), mid.data(), kN, snap.params, soloPath);
      alone = soloPath.upstream.latency;

      // The chain's walk: Autotune with its reader's needs, then the cards behind it.
      PitchPath path;
      StageContext ctx;
      ctx.tracker = {true, true, true};
      ctx.feedsFormant = k.grouped;
      if (k.grouped) ctx.groupGrid = withFormant->engines.formant.gridMarks();
      runStage(withFormant->engines, Card::Autotune, in.data(), mid.data(), kN, snap.params, path, ctx);
      Block cur = mid;
      for (int s = 0; s < kSlotCount; ++s) {
        const Card c = snap.layout.card[static_cast<size_t>(s)];
        if (c == Card::Autotune || c == Card::Empty) continue;
        runStage(withFormant->engines, c, cur.data(), out.data(), kN, snap.params, path);
        cur = out;
      }
      behind = path.upstream.latency;
    }
    check(alone > 0.0f, (std::string(k.name) + ": Autotune's own latency is the corrected voice's delay").c_str());
    if (k.grouped) {
      check(std::fabs(behind - alone) < 0.5f, (std::string(k.name) + ": the delay handed on equals Autotune's alone").c_str());
    } else {
      check(behind > alone + 1000.0f, (std::string(k.name) + ": the delay handed on adds the Formant engine's own lag").c_str());
    }
  }
}

// Autotune's tracker runs with Autotune's toggle off while a reader behind it is on, and the slot passes the
// dry input bit for bit. Formant directly behind reads that tracker and equals its engine run by hand on it.
void checkAutotuneTrackerForReaders() {
  const Layout layout = layoutOf({Card::Autotune, Card::Formant});
  const ChainSnapshot snap = snapshotToggles(layout, 0.5f, togglesOn({1}));
  auto chain = newRig();
  auto hand = newRig();
  chain->fades.settle(snap.slotOn);
  hand->fades.settle(snap.slotOn);
  bool same = true;
  bool dry = true;
  for (int b = 0; b < kCardRunBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    const Run run = runBlock(*chain, snap, in, StillClock());

    // By hand: Autotune runs for its reader and the slot hands on the dry input, Formant reads the tracker.
    Block tuned{};
    Block want{};
    cv::TrackerNeeds need{true, true, true};
    hand->engines.formant.feed(in.data(), kN);
    hand->engines.autotune.process(in.data(), tuned.data(), kN, snap.params.pitchFx.autotune, need,
                                   hand->engines.formant.gridMarks());
    const cv::UpstreamPitch up{&hand->engines.autotune.pitch(), 0.0f, 0.0f, &hand->engines.autotune, 0.0f, 0.0f};
    hand->engines.formant.process(in.data(), want.data(), kN, snap.params.pitchFx.formant, up);
    same = same && run.out == want;
    dry = dry && chain->fades.level(0) == 0;
  }
  check(same && dry, "autotune: with its toggle off and Formant on behind it, the slot passes the dry input and Formant equals its engine");
  check(chain->engines.autotune.pitch().period > 0.0f && chain->engines.formant.ownPitch().period == 0.0f,
        "autotune: its tracker holds the period for Formant, whose own tracker stays idle");

  // With Formant off too, no reader is left and the Autotune engine rests.
  const ChainSnapshot none = snapshotToggles(layout, 0.5f, togglesOn({}));
  for (int b = 0; b < 3 * kSlotFadeBlocks; ++b) runBlock(*chain, none, tone(300 + b, 0.3f), StillClock());
  const auto& ring = chain->engines.autotune.ringForTest().data();
  check(std::all_of(ring.begin(), ring.end(), [](float v) { return v == 0.0f; }),
        "autotune: with no reader on, a settled-off slot rests its engine and leaves its ring clear");
}

// Apraxia behind a Deepthroat-first layout: Deepthroat has no Autotune ahead, so Apraxia runs its own tracker
// on Deepthroat's output, and its SHIFT sweeps with the follower's envelope of that block.
void checkApraxiaBehindDeepthroat() {
  const Layout layout = layoutOf({Card::Growl, Card::FormantFollower});
  const ChainSnapshot snap = snapshotToggles(layout, 1.0f, allToggles(true));  // RANGE at +12 st
  auto chain = newRig();
  auto hand = newRig();
  chain->fades.settle(snap.slotOn);
  hand->fades.settle(snap.slotOn);
  bool same = true;
  bool finite = true;
  float lowest = 1e9f;
  float highest = -1e9f;
  for (int b = 0; b < 240; ++b) {
    const Block in = tone(b, (b / 40) % 2 == 0 ? 0.3f : 0.0f);
    const Run run = runBlock(*chain, snap, in, StillClock());
    Block grown{};
    Block want{};
    hand->engines.growl.process(in.data(), grown.data(), kN, snap.params.growl, cv::UpstreamPitch{});
    hand->engines.formant.process(
        grown.data(), want.data(), kN,
        hand->engines.follower.follow(grown.data(), kN, snap.params.pitchFx.formant, snap.params.follower),
        cv::UpstreamPitch{});
    same = same && run.out == want;
    finite = finite && run.finite && allFinite(run.out);
    lowest = std::min(lowest, chain->engines.formant.shiftSemis());
    highest = std::max(highest, chain->engines.formant.shiftSemis());
  }
  check(same && finite, "apraxia: behind Deepthroat it equals the engines run by hand and stays finite");
  check(highest - lowest > 0.5f, "apraxia: behind Deepthroat its SHIFT sweeps with the envelope");
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
    for (int b = 0; b < kCardRunBlocks; ++b) {
      const Block in = tone(b, 0.3f);
      const Run run = runBlock(*chain, snap, in, StillClock());
      Block gated{};
      hand->engines.inputGate.process(in.data(), gated.data(), kN, snap.params.inputGate);
      cv::BloomParams keyed = snap.params.bloom;
      keyed.keyFromGate = gatePlaced;
      Block want{};
      hand->engines.bloom.process(in.data(), want.data(), kN, keyed, gatePlaced && hand->engines.inputGate.open());
      o.same = o.same && run.out == want;
      if (b == kCardRunBlocks - 1) {
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
// one equals its slot's input at the cut clock, and how many of their samples differ from the chain's
// own input after the front half of VOX EQ.
struct SourceRun {
  int slices = 0;
  bool matchesSlotInput = true;
  int differs = 0;
  int samples = 0;
};

// Runs `layout` through the chain, and the slots ahead of the Chatter card by hand beside it. The hand
// run on a second rig gives the block entering the Chatter slot.
SourceRun runSource(const Layout& layout, const Toggles& toggles, int kBlocks = 250) {
  const ChainSnapshot snap = snapshotCrowd(layout, toggles);
  int bloomSlot = 0;
  for (int s = 0; s < kSlotCount; ++s) {
    if (layout.card[static_cast<size_t>(s)] == Card::Bloom) bloomSlot = s;
  }
  auto chain = newRig();
  auto hand = newRig();
  chain->fades.settle(snap.slotOn);
  hand->fades.settle(snap.slotOn);
  std::vector<float> entering;
  std::vector<float> head;
  for (int b = 0; b < kBlocks; ++b) {
    const Block in = tone(b, 0.3f);
    runBlock(*chain, snap, in, StillClock());
    Block a{};
    Block scratch{};
    hand->engines.polish.processFront(in.data(), a.data(), kN, snap.params.eq);
    append(head, a);
    PitchPath path;
    for (int s = 0; s < bloomSlot; ++s) {
      if (runSlot(hand->engines, hand->fades, s, snap, a.data(), scratch.data(), kN, path)) std::swap(a, scratch);
    }
    append(entering, a);
  }
  SourceRun r;
  const cv::Bloom& bloom = chain->engines.bloom;
  for (int slot = 0; slot < cv::Bloom::kSlices; ++slot) {
    if (!bloom.sliceValid(slot)) continue;
    ++r.slices;
    const size_t cut = bloom.sliceCutAt(slot);

    // A slice is the slot's input through the card's 11 kHz low-pass, started clear at its cut. Its
    // first and last 300 samples carry the pass window (278 samples) and are left out.
    constexpr int kEdge = 300;
    cv::Biquad lp;
    lp.setLowPass(cv::Bloom::kLowpassHz, cv::Bloom::kLowpassQ);
    for (int j = 0; j < bloom.sliceLen(slot); ++j) {
      const float want = lp.run(entering[cut + static_cast<size_t>(j)]);
      if (j < kEdge || j >= bloom.sliceLen(slot) - kEdge) continue;
      const float s = bloom.sliceData(slot)[j];
      if (std::fabs(s - want) > 1e-5f) r.matchesSlotInput = false;
      if (std::fabs(s - head[cut + static_cast<size_t>(j)]) > 1e-3f) ++r.differs;
      ++r.samples;
    }
  }
  return r;
}

// The Chatter card cuts its slices from its own input, the block leaving the slot ahead of it, so the
// slices carry every card between the voice and the slot. With Distortion in slot 2 the slices equal the
// distorted block and differ from the chain input; with Mob after Octave they equal Mob's output; with
// Mob ahead of Octave they equal the block leaving slot 6; with the card in slot 1 they equal the
// chain input.
void checkBloomSource() {
  Layout dist = layoutOf({Card::Empty, Card::Distortion});
  dist.card[kSlot7] = Card::Bloom;
  const SourceRun d = runSource(dist, togglesOn({1, kSlot7}));
  check(d.slices >= 2 && d.matchesSlotInput && d.samples > 0 && d.differs > d.samples / 2,
        "chatter source: with Distortion ahead of it the slices equal the distorted block, not the chain input");

  Layout behind = layoutOf({Card::InputGate, Card::Autotune, Card::Octave, Card::Crowd});
  behind.card[kSlot7] = Card::Bloom;
  const SourceRun a = runSource(behind, togglesOn({1, 2, 3, kSlot7}));
  check(a.slices >= 2 && a.matchesSlotInput && a.samples > 0 && a.differs > a.samples / 2,
        "chatter source: with Mob in slot 4 the slices equal Mob's output");

  Layout crowdFirst = layoutOf({Card::InputGate, Card::Crowd, Card::Octave});
  crowdFirst.card[kSlot7] = Card::Bloom;
  const SourceRun c = runSource(crowdFirst, togglesOn({1, 2, kSlot7}), 600);
  check(c.slices >= 2 && c.matchesSlotInput && c.differs > c.samples / 2,
        "chatter source: with Mob ahead of Octave, the slices equal the block leaving slot 6");

  Layout early = layoutOf({Card::Bloom, Card::Empty, Card::Octave});
  const SourceRun first = runSource(early, togglesOn({0, 2}));
  check(first.slices >= 2 && first.matchesSlotInput && first.samples > 0,
        "chatter source: with the card in slot 1 the slices equal the chain input");
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
    PitchPath path;
    runStage(hand->engines, Card::Distortion, in.data(), next.data(), kN, snap.params, path);
    worst = std::max(worst, maxAbsDiff(run.out, next));
  }
  check(worst == 0.0f, "slot flip: a slot settled off runs no engine and the next slot gets the dry input");
}

}  // namespace

void checkChainRun() {
  checkRouting();
  checkEmptyAndPitchCards();
  checkWalkAgainstHand();
  checkEmptyChain();
  checkPitchCardsInSeries();
  checkGrowlRingRests();
  checkNanGuard();
  checkStageTiming();
  checkFrontHead();
  checkDefaultChain();
  checkBypassEnds();
  checkBypassFade();
  checkBypassMidAndReversal();
  checkSlotFadeLaw();
  checkSlotFadeHoldsLevel();
  checkSlotFadeLevels();
  checkSlotFlipToOff();
  checkSlotFlipOnClearsEngine();
  checkUnisonToggle();
  checkSlotFlipReverses();
  checkInputGateKeepsDetector();
  checkInputGateOffInPause();
  checkPitchCardToggles();
  checkPitchTogglesFadeOwnSlot();
  checkCrowdStage();
  checkCrowdPitchStage();
  checkCrowdToggleOff();
  checkAppendedCardsRun();
  checkSlotsEqualEngines();
  checkFormantReusesTracker();
  checkGroupedFormantLatency();
  checkApraxiaJoinsAutotunePass();
  checkAutotuneToggleUnderFormant();
  checkAutotuneTrackerForReaders();
  checkApraxiaBehindDeepthroat();
  checkBloomKey();
  checkBloomSource();
  checkSettledSlotsAreInert();
}
