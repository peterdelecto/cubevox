// Checks the layout change fade on the host. Covers the gain ramp, interrupted fades, the silent
// hold and the set of engines to reset.

#include "support.h"

#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/registry.h"

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

}  // namespace

void checkFade() {
  checkFadeRamp();
  checkFadeInterrupted();
  checkFadeHold();
  checkEnginesToReset();
}
