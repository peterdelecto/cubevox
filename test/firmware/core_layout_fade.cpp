// Checks the audio block's pure policies on the host. Covers the layout change fade (the gain ramp,
// interrupted fades, the silent hold, the hold that keeps the output at zero under the mute
// sequence, the cut and the set of engines to reset) and the stream restart budget. The output
// stage and the block flow are in core_block_tail.cpp.

#include "support.h"

#include <algorithm>
#include <cmath>

#include "core/audio_recovery.h"
#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/mute_sequence.h"
#include "core/registry.h"
#include "core/settings.h"

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
      for (int j = 0; j < kFadeHoldBlocks + 1; ++j) at = fade.next(false).to;  // the hold, then one block of the fade-in
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
}

// The hold takes a playing output to zero at once. A layout that arrives under the hold swaps at once,
// clears behind a silent hold of the usual length, and then waits at zero for the release.
void checkFadeHeldSwaps() {
  LayoutFade playing;
  playing.next(false);
  playing.hold(true);
  LayoutFade::Block b = playing.next(false);
  check(b.from == 0.0f && b.to == 0.0f, "fade held: a hold on a playing output drops it to zero");

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
struct MuteFadeRun {
  float worstBeforeRelease = 0.0f;  // the loudest block edge before the relay has settled
  uint32_t firstAudible = 0;        // the first millisecond a block ends above zero, and the first at unity
  uint32_t unity = 0;
  uint32_t mutedAgainAt = 0;        // the second mute, pressed at 7.0 s and released at 7.1 s
  uint32_t firstAudible2 = 0;
  uint32_t unity2 = 0;
};

MuteFadeRun runMuteFade(uint32_t unmutedAt) {
  MuteSequence mute;
  LayoutFade fade;
  mute.reset(true, 0);
  mute.clocksRunning();
  MuteFadeRun r;
  uint32_t blocks = 0;
  bool pressed = false;
  for (uint32_t t = 0; t < 12000; ++t) {
    if (t == 7000) pressed = true;
    if (t == 7100) pressed = false;
    mute.update(t, !pressed);
    if (r.mutedAgainAt == 0 && mute.fullyMuted() && t > 7000) r.mutedAgainAt = t;
    while (blocks * 4 / 3 <= t) {
      fade.hold(mute.outputHeld());
      const LayoutFade::Block b = fade.next(false);
      ++blocks;
      if (t < unmutedAt) r.worstBeforeRelease = std::max(r.worstBeforeRelease, std::max(b.from, b.to));
      if (t < 7000) {
        if (b.to > 0.0f && r.firstAudible == 0) r.firstAudible = t;
        if (b.to == 1.0f && r.unity == 0) r.unity = t;
      } else if (t > 7100) {
        if (b.to > 0.0f && r.firstAudible2 == 0) r.firstAudible2 = t;
        if (b.to == 1.0f && r.unity2 == 0) r.unity2 = t;
      }
    }
  }
  return r;
}

void checkMuteHoldDrivesFade() {
  const uint32_t unmutedAt = kRelayCloseAfterPowerOnMs + kRelaySettleMs;
  const MuteFadeRun r = runMuteFade(unmutedAt);
  check(r.worstBeforeRelease == 0.0f, "mute hold: the output is digital zero from power-on until the relay has settled");
  check(r.firstAudible >= unmutedAt && r.firstAudible <= unmutedAt + 3,
        "mute hold: the fade up starts when the relay has settled, 20 ms after MUTE_N rises at 6.0 s");
  const uint32_t rampMs = r.unity - r.firstAudible;
  check(rampMs >= kFadeMs - 2 && rampMs <= kFadeMs + 3, "mute hold: the fade up takes the fade's 50 ms");
  const uint32_t released = 7100 + kSwitchDebounceMs + kXsmtLeadMs + kRelaySettleMs;
  check(r.mutedAgainAt != 0 && r.firstAudible2 >= released && r.firstAudible2 <= released + 3 &&
            r.unity2 - r.firstAudible2 <= kFadeMs + 3,
        "mute hold: after a wanted mute the release is XSMT, output at zero, MUTE_N, 20 ms, fade up, with no 6 s wait");
}

void checkEnginesToReset() {
  const Layout base = defaultLayout();
  auto bit = [](Engine e) { return 1u << static_cast<int>(e); };
  check(enginesToReset(base, base) == 0, "reset: same layout resets nothing");

  Layout swapped = base;
  swapped.card[1] = Card::Octave;
  swapped.card[2] = Card::Autotune;
  check(enginesToReset(base, swapped) == (bit(Engine::Autotune) | bit(Engine::Octave)),
        "reset: swapping Autotune and Octave resets each engine once");

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
  check(enginesToReset(base, empty) == bit(Engine::Octave), "reset: a card leaving a slot resets its engine");
  check(enginesToReset(empty, empty) == 0 && enginesToReset(Layout{{}}, Layout{{}}) == 0, "reset: empty slots reset nothing");
}

// The slot 7 choices: Gate and the incoming card's engine reset, and no other. The Octave to Formant swap in
// slot 3 resets both engines, and Formant to Apraxia, which share the Formant engine, resets it once.
void checkEnginesToResetAtSlot7() {
  const Layout base = defaultLayout();
  auto bit = [](Engine e) { return 1u << static_cast<int>(e); };
  Layout formant = base;
  formant.card[2] = Card::Formant;
  Layout lofi = base;
  lofi.card[6] = Card::Lofi;
  Layout bloom = base;
  bloom.card[6] = Card::Bloom;
  Layout apraxia = base;
  apraxia.card[2] = Card::FormantFollower;
  check(enginesToReset(base, formant) == (bit(Engine::Octave) | bit(Engine::Formant)),
        "reset: Octave to Formant in slot 3 resets the Octave and Formant engines");
  check(enginesToReset(formant, apraxia) == bit(Engine::Formant),
        "reset: Formant to Apraxia in slot 3 resets the Formant engine once, which both cards share");
  check(enginesToReset(base, lofi) == (bit(Engine::Gate) | bit(Engine::Lofi)) &&
            enginesToReset(base, bloom) == (bit(Engine::Gate) | bit(Engine::Bloom)) &&
            enginesToReset(lofi, bloom) == (bit(Engine::Lofi) | bit(Engine::Bloom)),
        "reset: a slot 7 swap resets the engine it leaves and the engine it enters");
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
  check(r.update(60015, true, true) == Action::Reset && !r.holdMute() && r.restarts() == 3,
        "recovery: a fourth fault inside a minute of three restarts resets the box");
  check(r.update(200000, true, true) == Action::None && r.restarts() == 3,
        "recovery: nothing restarts while the box waits for its reset");

  StreamRecovery edge;
  edge.update(0, true, true);
  edge.update(20000, true, true);
  edge.update(40000, true, true);
  check(edge.update(60000, true, true) == Action::Restart, "recovery: the window slides, a minute after the first");

  StreamRecovery refilled;
  refilled.update(0, true, true);
  refilled.update(1000, true, true);
  refilled.update(2000, true, true);
  check(refilled.update(90000, true, true) == Action::Restart && refilled.restarts() == 4,
        "recovery: a fault after the minute has passed restarts the stream as usual");

  StreamRecovery spaced;
  bool reset = false;
  for (uint32_t i = 0; i < 10; ++i) reset = reset || spaced.update(i * 25000, true, true) == Action::Reset;
  check(!reset && spaced.restarts() == 10, "recovery: faults further apart restart every time");

  StreamRecovery failed;
  failed.update(0, true, true);
  check(failed.fail() == Action::Reset && !failed.holdMute(), "recovery: a restart that fails resets the box");
  check(failed.update(1, true, true) == Action::None, "recovery: nothing restarts after a failed restart");
}

}  // namespace

void checkFade() {
  checkFadeRamp();
  checkFadeInterrupted();
  checkFadeHold();
  checkFadeSilentBlocks();
  checkFadeHeld();
  checkFadeHeldSwaps();
  checkMuteHoldDrivesFade();
  checkFadeCut();
  checkEnginesToReset();
  checkEnginesToResetAtSlot7();
  checkStreamRecovery();
}
