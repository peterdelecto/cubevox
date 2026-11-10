// Checks the audio block's tail on the host: the output gain ramp, the soft ceiling ahead of the trim,
// the loop that shapes a block for the DAC, the guard in front of CLEANUP EQ's back half, and the block
// flow the board and the host share (the late block's dry pass and its blends, the runaway block that
// the guard cuts, and the engines that restart clean after it).

#include "support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/block_flow.h"
#include "core/chain_run.h"
#include "core/compose.h"
#include "core/late_block.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/output_stage.h"
#include "core/panel.h"
#include "core/settings.h"
#include "engine/polish.h"

namespace {

constexpr int kN = cv::kBlock;
using Block = std::array<float, kN>;

Block tone(int block, float amp) {
  constexpr double kPi = 3.14159265358979323846;
  Block x{};
  for (int i = 0; i < kN; ++i) {
    const double t = static_cast<double>(block * kN + i) / cv::kSampleRate;
    x[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * kPi * 220.0 * t));
  }
  return x;
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

float micGain() { return std::pow(10.0f, kMicTrimDb / 20.0f); }

// OUTPUT LINE to MIC is 30 dB. Across one block the gain moves 1/64 of the change per sample, where
// a step put the whole change between two samples.
void checkOutputGainRamp() {
  OutputGain gain;
  GainRamp r = gain.next(1.0f, 1.0f, 1.0f, cv::kBlock);
  check(r.start == 1.0f && r.step == 0.0f, "output gain: a steady trim is flat and exact");

  const float mic = micGain();
  r = gain.next(mic, 1.0f, 1.0f, cv::kBlock);
  const float change = 1.0f - mic;
  check(r.start == 1.0f && std::fabs(r.step + change / cv::kBlock) < 1e-7f,
        "output gain: a trim change ramps from the old gain across the block");
  check(std::fabs(r.step) < change / 60.0f, "output gain: no sample-to-sample step bigger than 1/64 of the change");
  const float end = r.start + r.step * cv::kBlock;
  const GainRamp after = gain.next(mic, 1.0f, 1.0f, cv::kBlock);
  check(std::fabs(after.start - end) < 1e-6f && after.step == 0.0f, "output gain: the next block carries on at the new trim");

  const GainRamp faded = gain.next(mic, 1.0f, 0.5f, cv::kBlock);
  check(faded.start == mic && std::fabs(faded.start + faded.step * cv::kBlock - 0.5f * mic) < 1e-7f,
        "output gain: the layout fade multiplies in");
}

// The ceiling shapes the full-scale signal and the trim scales the result. A 0 dBFS square lands at
// the shoulder times the trim, and a chain clipped well past full scale lands there too.
void checkCeilingBeforeTrim() {
  const float mic = micGain();
  const float shoulder = cv::softCeiling(1.0f);
  const float want = shoulder * mic;
  check(shoulder > 0.95f && shoulder < 0.96f, "ceiling: a 0 dBFS sample sits on the shoulder, 0.95");
  check(shapeOutput(1.0f, mic) == want && shapeOutput(-1.0f, mic) == -want,
        "ceiling: a 0 dBFS square through MIC lands at the shoulder times the trim");
  check(std::fabs(want - 0.03007f) < 1e-5f, "ceiling: MIC puts the square at 0.03007, 30.4 dB under full scale");

  const float clipped = shapeOutput(4.0f, mic);
  check(std::fabs(clipped - mic * cv::softCeiling(4.0f)) < 1e-9f && clipped <= mic && clipped > 0.98f * mic,
        "ceiling: a chain clipped past full scale is held at the ceiling times the trim");
  check(std::fabs(shapeOutput(0.5f, mic) - 0.5f * mic) < 1e-9f, "ceiling: inside the knee the trim scales the signal untouched");
}

// LINE runs a gain of exactly 1, and then every float in a sweep, the edge cases included, comes out
// as the ceiling alone gives it.
void checkLineIsCeilingAlone() {
  OutputGain gain;
  const GainRamp line = gain.next(1.0f, 1.0f, 1.0f, cv::kBlock);
  check(line.start == 1.0f && line.step == 0.0f, "ceiling: LINE runs a gain of exactly 1");
  int differing = 0;
  int tested = 0;
  const auto same = [&](float x) {
    ++tested;
    if (!sameBits(shapeOutput(x, 1.0f), cv::softCeiling(x))) ++differing;
  };
  for (int i = -40000; i <= 40000; ++i) same(static_cast<float>(i) * 1e-4f);
  const float edges[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-30f, 1e30f, -1e30f, INFINITY, -INFINITY, NAN};
  for (const float x : edges) same(x);
  check(differing == 0, "ceiling: LINE output is the ceiling alone, bit for bit");
  check(tested > 80000, "ceiling: the LINE sweep covers -4 to +4 and the edge cases");
}

// The reference loop: shapeOutput on every sample along the ramp.
void referenceEmit(const Block& a, GainRamp ramp, Block& out) {
  float gain = ramp.start;
  for (int i = 0; i < kN; ++i) {
    out[static_cast<size_t>(i)] = shapeOutput(a[static_cast<size_t>(i)], gain);
    gain += ramp.step;
  }
}

bool emitMatches(const Block& a, GainRamp ramp) {
  Block want{};
  Block got{};
  referenceEmit(a, ramp, want);
  emitOutput(a.data(), kN, ramp, [&got](int i, float v) { got[static_cast<size_t>(i)] = v; });
  bool same = true;
  for (size_t i = 0; i < want.size(); ++i) same = same && sameBits(want[i], got[i]);
  return same;
}

// The loop skips the ceiling and the clamp for a quiet block on a ramp inside [-1, 1], and must give the
// same samples as the reference everywhere else.
void checkEmitOutput() {
  const float mic = micGain();
  const Block quiet = tone(0, 0.5f);
  const Block loud = tone(0, 1.5f);
  Block nanInside = quiet;
  nanInside[20] = NAN;
  Block infinite = quiet;
  infinite[3] = -INFINITY;
  Block atKnee = quiet;
  atKnee[9] = cv::kSoftCeilingKnee;
  check(emitMatches(quiet, {1.0f, 0.0f}), "emit: a quiet block on LINE matches the reference");
  check(emitMatches(quiet, {1.0f, (mic - 1.0f) / kN}), "emit: a quiet block ramping to MIC matches the reference");
  check(emitMatches(quiet, {0.0f, 1.0f / kN}), "emit: a quiet block on a fade-in matches the reference");
  check(emitMatches(atKnee, {1.0f, 0.0f}), "emit: a sample exactly at the knee matches the reference");
  check(emitMatches(loud, {1.0f, 0.0f}), "emit: a loud block on LINE matches the reference");
  check(emitMatches(loud, {mic, 0.0f}), "emit: a loud block on MIC matches the reference");
  check(emitMatches(nanInside, {1.0f, 0.0f}), "emit: a NaN in a quiet block takes the full path and reads 0");
  check(emitMatches(infinite, {1.0f, 0.0f}), "emit: an infinity takes the full path and reads full scale");
  check(emitMatches(quiet, {0.0f, 1.5f / kN}), "emit: a ramp that ends past unity is clamped as the reference clamps");
  check(emitMatches(loud, {-0.5f, 2.0f / kN}), "emit: a ramp that crosses zero on a loud block matches the reference");
}

// A block with one sample set to `value`, over a quiet tone.
Block withSample(float value) {
  Block x = tone(0, 0.3f);
  x[17] = value;
  return x;
}

bool allZero(const Block& x) {
  return std::all_of(x.begin(), x.end(), [](float v) { return v == 0.0f; });
}

// The guard reads the block's sum of squares. A block over +60 dBFS RMS fails, so a finite runaway
// and samples of opposite sign that would cancel in a plain sum fail with the NaN and the infinity.
void checkGuardTrips() {
  Block x = tone(0, 0.5f);
  const Block quiet = x;
  check(guardBlock(x.data(), kN) && x == quiet, "guard: a finite block passes untouched");
  Block loud = tone(0, 4.0f);
  const Block loudCopy = loud;
  check(guardBlock(loud.data(), kN) && loud == loudCopy, "guard: a block at four times full scale still passes");
  x = withSample(NAN);
  check(!guardBlock(x.data(), kN) && allZero(x), "guard: one NaN silences the block");
  x = withSample(-NAN);
  check(!guardBlock(x.data(), kN) && allZero(x), "guard: a NaN of the other sign is caught too");
  x = withSample(INFINITY);
  check(!guardBlock(x.data(), kN) && allZero(x), "guard: an infinity is caught");
  x = withSample(-INFINITY);
  check(!guardBlock(x.data(), kN), "guard: a negative infinity is caught");
  x = withSample(1e20f);
  check(!guardBlock(x.data(), kN) && allZero(x), "guard: one finite sample of 1e20 is caught");

  Block alternating{};
  for (const float amp : {1e20f, 3e38f, 5000.0f}) {
    for (int i = 0; i < kN; ++i) alternating[static_cast<size_t>(i)] = (i & 1) ? -amp : amp;
    check(!guardBlock(alternating.data(), kN) && allZero(alternating),
          "guard: a block that alternates in sign and cancels in a plain sum is caught");
  }
  Block flat{};
  flat.fill(999.0f);
  check(guardBlock(flat.data(), kN), "guard: a block just under +60 dBFS RMS passes");
  flat.fill(1001.0f);
  check(!guardBlock(flat.data(), kN), "guard: a block just over +60 dBFS RMS is caught");
}

// A sine block through the guard, then the polish EQ. Returns whether every output sample of the last
// block is finite and some are not zero.
bool runEq(cv::Polish& eq, bool guarded, bool injectNan) {
  const cv::PolishParams params;
  std::array<float, cv::kBlock> x{};
  std::array<float, cv::kBlock> y{};
  bool finite = true;
  bool sound = false;
  for (int block = 0; block < 40; ++block) {
    for (int i = 0; i < cv::kBlock; ++i) {
      x[static_cast<size_t>(i)] = 0.25f * std::sin(0.05f * static_cast<float>(block * cv::kBlock + i));
    }
    if (injectNan && block == 10) x[17] = NAN;
    if (guarded) guardBlock(x.data(), cv::kBlock);
    eq.processBack(x.data(), y.data(), cv::kBlock, params);
    finite = true;
    sound = false;
    for (float v : y) {
      finite = finite && std::isfinite(v);
      sound = sound || v != 0.0f;
    }
  }
  return finite && sound;
}

void checkGuardKeepsEqFinite() {
  cv::Polish bare;
  check(!runEq(bare, false, true), "guard: one unguarded NaN leaves the EQ silent or non-finite for good");
  cv::Polish guarded;
  check(runEq(guarded, true, true), "guard: with the guard the EQ plays on after the NaN");
}

// Holds the engines of one chain, built the way the board builds them. It is too big for the stack.
struct Rig {
  std::unique_ptr<cv::AutotuneFx> autotune = std::make_unique<cv::AutotuneFx>();
  std::unique_ptr<cv::OctaveFx> octave = std::make_unique<cv::OctaveFx>();
  std::unique_ptr<cv::Unison> unison = std::make_unique<cv::Unison>();
  std::unique_ptr<cv::Distortion> distortion = std::make_unique<cv::Distortion>();
  ChainEngines engines{*autotune, *octave, *unison, *distortion};
};

struct StillClock {
  uint32_t operator()() { return 0; }
};

// Every knob at 0.5 and every toggle on, with CLEANUP EQ (at STRONG) and OUTPUT PAD as given.
ChainSnapshot snapshotOf(const Layout& layout, bool eq, bool mic) {
  PanelReading panel;
  panel.toggle.fill(true);
  for (auto& slot : panel.knob) slot.fill(0.5f);
  Settings settings;
  settings.cleanupEq = eq ? CleanupEq::Strong : CleanupEq::Off;
  if (mic) settings.outputLevel = OutputLevel::Mic;
  return composeSnapshot(layout, panel, settings, BenchOverrides());
}

Layout emptyLayout() {
  Layout layout;
  layout.card.fill(Card::Empty);
  return layout;
}

Layout distortionOnly() {
  Layout layout = emptyLayout();
  layout.card[0] = Card::Distortion;
  return layout;
}

// One block through a BlockFlow with a snapshot that never changes, so the block never swaps.
struct Played {
  Block out{};
  BlockFlow::Report report;
};

Played play(BlockFlow& flow, Rig& rig, const ChainSnapshot& snap, const Block& in, bool bypass = false) {
  Played p;
  Block a = in;
  Block b{};
  const BlockFlow::Start start = flow.begin(false, false);
  p.report = flow.run(rig.engines, snap, start, a.data(), b.data(), bypass, StillClock(),
                      [&p](int i, float v) { p.out[static_cast<size_t>(i)] = v; });
  return p;
}

// The chain alone on a reference rig, as it stands ahead of the output stage.
Block chainOut(Rig& rig, SlotFades& fades, const ChainSnapshot& snap, const Block& in) {
  Block a = in;
  Block scratch{};
  ChainCycles cycles;
  runChain(rig.engines, fades, a.data(), scratch.data(), kN, snap, StillClock(), cycles);
  return a;
}

// A block through the output stage at the MIC trim, which has settled.
Block stageOf(const Block& a) {
  Block out{};
  emitOutput(a.data(), kN, {micGain(), 0.0f}, [&out](int i, float v) { out[static_cast<size_t>(i)] = v; });
  return out;
}

// The pre-stage block a late join gives: the first kLateBlendSamples samples ease from `from`.
Block blended(const Block& a, float from) {
  Block x = a;
  for (int i = 0; i < kLateBlendSamples; ++i) {
    const float t = static_cast<float>(i + 1) / static_cast<float>(kLateBlendSamples);
    x[static_cast<size_t>(i)] = from + (a[static_cast<size_t>(i)] - from) * t;
  }
  return x;
}

// A late block raises the flag. The next block skips the chain and plays the dry input through the
// output stage, the dry counter reads 1, and the block after runs the chain again. Each join blends in
// from the last sample over kLateBlendSamples samples.
void checkLateBlockPassesDry() {
  const ChainSnapshot snap = snapshotOf(distortionOnly(), true, true);
  Rig rig;
  Rig reference;
  Rig other;
  SlotFades referenceFades;
  SlotFades otherFades;
  BlockFlow flow;

  // The first block ramps the trim to MIC, so the blocks after it run flat at the trim.
  play(flow, rig, snap, tone(0, 1.5f));
  chainOut(reference, referenceFades, snap, tone(0, 1.5f));
  const Played second = play(flow, rig, snap, tone(1, 1.5f));
  const Block chain1 = chainOut(reference, referenceFades, snap, tone(1, 1.5f));
  check(second.report.chainRan && flow.dryBlocks() == 0 && second.out == stageOf(chain1),
        "late block: with no flag the block runs the chain");

  const Block in2 = tone(2, 1.5f);
  flow.markLate();
  const Played dry = play(flow, rig, snap, in2);
  check(!dry.report.chainRan && flow.dryBlocks() == 1, "late block: the block after a late one passes the dry input and counts 1");
  check(dry.out == stageOf(blended(in2, chain1[kN - 1])),
        "late block: its output is the dry input through the output stage, eased in from the last sample");
  const Block plain = stageOf(in2);
  bool bodyIsInput = true;
  for (size_t i = kLateBlendSamples; i < dry.out.size(); ++i) bodyIsInput = bodyIsInput && dry.out[i] == plain[i];
  check(bodyIsInput, "late block: after the blend the dry block is the input, trim and ceiling");
  check(dry.out != stageOf(chainOut(other, otherFades, snap, in2)), "late block: the dry block is the input, not the chain's output");
  check(std::fabs(dry.out[0] - second.out[kN - 1]) < 0.15f * std::fabs(plain[0] - second.out[kN - 1]),
        "late block: the join into the dry block steps by a fraction of the hard switch");

  // The reference ran blocks 0 and 1, so it has the engine history the dry block left behind.
  const Block in3 = tone(3, 1.5f);
  const Played after = play(flow, rig, snap, in3);
  const Block chain3 = chainOut(reference, referenceFades, snap, in3);
  check(after.report.chainRan && flow.dryBlocks() == 1, "late block: the flag clears, so the block after the dry one runs the chain");
  check(after.out == stageOf(blended(chain3, in2[kN - 1])),
        "late block: the block after the dry one is the chain's output eased in from the dry block's last sample, the engines one block stale");
}

// The policy alone: a block that skips the chain anyway takes the flag without counting it.
void checkLatePolicy() {
  LateBlocks late;
  late.markLate();
  check(!late.takeDry(true) && late.dryBlocks() == 0 && !late.takeDry(false),
        "late block: a silent block clears the flag and is not counted");
  late.markLate();
  check(late.takeDry(false) && late.dryBlocks() == 1, "late block: a flagged block passes dry and counts");
  late.clearCount();
  late.markLate();
  check(late.takeDry(false) && late.dryBlocks() == 1, "late block: the count restarts from clearCount");
}

// Runs `blocks` blocks of a steady tone through a flow, with the block `poisonAt` replaced by `bad`.
// Returns the last block's output and counts the blocks the guard discarded.
Block runWithPoison(const ChainSnapshot& snap, int blocks, int poisonAt, const Block& bad, int& discarded) {
  Rig rig;
  BlockFlow flow;
  Played p;
  discarded = 0;
  for (int b = 0; b < blocks; ++b) {
    p = play(flow, rig, snap, b == poisonAt ? bad : tone(b, 0.3f));
    if (p.report.discarded) ++discarded;
  }
  return p.out;
}

// A slot's runaway block, whatever its shape, is cut and counted, and the box then plays exactly what
// a clean box plays. CLEANUP EQ is off, because the poison enters at the input and would reach its front
// half, which the ADC cannot do.
void checkRunawayRecovers() {
  const ChainSnapshot snap = snapshotOf(emptyLayout(), false, false);
  int clean = 0;
  const Block want = runWithPoison(snap, 600, -1, Block{}, clean);
  Block alternating{};
  Block huge{};
  Block giant{};
  huge.fill(1e20f);
  for (int i = 0; i < kN; ++i) {
    alternating[static_cast<size_t>(i)] = (i & 1) ? -1e20f : 1e20f;
    giant[static_cast<size_t>(i)] = (i & 1) ? -3e38f : 3e38f;
  }
  const Block cases[] = {withSample(NAN), withSample(INFINITY), huge, alternating, giant};
  const char* names[] = {"a NaN", "an infinity", "a block at +1e20", "a block alternating at 1e20", "a block alternating at 3e38"};
  check(clean == 0, "runaway: a clean run discards nothing");
  for (size_t k = 0; k < 5; ++k) {
    int discarded = 0;
    const Block got = runWithPoison(snap, 600, 20, cases[k], discarded);
    float worst = 0.0f;
    for (size_t i = 0; i < got.size(); ++i) worst = std::max(worst, std::fabs(got[i] - want[i]));
    char what[120];
    std::snprintf(what, sizeof what, "runaway: %s is cut and counted once, and the box then plays what a clean box plays", names[k]);
    check(discarded == 1 && worst < 1e-5f && !allZero(got), what);
  }
}

// After a discard the layout fade cuts, so the next blocks are silent while the engines clear.
void checkDiscardCuts() {
  const ChainSnapshot snap = snapshotOf(emptyLayout(), false, false);
  Rig rig;
  BlockFlow flow;
  play(flow, rig, snap, tone(0, 0.3f));
  Block bad{};
  bad.fill(1e20f);
  const Played cut = play(flow, rig, snap, bad);
  const BlockFlow::Start next = flow.begin(false, false);
  check(cut.report.discarded && next.fade.silent && next.fade.to == 0.0f, "runaway: the block after a discard is silent");
}

// The newest block the scope's ring holds.
Block newestBlock(const ScopeRing& ring) {
  std::array<float, kScopeReadSamples> all;
  ring.read(all.data());
  Block b;
  std::copy(all.end() - kN, all.end(), b.begin());
  return b;
}

// Every audio block writes one block into the scope's ring, through a layout swap with its silent
// blocks, a late block and a NaN cut. A chain block writes the chain output, ahead of the output
// gain, so the fade-out blocks still write the full signal. A silent block, a dry block and a cut
// block write zeros.
void checkScopeRingAdvances() {
  const ChainSnapshot snap = snapshotOf(emptyLayout(), false, false);
  Rig rig;
  BlockFlow flow;
  bool content = true;
  int silent = 0;
  int dry = 0;
  int cut = 0;
  int fading = 0;
  bool swapped = false;
  for (int b = 0; b < 220; ++b) {
    const Block in = b == 170 ? withSample(NAN) : tone(b, 0.5f);
    if (b == 150) flow.markLate();
    const BlockFlow::Start start = flow.begin(false, b >= 5 && !swapped);
    swapped = swapped || start.fade.swap;
    Block a = in;
    Block scratch{};
    const BlockFlow::Report report =
        flow.run(rig.engines, snap, start, a.data(), scratch.data(), false, StillClock(), [](int, float) {});
    const bool zeros = start.fade.silent || start.dry || report.discarded;
    const Block got = newestBlock(flow.scope());
    content = content && (zeros ? allZero(got) : got == in);
    silent += start.fade.silent ? 1 : 0;
    dry += start.dry ? 1 : 0;
    cut += report.discarded ? 1 : 0;
    fading += (!zeros && start.fade.to < start.fade.from) ? 1 : 0;
  }
  check(silent > 0 && dry == 1 && cut == 1 && fading > 0, "scope ring: the run met a swap's silent blocks, one late block, one NaN cut and a fade-out");
  check(content, "scope ring: the newest block follows every audio block through a swap, a late block and a NaN cut; chain blocks write the chain output, even while the output fades, and silent, dry and cut blocks write zeros");
}

// OUTPUT PAD changes the output and leaves the ring as it was, since the scope reads the chain
// output ahead of the output trim.
void checkScopeAheadOfTrim() {
  const ChainSnapshot line = snapshotOf(emptyLayout(), false, false);
  const ChainSnapshot mic = snapshotOf(emptyLayout(), false, true);
  Rig rigA;
  Rig rigB;
  BlockFlow flowA;
  BlockFlow flowB;
  bool same = true;
  bool outputsDiffer = false;
  for (int b = 0; b < 400; ++b) {
    const Block in = tone(b, 1.2f);
    const Played pa = play(flowA, rigA, line, in);
    const Played pb = play(flowB, rigB, mic, in);
    same = same && newestBlock(flowA.scope()) == newestBlock(flowB.scope());
    outputsDiffer = outputsDiffer || pa.out != pb.out;
  }
  check(same && outputsDiffer, "scope ring: an OUTPUT PAD setting that changes the output leaves the ring the same");
}

// The default layout with every knob at 0.5 and the toggles as given, CLEANUP EQ off.
ChainSnapshot defaultWith(const Layout& layout, const std::array<bool, kSlotCount>& on) {
  PanelReading panel;
  panel.toggle = on;
  for (auto& slot : panel.knob) slot.fill(0.5f);
  return composeSnapshot(layout, panel, Settings(), BenchOverrides());
}

int slotOf(const Layout& layout, Card c) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (layout.card[static_cast<size_t>(s)] == c) return s;
  }
  return -1;
}

