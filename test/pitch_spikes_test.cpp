// Worst blocks of the pitch tracker, on Adam's three clips. The box's stress runs (fw v2.01.44) read two
// spikes the mean hides, and this holds each fix.
//
// 1. The onset estimate (pitch.h). Its 48 kHz refinement summed 13 lags, 1.6 host us, in the block of the
//    estimate's first stage, so the pitch slot's worst block went from the voiced hop's 2.5 us to 3.4 us.
//    The refinement now walks downhill from the coarse period, three lags in the usual case and at most
//    kOnsetMaxLags. The worst block with the estimate must stay within a margin of the worst block without it.
// 2. A pitch stage switched on (pitch_fx.h, chain_run.cpp). Its reset cleared the 51 kB input ring in one
//    block. The board's memset stores a byte at a time, about 250k cycles, so the one block read 47 % of
//    the budget against a settled worst of 7 %. A stopped stage now clears the ring a slice a block, and
//    the switch-on finds it clear. The host's memset is fast, so the ring work is counted as well as timed.
//    The Ring mod card keeps a ring and a tracker of its own and rests the same way, so the check runs on
//    it too.
//
// Block times are the minimum over several passes of one deterministic run, in host nanoseconds. No window,
// no audio device.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "engine/pitch.h"

#include "core/chain_run.h"
#include "core/compose.h"
#include "core/layout.h"
#include "core/panel.h"
#include "core/registry.h"
#include "core/settings.h"

#include "clip_util.h"
#include "ringed_tracker.h"

#ifndef CUBEVOX_SAMPLES_DIR
#define CUBEVOX_SAMPLES_DIR "samples"
#endif

