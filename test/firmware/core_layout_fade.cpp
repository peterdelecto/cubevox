// Checks the audio block's pure policies on the host. Covers the layout change fade (the gain ramp,
// interrupted fades, the silent hold, the hold that keeps the output at zero under the mute
// sequence, the cut and the set of engines to reset), the output gain ramp, the ceiling ahead of the
// trim, the NaN guard in front of the polish stages and the stream restart budget.

#include "support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "core/audio_recovery.h"
#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/mute_sequence.h"
#include "core/output_stage.h"
#include "core/registry.h"
#include "core/settings.h"
#include "engine/polish.h"

namespace {

void checkFadeRamp() {
  LayoutFade fade;
  LayoutFade::Block b = fade.next(false);
  check(b.from == 1.0f && b.to == 1.0f && !b.swap, "fade: idle passes at unity");

  // Run a whole fade; the new layout is pending until the swap.
  float last = 1.0f;
  int outBlocks = 0;
  int inBlocks = 0;
  int swaps = 0;
  bool swapped = false;
  bool monotone = true;
  bool continuous = true;
  for (int i = 0; i < 4 * kFadeBlocks; ++i) {
    b = fade.next(!swapped);
    if (b.from != last) continuous = false;
    if (b.swap) {
      ++swaps;
      swapped = true;
      check(b.from == 0.0f && last == 0.0f, "fade: the swap block starts at zero");
    }
    if (!swapped) {
      ++outBlocks;
      if (b.to > b.from) monotone = false;
    } else {
      if (b.to > b.from) ++inBlocks;  // the silent hold blocks are not part of the ramp
      if (b.to < b.from) monotone = false;
    }
    last = b.to;
    if (swapped && b.to == 1.0f) break;
  }
  check(swaps == 1, "fade: one swap per layout change");
  check(monotone && continuous, "fade: gain falls then rises with no jump");
  check(outBlocks == kFadeBlocks && inBlocks == kFadeBlocks, "fade: each ramp takes kFadeBlocks blocks");
  check(last == 1.0f, "fade: gain returns to unity");
  const int rampMs = kFadeBlocks * cv::kBlock * 1000 / cv::kSampleRate;
  check(rampMs >= kFadeMs && rampMs <= kFadeMs + 2, "fade: each ramp lasts about 50 ms");
  b = fade.next(false);
  check(b.from == 1.0f && b.to == 1.0f && !b.swap, "fade: idle again after the ramp");
}

void checkFadeInterrupted() {
  LayoutFade::Block b{};
  // A second layout during the fade-out is taken at the same zero point: one swap, one fade.
  LayoutFade fade;
  int swaps = 0;
  for (int i = 0; i < 4 * kFadeBlocks; ++i) {
    b = fade.next(true);
    if (b.swap) {
      ++swaps;
      break;
    }
  }
  check(swaps == 1, "fade: a second layout mid-fade waits for the same zero point");

  // A layout arriving during the fade-in turns the ramp down again from where it is.
  fade = LayoutFade();
  bool pending = true;
  float at = 0.0f;
  for (int i = 0; i < 2 * kFadeBlocks; ++i) {
    b = fade.next(pending);
    if (b.swap) pending = false;
    if (b.swap) {
      for (int j = 0; j < 10; ++j) at = fade.next(false).to;
      break;
    }
  }
  b = fade.next(true);
  check(b.from == at && b.to < b.from && !b.swap, "fade: a new layout during fade-in ramps down from the current gain");

  // The layout changing back during the fade-out turns the ramp up with no swap.
  fade = LayoutFade();
  for (int i = 0; i < 10; ++i) b = fade.next(true);
  const float turn = b.to;
  b = fade.next(false);
  check(b.from == turn && b.to > b.from && !b.swap, "fade: changing back mid-fade-out ramps up without a swap");
}

void checkFadeHold() {
  LayoutFade fade(3);
  LayoutFade::Block b;
  int swaps = 0;
  int zeroBlocks = 0;
  bool inFade = false;
  float last = 1.0f;
  for (int i = 0; i < 6 * kFadeBlocks; ++i) {
    b = fade.next(swaps == 0);
    if (b.swap) ++swaps;
    if (swaps > 0 && b.from == 0.0f && b.to == 0.0f) ++zeroBlocks;
    if (swaps > 0 && b.to > b.from) inFade = true;
    check(b.from == last, "fade hold: no jump in gain");
    last = b.to;
    if (inFade && b.to == 1.0f) break;
  }
  check(swaps == 1 && zeroBlocks == 4 && last == 1.0f, "fade hold: the swap block plus 3 held blocks stay at zero, then the ramp returns");
}

// A layout change flags its swap block and the held blocks after it as silent, the blocks the
// caller spends on engine clears. The ramp blocks around them are not.
void checkFadeSilentBlocks() {
  LayoutFade fade;
  int silent = 0;
  int swaps = 0;
  bool silentIsSwapOrZero = true;
  bool pending = true;
  for (int i = 0; i < 4 * kFadeBlocks; ++i) {
    const LayoutFade::Block b = fade.next(pending);
    if (b.swap) {
      ++swaps;
      pending = false;
    }
    silentIsSwapOrZero = silentIsSwapOrZero && b.silent == (b.swap || (b.from == 0.0f && b.to == 0.0f));
    if (b.silent) ++silent;
    if (b.to == 1.0f && !pending) break;
  }
  check(swaps == 1 && silent == 1 + kFadeHoldBlocks && silentIsSwapOrZero,
        "fade silent: the swap block and the held blocks after it are silent, the ramps are not");
}

// hold() keeps the output at zero with the chain running, and a release fades up from zero.
void checkFadeHeld() {
  LayoutFade fade;
  fade.hold(true);
  LayoutFade::Block b = fade.next(false);
  check(b.from == 0.0f && b.to == 0.0f && !b.swap && !b.silent,
        "fade held: the first block drops to zero, and the chain keeps running under it");
  bool steady = true;
  for (int i = 0; i < 100; ++i) {
    b = fade.next(false);
    steady = steady && b.from == 0.0f && b.to == 0.0f && !b.swap && !b.silent;
  }
  check(steady, "fade held: the output stays at zero while the hold lasts");

  fade.hold(false);
  float last = 0.0f;
  int blocks = 0;
  bool smooth = true;
  for (int i = 0; i < 3 * kFadeBlocks; ++i) {
    b = fade.next(false);
    smooth = smooth && b.from == last && b.to > b.from && !b.silent;
    last = b.to;
    ++blocks;
    if (b.to == 1.0f) break;
  }
  check(blocks == kFadeBlocks && last == 1.0f && smooth, "fade held: a release fades up from zero in kFadeBlocks blocks, 50 ms");
  b = fade.next(false);
  check(b.from == 1.0f && b.to == 1.0f && !b.silent, "fade held: unity again after the fade up");

  // The hold takes a playing output to zero at once.
  LayoutFade playing;
  playing.next(false);
  playing.hold(true);
  b = playing.next(false);
  check(b.from == 0.0f && b.to == 0.0f, "fade held: a hold on a playing output drops it to zero");

  // A layout that arrives under the hold swaps at once, clears behind a silent hold of the usual
  // length, and then waits at zero for the release.
  LayoutFade swap;
  swap.hold(true);
  swap.next(false);
  b = swap.next(true);
  check(b.swap && b.silent && b.from == 0.0f && b.to == 0.0f, "fade held: a layout swaps at once under the hold");
  int silent = 0;
  for (int i = 0; i < 3 * kFadeHoldBlocks; ++i) {
    b = swap.next(false);
    if (!b.silent) break;
    ++silent;
  }
  check(silent == kFadeHoldBlocks && !b.silent && b.from == 0.0f && b.to == 0.0f,
        "fade held: the swap is followed by the usual silent blocks, then zero blocks that run the chain");
  swap.hold(false);
  b = swap.next(false);
  check(b.from == 0.0f && b.to > 0.0f && !b.swap, "fade held: the release after a swap fades up from zero");
}

// The mute sequence's hold drives the fade: the output stays at digital zero from power-on until the
// relay has settled, fades up over kFadeBlocks blocks after it, and a later mute and release repeat
// the pattern without the power-on wait. The sequence steps once a millisecond and a block lands
// every 4/3 ms, as 64 samples at 48 kHz do.
void checkMuteHoldDrivesFade() {
  MuteSequence mute;
  LayoutFade fade;
  mute.reset(true, 0);
  mute.clocksRunning();
  uint32_t blocks = 0;
  uint32_t firstAudible = 0;
  uint32_t unity = 0;
  float worstBeforeRelease = 0.0f;
  const uint32_t unmutedAt = kRelayCloseAfterPowerOnMs + kRelaySettleMs;
  bool pressed = false;
  uint32_t mutedAgainAt = 0;
  uint32_t firstAudible2 = 0;
  uint32_t unity2 = 0;
  for (uint32_t t = 0; t < 12000; ++t) {
    if (t == 7000) pressed = true;
    if (t == 7100) pressed = false;
    mute.update(t, !pressed);
    if (mutedAgainAt == 0 && mute.fullyMuted() && t > 7000) mutedAgainAt = t;
    while (blocks * 4 / 3 <= t) {
      fade.hold(mute.outputHeld());
      const LayoutFade::Block b = fade.next(false);
      ++blocks;
      if (t < unmutedAt) worstBeforeRelease = std::max(worstBeforeRelease, std::max(b.from, b.to));
      if (t < 7000) {
        if (b.to > 0.0f && firstAudible == 0) firstAudible = t;
        if (b.to == 1.0f && unity == 0) unity = t;
      } else if (t > 7100) {
        if (b.to > 0.0f && firstAudible2 == 0) firstAudible2 = t;
        if (b.to == 1.0f && unity2 == 0) unity2 = t;
      }
    }
  }
  check(worstBeforeRelease == 0.0f, "mute hold: the output is digital zero from power-on until the relay has settled");
  check(firstAudible >= unmutedAt && firstAudible <= unmutedAt + 3,
        "mute hold: the fade up starts when the relay has settled, 20 ms after MUTE_N rises at 6.0 s");
  const uint32_t rampMs = unity - firstAudible;
  check(rampMs >= kFadeMs - 2 && rampMs <= kFadeMs + 3, "mute hold: the fade up takes the fade's 50 ms");
  check(mutedAgainAt != 0 && firstAudible2 >= 7100 + kSwitchDebounceMs + kXsmtLeadMs + kRelaySettleMs &&
            firstAudible2 <= 7100 + kSwitchDebounceMs + kXsmtLeadMs + kRelaySettleMs + 3 && unity2 - firstAudible2 <= kFadeMs + 3,
        "mute hold: after a wanted mute the release is XSMT, output at zero, MUTE_N, 20 ms, fade up, with no 6 s wait");
}

void checkEnginesToReset() {
  const Layout base = defaultLayout();
  auto bit = [](Engine e) { return 1u << static_cast<int>(e); };
  check(enginesToReset(base, base) == 0, "reset: same layout resets nothing");

  Layout swapped = base;
  swapped.card[1] = Card::Octave;
  swapped.card[2] = Card::Autotune;
  check(enginesToReset(base, swapped) == bit(Engine::Pitch), "reset: swapping the two pitch cards resets the pitch engine once");

  Layout chasm = base;
  chasm.card[7] = Card::ReverbChasm;
  check(enginesToReset(base, chasm) == bit(Engine::Reverb), "reset: Spring to Chasm resets the reverb engine");

  Layout moved = base;
  moved.card[4] = Card::Empty;
  moved.card[0] = Card::Slapback;
  moved.card[1] = Card::Autotune;
  check(enginesToReset(base, moved) == (bit(Engine::InputGate) | bit(Engine::Slapback)),
        "reset: only engines in changed slots reset, Autotune stayed put");

  Layout empty = base;
  empty.card[2] = Card::Empty;
  check(enginesToReset(base, empty) == bit(Engine::Pitch), "reset: a card leaving a slot resets its engine");
  check(enginesToReset(empty, empty) == 0 && enginesToReset(Layout{{}}, Layout{{}}) == 0, "reset: empty slots reset nothing");
}

// A cut drops to zero at once, holds silent long enough to clear every engine, then fades in.
void checkFadeCut() {
  LayoutFade fade;
  fade.next(false);
  fade.cut();
  int silent = 0;
  LayoutFade::Block b{};
  for (int i = 0; i < 2 * kFadeHoldBlocks; ++i) {
    b = fade.next(false);
    if (b.from != 0.0f || b.to != 0.0f) break;
    check(!b.swap, "fade cut: the silent blocks swap nothing");
    ++silent;
  }
  check(silent == kFadeHoldBlocks + 1 && silent >= kEngineCount, "fade cut: one silent block per engine clear");
  check(b.from == 0.0f && b.to > 0.0f, "fade cut: the fade-in starts from zero");
  int in = 1;
  while (b.to < 1.0f && in < 2 * kFadeBlocks) {
    b = fade.next(false);
    ++in;
  }
  check(in == kFadeBlocks && fade.next(false).to == 1.0f, "fade cut: the fade-in takes kFadeBlocks blocks");

  LayoutFade pending;
  pending.cut();
  b = pending.next(true);
  check(b.swap && b.from == 0.0f && b.to == 0.0f, "fade cut: a layout waiting at the cut swaps at once");
}

// OUTPUT LINE to MIC is 30 dB. Across one block the gain moves 1/64 of the change per sample, where
// a step put the whole change between two samples.
void checkOutputGainRamp() {
  OutputGain gain;
  GainRamp r = gain.next(1.0f, 1.0f, 1.0f, cv::kBlock);
  check(r.start == 1.0f && r.step == 0.0f, "output gain: a steady trim is flat and exact");

  const float mic = std::pow(10.0f, kMicTrimDb / 20.0f);
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
// the shoulder times the trim, a chain clipped well past full scale lands there too, and LINE (a gain
// of exactly 1) matches the old order bit for bit.
void checkCeilingBeforeTrim() {
  const float mic = std::pow(10.0f, kMicTrimDb / 20.0f);
  const float shoulder = cv::softCeiling(1.0f);
  const float want = shoulder * mic;
  check(shoulder > 0.95f && shoulder < 0.96f, "ceiling: a 0 dBFS sample sits on the shoulder, 0.95");
  check(shapeOutput(1.0f, mic) == want && shapeOutput(-1.0f, mic) == -want,
        "ceiling: a 0 dBFS square through MIC lands at the shoulder times the trim");
  check(std::fabs(want - 0.03007f) < 1e-5f, "ceiling: MIC puts the square at 0.03007, 30.4 dB under full scale");

  // Four times full scale, clipped by the chain: the old order scaled it to 0.126, clear of the
  // ceiling, and left the flat top intact.
  const float oldOrder = clampSample(cv::softCeiling(4.0f * mic));
  const float newOrder = shapeOutput(4.0f, mic);
  check(std::fabs(oldOrder - 4.0f * mic) < 1e-7f, "ceiling: the old order never touched a MIC signal");
  check(std::fabs(newOrder - mic * cv::softCeiling(4.0f)) < 1e-9f && newOrder <= mic && newOrder > 0.98f * mic,
        "ceiling: a chain clipped past full scale is held at the ceiling times the trim");

  // Inside the knee both orders agree up to the product's rounding.
  check(std::fabs(shapeOutput(0.5f, mic) - 0.5f * mic) < 1e-9f, "ceiling: inside the knee the trim scales the signal untouched");

  // LINE: the gain ramp holds exactly 1, and every float in a sweep, the edge cases included, comes
  // out as the old order gave it.
  OutputGain gain;
  const GainRamp line = gain.next(1.0f, 1.0f, 1.0f, cv::kBlock);
  check(line.start == 1.0f && line.step == 0.0f, "ceiling: LINE runs a gain of exactly 1");
  int differing = 0;
  int tested = 0;
  const auto same = [&](float x) {
    const float a = shapeOutput(x, 1.0f);
    const float b = clampSample(cv::softCeiling(x * 1.0f));
    ++tested;
    if (std::memcmp(&a, &b, sizeof a) != 0) ++differing;
  };
  for (int i = -40000; i <= 40000; ++i) same(static_cast<float>(i) * 1e-4f);
  for (const float x : {0.0f, -0.0f, 1.0f, -1.0f, 1e-30f, 1e30f, -1e30f, INFINITY, -INFINITY, NAN}) same(x);
  check(differing == 0, "ceiling: LINE output equals the old order bit for bit");
  check(tested > 80000, "ceiling: the LINE sweep covers -4 to +4 and the edge cases");
}

// A sine block in, through the guard, then the polish EQ. Returns whether every output sample of
// the last block is finite and some are not zero.
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
    eq.process(x.data(), y.data(), cv::kBlock, params);
    finite = true;
    sound = false;
    for (float v : y) {
      finite = finite && std::isfinite(v);
      sound = sound || v != 0.0f;
    }
  }
  return finite && sound;
}