// Fills the Reverb with a tone, switches it off and lets every other tail die on silence.
void fillThenStopReverb(BlockFlow& flow, Rig& rig, const ChainSnapshot& on, const ChainSnapshot& off) {
  const Block silence{};
  for (int b = 0; b < 400; ++b) play(flow, rig, on, tone(b, 0.5f));
  for (int b = 0; b < 10; ++b) play(flow, rig, off, tone(b, 0.5f));
  for (int b = 0; b < 3000; ++b) play(flow, rig, off, silence);
}

double energy(const Block& x) {
  double e = 0.0;
  for (float v : x) e += static_cast<double>(v) * static_cast<double>(v);
  return e;
}

// A Reverb switched off keeps its tail frozen. Switched on again it starts from silence, by a plain
// toggle and by a toggle that goes on while a layout change in another slot fades out.
void checkSwapClearsReengaged() {
  const Layout base = defaultLayout();
  const int reverb = slotOf(base, Card::ReverbSpring);
  const int distortion = slotOf(base, Card::Distortion);
  check(reverb >= 0 && distortion >= 0, "swap: the default layout holds Spring Reverb and Distortion");
  if (reverb < 0 || distortion < 0) return;
  std::array<bool, kSlotCount> allOn{};
  allOn.fill(true);
  std::array<bool, kSlotCount> reverbOff = allOn;
  reverbOff[static_cast<size_t>(reverb)] = false;
  Layout next = base;
  next.card[static_cast<size_t>(distortion)] = Card::Lofi;
  const ChainSnapshot on = defaultWith(base, allOn);
  const ChainSnapshot off = defaultWith(base, reverbOff);
  const ChainSnapshot swapped = defaultWith(next, allOn);
  const Block silence{};

  Rig plainRig;
  BlockFlow plainFlow;
  fillThenStopReverb(plainFlow, plainRig, on, off);
  double plain = 0.0;
  for (int b = 0; b < 100; ++b) plain += energy(play(plainFlow, plainRig, on, silence).out);

  Rig swapRig;
  BlockFlow swapFlow;
  fillThenStopReverb(swapFlow, swapRig, on, off);
  ChainSnapshot held = off;
  bool pending = true;
  int afterSwap = 0;
  double across = 0.0;
  for (int b = 0; b < 200 && afterSwap < 100; ++b) {
    const BlockFlow::Start start = swapFlow.begin(false, pending);
    if (start.fade.swap) {
      swapFlow.adopt(held, swapped);
      held = swapped;
      pending = false;
    }
    Block a = silence;
    Block scratch{};
    Block out{};
    swapFlow.run(swapRig.engines, held, start, a.data(), scratch.data(), false, StillClock(),
                 [&out](int i, float v) { out[static_cast<size_t>(i)] = v; });
    if (!pending) {
      across += energy(out);
      ++afterSwap;
    }
  }
  check(afterSwap == 100 && plain < 1e-9 && across < 1e-9,
        "swap: a Reverb toggled on across a layout swap starts from silence, as a plain toggle does");
}

}  // namespace

void checkBlockTail() {
  checkOutputGainRamp();
  checkCeilingBeforeTrim();
  checkLineIsCeilingAlone();
  checkEmitOutput();
  checkGuardTrips();
  checkGuardKeepsEqFinite();
  checkLatePolicy();
  checkLateBlockPassesDry();
  checkRunawayRecovers();
  checkDiscardCuts();
  checkScopeRingAdvances();
  checkScopeAheadOfTrim();
  checkSwapClearsReengaged();
}