namespace {

using Signal = std::vector<float>;

constexpr int kBlk = cv::kBlock;
constexpr int kPasses = 9;
constexpr int kAttempts = 3;

// Host noise margin on a worst-block comparison: a fraction of the settled worst, and a few ticks of the
// clock (about 41 ns each on the Mac).
constexpr double kRatioMargin = 1.12;
constexpr double kTickMarginNs = 120.0;

uint32_t nowNs() {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0, double d = 0.0) {
  char detail[300];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// 1. The tracker alone. Per block: the time of the push, the minimum over the passes, and what ran in it.
struct TrackerBlock {
  double ns = 0.0;
  bool voiced = false;
  bool onsetWork = false;  // an estimate ran in the block
  int refineLags = 0;
};

// The refinement may sum 13 lags; it sums about this many on average on the clips.
constexpr double kMeanLagsLimit = 6.0;

void trackerRun(const Signal& x, bool first, std::vector<TrackerBlock>& best) {
  RingedTracker tr;
  uint32_t runs = tr.onsetRunsForTest();
  size_t b = 0;
  for (size_t i = 0; i + kBlk <= x.size(); i += kBlk, ++b) {
    const uint32_t t0 = nowNs();
    tr.push(&x[i], kBlk);
    const double ns = static_cast<double>(nowNs() - t0);
    if (first) {
      TrackerBlock q;
      q.voiced = tr.result().voiced;
      q.refineLags = tr.refineLagsLastPushForTest();
      q.onsetWork = tr.onsetRunsForTest() != runs;
      q.ns = ns;
      best.push_back(q);
    } else {
      best[b].ns = std::min(best[b].ns, ns);
    }
    runs = tr.onsetRunsForTest();
  }
}

bool testOnsetSpread(const std::vector<Signal>& clips) {
  bool ok = true;
  int clipNo = 0;
  for (const Signal& clip : clips) {
    ++clipNo;
    double settled = 0.0, worst = 0.0, meanLags = 0.0;
    int overLimit = 0, estimates = 0, onsetBlocks = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
      std::vector<TrackerBlock> best;
      for (int pass = 0; pass < kPasses; ++pass) trackerRun(clip, pass == 0, best);
      settled = worst = 0.0;
      overLimit = estimates = onsetBlocks = 0;
      int lags = 0;
      for (const TrackerBlock& q : best) {
        worst = std::max(worst, q.ns);
        if (q.voiced && !q.onsetWork) settled = std::max(settled, q.ns);
        overLimit += q.refineLags > cv::PitchTracker::onsetMaxLagsForTest();
        estimates += q.refineLags > 0;
        lags += q.refineLags;
        onsetBlocks += q.onsetWork;
      }
      meanLags = estimates > 0 ? static_cast<double>(lags) / estimates : 0.0;
      if (worst <= settled * kRatioMargin + kTickMarginNs) break;
    }
    char name[100];
    std::snprintf(name, sizeof name, "clip %d: the onset estimate adds no worst block", clipNo);
    ok &= report(name, worst <= settled * kRatioMargin + kTickMarginNs,
                 "worst block %.0f ns, settled worst (voiced hop) %.0f ns (x%.2f + %.0f ns allowed)", worst, settled,
                 kRatioMargin, kTickMarginNs);
    std::snprintf(name, sizeof name, "clip %d: the refinement sums few lags", clipNo);
    ok &= report(name, onsetBlocks > 0 && estimates > 0 && overLimit == 0 && meanLags <= kMeanLagsLimit,
                 "blocks with an estimate run %.0f (> 0), refinements %.0f (> 0), over the lag limit %.0f (0), mean lags "
                 "%.1f (<= 6)",
                 onsetBlocks, estimates, overLimit, meanLags);
  }
  return ok;
}

// 2. A card switched on that keeps a ring and a tracker. Layout: Input gate, two empty slots, Unison, Echo, the
// subject, Gate, Spring. The subject is Deepthroat, whose toggle starts and stops the shared pitch stage and
// nothing else does, or Ring mod, whose toggle starts and stops the card's own tracker.
struct Rig {
  std::unique_ptr<cv::PitchFx> pitch = std::make_unique<cv::PitchFx>();
  std::unique_ptr<cv::Unison> unison = std::make_unique<cv::Unison>();
  std::unique_ptr<cv::Distortion> distortion = std::make_unique<cv::Distortion>();
  std::unique_ptr<cv::RingMod> ringMod = std::make_unique<cv::RingMod>();
  ChainEngines engines{*pitch, *unison, *distortion, *ringMod};
  SlotFades fades;
};

constexpr int kSubjectSlot = 5;
constexpr int kFlipEvery = 225;  // 0.3 s, as firmware/stress.py spaces its toggle flips
constexpr int kFirstFlip = 300;
constexpr int kFlips = 8;        // off, on, off, on ...
constexpr int kPair = 2 * kFlipEvery;
constexpr int kSettleBlocks = 30;
constexpr int kShortRest = kSlotFadeBlocks + 2;  // the fade-out ends and two blocks rest before the toggle comes back

Layout trackerLayout(Card subject) {
  Layout layout;
  layout.card = {{Card::InputGate, Card::Empty, Card::Empty, Card::Unison, Card::Slapback, subject, Card::Gate,
                  Card::ReverbSpring}};
  return layout;
}

ChainSnapshot trackerSnapshot(Card subject) {
  PanelReading panel;
  panel.toggle.fill(true);
  for (auto& slot : panel.knob) slot.fill(0.5f);
  return composeSnapshot(trackerLayout(subject), panel, Settings{}, BenchOverrides());
}

struct SwitchRun {
  std::vector<double> ns;       // the subject slot's time, block by block
  uint32_t maxClearedInBlock = 0;
  uint32_t clearedAtSwitchOn = 0;  // floats the ring clears zeroed in the switch-on blocks, summed
  int ringNotClear = 0;         // switch-on blocks that left a float outside the new call nonzero
  int blocks = 0;
};

// The floats the subject's ring has cleared since construction.
uint32_t ringCleared(const Rig& rig, Card subject) {
  return subject == Card::RingMod ? rig.ringMod->ringClearedForTest() : rig.pitch->ringClearedForTest();
}

// The nonzero floats in the subject's ring.
int ringNonzero(const Rig& rig, Card subject) {
  int stale = 0;
  if (subject == Card::RingMod) {
    for (const float v : rig.ringMod->ringForTest()) stale += v != 0.0f;
  } else {
    for (const float v : rig.pitch->ring()) stale += v != 0.0f;
  }
  return stale;
}

SwitchRun switchRun(const Signal& x, int offBlocks, Card subject) {
  SwitchRun r;
  auto rig = std::make_unique<Rig>();
  ChainSnapshot snap = trackerSnapshot(subject);
  rig->fades.settle(snap.slotOn);
  std::array<float, kBlk> a{}, scratch{};
  int blk = 0;
  uint32_t cleared = ringCleared(*rig, subject);
  for (size_t i = 0; i + kBlk <= x.size(); i += kBlk, ++blk) {
    // Each pair of flips starts at kFirstFlip + k * kPair: the toggle goes off there and back on offBlocks later.
    bool switchOn = false;
    const int phase = blk - kFirstFlip;
    if (phase >= 0 && phase / kPair < kFlips / 2) {
      const int in = phase % kPair;
      if (in == 0) snap.slotOn[kSubjectSlot] = false;
      if (in == offBlocks) {
        snap.slotOn[kSubjectSlot] = true;
        switchOn = true;
      }
    }
    std::copy(x.begin() + static_cast<long>(i), x.begin() + static_cast<long>(i) + kBlk, a.begin());
    ChainCycles cyc;
    runChain(rig->engines, rig->fades, a.data(), scratch.data(), kBlk, snap, nowNs, cyc);
    r.ns.push_back(static_cast<double>(cyc.slot[kSubjectSlot]));
    const uint32_t now = ringCleared(*rig, subject);
    r.maxClearedInBlock = std::max(r.maxClearedInBlock, now - cleared);
    if (switchOn) {
      r.clearedAtSwitchOn += now - cleared;
      // The ring holds the new call and nothing else: a clear that stopped short would leave old samples.
      r.ringNotClear += ringNonzero(*rig, subject) > kBlk;
    }
    cleared = now;
    ++r.blocks;
  }
  return r;
}

bool testSwitchOn(const std::vector<Signal>& clips, Card subject, const char* label) {
  const int restFloats = subject == Card::RingMod ? cv::kRingModRestFloats : cv::PitchFx::kRestClearFloats;
  char name[120];
  Signal x;
  for (const Signal& c : clips) x.insert(x.end(), c.begin(), c.end());
  while (x.size() < static_cast<size_t>(kBlk) * (kFirstFlip + kFlips * kFlipEvery + 100)) x.insert(x.end(), x.begin(), x.end());

  // A long rest between the flips, as the stress run leaves: the ring is clear when the stage starts.
  bool ok = true;
  SwitchRun runs[kPasses];
  std::vector<double> best;
  double settled = 0.0, firstOn = 0.0, after = 0.0;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    best.clear();
    for (int pass = 0; pass < kPasses; ++pass) {
      runs[pass] = switchRun(x, kFlipEvery, subject);
      if (pass == 0) best = runs[0].ns;
      for (size_t b = 0; b < best.size(); ++b) best[b] = std::min(best[b], runs[pass].ns[b]);
    }
    settled = firstOn = after = 0.0;
    for (size_t b = 0; b < best.size(); ++b) {
      const int phase = static_cast<int>(b) - kFirstFlip;
      if (phase < 0 || phase / kPair >= kFlips / 2) {
        // before the first flip and after the last pair, once the first enable has settled
        if (static_cast<int>(b) > 2 * kSettleBlocks) settled = std::max(settled, best[b]);
        continue;
      }
      const int in = phase % kPair;
      if (in == kFlipEvery) firstOn = std::max(firstOn, best[b]);
      if (in > kFlipEvery && in < kFlipEvery + kSettleBlocks) after = std::max(after, best[b]);
      if (in >= kFlipEvery + kSettleBlocks) settled = std::max(settled, best[b]);
    }
    if (std::max(firstOn, after) <= settled * kRatioMargin + kTickMarginNs) break;
  }
  const bool timed = std::max(firstOn, after) <= settled * kRatioMargin + kTickMarginNs;
  std::snprintf(name, sizeof name, "%s: switch-on after a rest: no block above the settled worst", label);
  ok &= report(name, timed,
               "first block %.0f ns, next %.0f blocks' worst %.0f ns, settled worst %.0f ns (x1.12 + 120 ns allowed)",
               firstOn, static_cast<double>(kSettleBlocks), after, settled);