void checkNanGuard() {
  std::array<float, cv::kBlock> x{};
  x.fill(0.5f);
  check(guardBlock(x.data(), cv::kBlock) && x[3] == 0.5f, "nan guard: a finite block passes untouched");
  x[17] = NAN;
  check(!guardBlock(x.data(), cv::kBlock) && x[17] == 0.0f && x[3] == 0.0f, "nan guard: one NaN silences the block");
  x.fill(0.5f);
  x[63] = INFINITY;
  check(!guardBlock(x.data(), cv::kBlock), "nan guard: an infinity is caught");
  x.fill(0.5f);
  x[0] = -INFINITY;
  check(!guardBlock(x.data(), cv::kBlock), "nan guard: a negative infinity is caught");

  cv::Polish bare;
  check(!runEq(bare, false, true), "nan guard: one unguarded NaN leaves the EQ silent or non-finite for good");
  cv::Polish guarded;
  check(runEq(guarded, true, true), "nan guard: with the guard the EQ plays on after the NaN");
}

void checkStreamRecovery() {
  using Action = StreamRecovery::Action;
  StreamRecovery r;
  check(r.update(0, false, false) == Action::None && !r.holdMute(), "recovery: no fault, nothing to do");
  check(r.update(10, true, false) == Action::None && r.holdMute(), "recovery: a fault holds the mute");
  check(r.update(12, true, false) == Action::None && r.holdMute(), "recovery: the restart waits for the full mute");
  check(r.update(16, true, true) == Action::Restart && !r.holdMute() && r.restarts() == 1,
        "recovery: the stream restarts once the output is muted");
  check(r.update(20000, true, true) == Action::Restart && r.update(40000, true, true) == Action::Restart,
        "recovery: an already muted output restarts in the same pass");
  check(r.update(60015, true, true) == Action::Latch && r.latched() && r.restarts() == 3,
        "recovery: a fourth fault inside a minute of three restarts latches");
  check(r.update(200000, true, true) == Action::None && r.latched(), "recovery: nothing restarts after a latch");

  StreamRecovery edge;
  edge.update(0, true, true);
  edge.update(20000, true, true);
  edge.update(40000, true, true);
  check(edge.update(60000, true, true) == Action::Restart, "recovery: the window slides, a minute after the first");

  StreamRecovery spaced;
  bool latched = false;
  for (uint32_t i = 0; i < 10; ++i) latched = latched || spaced.update(i * 25000, true, true) == Action::Latch;
  check(!latched && spaced.restarts() == 10, "recovery: faults further apart restart every time");

  StreamRecovery failed;
  failed.update(0, true, true);
  failed.fail();
  check(failed.latched() && failed.update(1, true, true) == Action::None, "recovery: a failed restart latches");
}

}  // namespace

void checkFade() {
  checkFadeRamp();
  checkFadeInterrupted();
  checkFadeHold();
  checkFadeSilentBlocks();
  checkFadeHeld();
  checkMuteHoldDrivesFade();
  checkFadeCut();
  checkEnginesToReset();
  checkOutputGainRamp();
  checkCeilingBeforeTrim();
  checkNanGuard();
  checkStreamRecovery();
}