  const SwitchRun& r = runs[0];
  std::snprintf(name, sizeof name, "%s: switch-on after a rest: the ring is already clear", label);
  ok &= report(name, r.clearedAtSwitchOn == 0 && r.ringNotClear == 0,
               "floats the switch-on blocks cleared %.0f (0), switch-ons that left old samples in the ring %.0f (0)",
               static_cast<double>(r.clearedAtSwitchOn), static_cast<double>(r.ringNotClear));
  std::snprintf(name, sizeof name, "%s: a stopped card clears a slice a block", label);
  ok &= report(name, r.maxClearedInBlock <= static_cast<uint32_t>(restFloats) && r.maxClearedInBlock > 0,
               "most floats cleared in one block %.0f (<= %.0f, > 0)", static_cast<double>(r.maxClearedInBlock),
               static_cast<double>(restFloats));

  // A quick flip back, before the rest has cleared the ring: the reset clears the rest, and the ring is
  // still clear of old samples when the card starts.
  const SwitchRun quick = switchRun(x, kShortRest, subject);
  std::snprintf(name, sizeof name, "%s: switch-on after a short rest: the ring is still clear", label);
  ok &= report(name,
               quick.clearedAtSwitchOn > 0 && quick.ringNotClear == 0,
               "floats the switch-on blocks cleared %.0f (> 0), switch-ons that left old samples in the ring %.0f (0)",
               static_cast<double>(quick.clearedAtSwitchOn), static_cast<double>(quick.ringNotClear));
  return ok;
}

}  // namespace

int main() {
  bool whole = true;
  const std::vector<Signal> clips = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (clips.empty() || !whole) {
    std::printf("SKIP pitch_spikes  samples/ missing or incomplete\n");
    return 0;
  }
  bool ok = true;
  ok &= testOnsetSpread(clips);
  ok &= testSwitchOn(clips, Card::Growl, "Deepthroat");
  ok &= testSwitchOn(clips, Card::RingMod, "Ring mod");
  std::printf("%s pitch_spikes\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
