// Checks the layout change taken at once: a card swapped in a slot that is off changes nothing heard, so
// the output does not fade, the changed engines clear one per chain block, and a slot whose engine waits
// for its clear stays at the dry input until it clears. Every other change keeps the fade.

#include "support.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "core/block_flow.h"
#include "core/chain_run.h"
#include "core/compose.h"
#include "core/layout.h"
#include "core/layout_fade.h"
#include "core/panel.h"
#include "core/settings.h"

namespace {

constexpr int kN = cv::kBlock;
using Block = std::array<float, kN>;

// Two partials of a sung note, so the trackers find a pitch.
Block voice(int block) {
  constexpr double kPi = 3.14159265358979323846;
  Block x{};
  for (int i = 0; i < kN; ++i) {
    const double t = static_cast<double>(block * kN + i) / cv::kSampleRate;
    x[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 196.0 * t) + 0.1 * std::sin(2.0 * kPi * 392.0 * t));
  }
  return x;
}

bool sameBits(const Block& a, const Block& b) { return std::memcmp(a.data(), b.data(), sizeof a) == 0; }

int popcount(uint32_t v) { return static_cast<int>(std::bitset<32>(v).count()); }

uint32_t bit(Engine e) { return 1u << static_cast<int>(e); }

struct StillClock {
  uint32_t operator()() { return 0; }
};

// Every knob at 0.5 and the toggles as given, CLEANUP EQ at its default.
ChainSnapshot snapshotWith(const Layout& layout, const std::array<bool, kSlotCount>& on) {
  PanelReading panel;
  panel.toggle = on;
  for (auto& slot : panel.knob) slot.fill(0.5f);
  return composeSnapshot(layout, panel, Settings(), BenchOverrides());
}

std::array<bool, kSlotCount> allOnBut(int off) {
  std::array<bool, kSlotCount> on{};
  on.fill(true);
  if (off >= 0) on[static_cast<size_t>(off)] = false;
  return on;
}

// The board's audio block on the host: syncSnapshot, the fade's swap and BlockFlow::run, as audio.cpp
// runs them, with a record of what the blocks did.
struct Box {
  std::unique_ptr<cv::AutotuneFx> autotune = std::make_unique<cv::AutotuneFx>();
  std::unique_ptr<cv::OctaveFx> octave = std::make_unique<cv::OctaveFx>();
  std::unique_ptr<cv::Unison> unison = std::make_unique<cv::Unison>();
  std::unique_ptr<cv::Distortion> distortion = std::make_unique<cv::Distortion>();
  ChainEngines engines{*autotune, *octave, *unison, *distortion};
  BlockFlow flow;
  ChainSnapshot held;
  ChainSnapshot next;
  bool pending = false;
  bool faded = false;    // a block's gain left 1, or the fade swapped
  int mostClears = 0;    // the most engines one block cleared

  explicit Box(const ChainSnapshot& first) : held(first) { flow.settleSlots(first.slotOn); }

  // syncSnapshot: the same layout and a change nobody hears are taken at once, any other waits for the fade.
  void publish(const ChainSnapshot& pub) {
    const bool differs = pub.layout != held.layout;
    pending = differs && !flow.adoptAtOnce(held, pub);
    if (pending) next = pub;
    else held = pub;
  }

  Block run(const Block& in) {
    const BlockFlow::Start start = flow.begin(false, pending);
    if (start.fade.swap) {
      flow.adopt(held, next);
      held = next;
      pending = false;
    }
    faded = faded || start.fade.swap || start.fade.from != 1.0f || start.fade.to != 1.0f;
    const int before = popcount(flow.enginesWaiting());
    Block a = in;
    Block b{};
    Block out{};
    flow.run(engines, held, start, a.data(), b.data(), false, StillClock(),
             [&out](int i, float v) { out[static_cast<size_t>(i)] = v; });
    mostClears = std::max(mostClears, before - popcount(flow.enginesWaiting()));
    return out;
  }
};

// Runs a box that takes `after` at block `at` beside one that never changes, on the same input, and
// says whether every output sample matched.
struct Pair {
  bool same = true;
  bool faded = false;
  int mostClears = 0;
  uint32_t leftWaiting = 0;
};

Pair runBeside(const ChainSnapshot& before, const ChainSnapshot& after, int at, int blocks) {
  auto swapped = std::make_unique<Box>(before);
  auto plain = std::make_unique<Box>(before);
  Pair p;
  for (int k = 0; k < blocks; ++k) {
    if (k == at) swapped->publish(after);
    const Block in = voice(k);
    p.same = sameBits(swapped->run(in), plain->run(in)) && p.same;
  }
  p.faded = swapped->faded;
  p.mostClears = swapped->mostClears;
  p.leftWaiting = swapped->flow.enginesWaiting();
  return p;
}

// Every card the menu's SLOT rows offer, swapped into the default layout's slot with that slot off: the
// output across the swap is the same, bit for bit, as a box that never swapped, and the fade never starts.
void checkOffSlotSwapsUnheard() {
  const Layout base = defaultLayout();
  for (int row = 0; row < static_cast<int>(SwapSlot::Count); ++row) {
    const SwapSlot which = static_cast<SwapSlot>(row);
    const int slot = swapSlotOf(base, which);
    const SwapCards& cards = swapCards(which);
    for (size_t k = 1; k < cards.count; ++k) {
      Layout swappedLayout = base;
      swappedLayout.card[static_cast<size_t>(slot)] = cards.card[k];
      const Pair p = runBeside(snapshotWith(base, allOnBut(slot)), snapshotWith(swappedLayout, allOnBut(slot)), 60, 160);
      char what[160];
      std::snprintf(what, sizeof what, "swap at once: %s into off slot %d plays the same samples as no swap, with no fade",
                    cards.word[k], slot + 1);
      check(p.same && !p.faded, what);
      std::snprintf(what, sizeof what, "swap at once: %s into off slot %d clears its engines, at most %d a block",
                    cards.word[k], slot + 1, kResetsPerChainBlock);
      check(p.leftWaiting == 0 && p.mostClears >= 1 && p.mostClears <= kResetsPerChainBlock, what);
    }
  }
}

// True when publishing `after` over `before` starts the layout fade.
bool fades(const ChainSnapshot& before, const ChainSnapshot& after) {
  auto box = std::make_unique<Box>(before);
  for (int k = 0; k < 20; ++k) box->run(voice(k));
  box->publish(after);
  for (int k = 20; k < 40; ++k) box->run(voice(k));
  return box->faded;
}

// A swap in a slot that is on fades as before, and so does a swap in an off slot whose card an on slot
// reads: Autotune ahead of Mob, the Input gate ahead of Chatter, and a card placed between Autotune and the
// Formant card it shares a grain cut with.
void checkHeardSwapsFade() {
  const Layout base = defaultLayout();
  Layout formant = base;
  formant.card[2] = Card::Formant;
  check(fades(snapshotWith(base, allOnBut(-1)), snapshotWith(formant, allOnBut(-1))),
        "swap at once: a swap in a slot that is on keeps the fade");

  Layout mob = base;
  mob.card[3] = Card::Crowd;
  Layout mobAlone = mob;
  mobAlone.card[1] = Card::Empty;
  check(fades(snapshotWith(mob, allOnBut(1)), snapshotWith(mobAlone, allOnBut(1))),
        "swap at once: taking Autotune out of its off slot keeps the fade while Mob behind it is on");
  std::array<bool, kSlotCount> mobOff = allOnBut(1);
  mobOff[3] = false;
  check(!fades(snapshotWith(mob, mobOff), snapshotWith(mobAlone, mobOff)),
        "swap at once: the same change with Mob off too is taken at once");

  Layout chatter = base;
  chatter.card[6] = Card::Bloom;
  Layout chatterAlone = chatter;
  chatterAlone.card[0] = Card::Empty;
  check(fades(snapshotWith(chatter, allOnBut(0)), snapshotWith(chatterAlone, allOnBut(0))),
        "swap at once: taking the Input gate out of its off slot keeps the fade while Chatter is on");

  Layout pair = base;
  pair.card[2] = Card::Empty;
  pair.card[3] = Card::Formant;
  Layout split = pair;
  split.card[2] = Card::Octave;
  check(fades(snapshotWith(pair, allOnBut(2)), snapshotWith(split, allOnBut(2))),
        "swap at once: a card placed in the off slot between Autotune and Formant keeps the fade");
}

// The layout-level rule on its own: what each card reads, and the engines that clear.
void checkChangeHeard() {
  const Layout base = defaultLayout();
  Layout mob = base;
  mob.card[3] = Card::Crowd;
  check(enginesReadBy(mob, 3) == (bit(Engine::Crowd) | bit(Engine::Autotune)) &&
            enginesReadBy(base, 3) == bit(Engine::Unison),
        "swap at once: Mob reads its own engine and Autotune's, Unison its own");
  Layout chatter = base;
  chatter.card[6] = Card::Bloom;
  check(enginesReadBy(chatter, 6) == (bit(Engine::Bloom) | bit(Engine::InputGate)),
        "swap at once: Chatter reads the Input gate's engine when one is placed");
  const uint8_t allLive = 0xFF;
  check(changeHeard(base, base, allLive, bit(Engine::Unison)) &&
            !changeHeard(base, base, static_cast<uint8_t>(allLive & ~(1u << 3)), bit(Engine::Unison)),
        "swap at once: an engine still waiting for its clear is heard only while its slot is live");
}

// What slot `slot` did after a change taken at once and its toggle going up a block later.
struct ToggleRun {
  int heldBlocks = 0;   // blocks with the toggle up that the slot spent at the dry input, its engine waiting
  bool rises = true;    // once the engine cleared, the slot rose one fade level a block, never before
  bool faded = false;
  int mostClears = 0;
};

// Publishes `swap` (cards changed in slots that are off) at block 60 and `toggled` (the same layout with
// slot `slot`'s toggle up) at block 61, and follows slot `slot` while engine `e` waits for its clear.
ToggleRun toggleAfterSwap(const ChainSnapshot& before, const ChainSnapshot& swap, const ChainSnapshot& toggled, int slot,
                          Engine e) {
  auto box = std::make_unique<Box>(before);
  for (int k = 0; k < 60; ++k) box->run(voice(k));
  box->publish(swap);
  box->run(voice(60));
  box->publish(toggled);
  ToggleRun r;
  int expect = 0;
  for (int k = 61; k < 61 + kSlotFadeBlocks + 6; ++k) {
    box->run(voice(k));
    const bool waiting = (box->flow.enginesWaiting() & bit(e)) != 0;
    if (waiting && box->flow.slotLevel(slot) == 0) ++r.heldBlocks;
    if (!waiting) expect = std::min(expect + 1, kSlotFadeBlocks);
    r.rises = r.rises && box->flow.slotLevel(slot) == expect;
  }
  r.rises = r.rises && expect == kSlotFadeBlocks;
  r.faded = box->faded;
  r.mostClears = box->mostClears;
  return r;
}

// A swapped slot's toggle goes up in the block after the swap. The slot stays at the dry input until its
// engine has cleared, then fades in over kSlotFadeBlocks as a plain toggle does, with no layout fade. One
// card swapped leaves two engines to clear, so the new one is clear by the time the toggle is read. A change
// that swaps Unison for Mob and Distortion for Deepthroat at once leaves four: Unison, Distortion, Mob, then
// Deepthroat clear in engine order, so Deepthroat's slot waits two blocks with its toggle up.
void checkToggleAfterSwap() {
  const Layout base = defaultLayout();
  Layout mob = base;
  mob.card[3] = Card::Crowd;
  const ToggleRun one =
      toggleAfterSwap(snapshotWith(base, allOnBut(3)), snapshotWith(mob, allOnBut(3)), snapshotWith(mob, allOnBut(-1)), 3, Engine::Crowd);
  check(one.rises && !one.faded && one.mostClears <= kResetsPerChainBlock,
        "swap at once: Mob switched on the block after its swap fades in once its engine is clear, with no layout fade");

  Layout both = mob;
  both.card[5] = Card::Growl;
  std::array<bool, kSlotCount> twoOff = allOnBut(3);
  twoOff[5] = false;
  const ToggleRun two = toggleAfterSwap(snapshotWith(base, twoOff), snapshotWith(both, twoOff),
                                        snapshotWith(both, allOnBut(3)), 5, Engine::Growl);
  check(two.heldBlocks == 2, "swap at once: Deepthroat's slot stays at the dry input with its toggle up while its engine waits");
  check(two.rises && !two.faded && two.mostClears <= kResetsPerChainBlock,
        "swap at once: then it fades in one level a block, no layout fade, one engine clear a block at most");
}

// Runs Octave's slot from off: placed by a swap at block 50 (`swap`) or placed from the start, its toggle
// up at block 51. `stale` writes a loud ring into the Octave engine at blocks 40 to 49.
std::array<Block, 100> octaveFromOff(bool swap, bool stale) {
  Layout formant = defaultLayout();
  formant.card[2] = Card::Formant;
  const Layout octave = defaultLayout();
  auto box = std::make_unique<Box>(snapshotWith(swap ? formant : octave, allOnBut(2)));
  Block loud{};
  for (int i = 0; i < kN; ++i) loud[static_cast<size_t>(i)] = (i & 8) ? 0.9f : -0.9f;
  std::array<Block, 100> out{};
  for (int k = 0; k < 150; ++k) {
    if (stale && k >= 40 && k < 50) box->octave->idle(loud.data(), kN);
    if (swap && k == 50) box->publish(snapshotWith(octave, allOnBut(2)));
    if (k == 51) box->publish(snapshotWith(octave, allOnBut(-1)));
    const Block got = box->run(voice(k));
    if (k >= 50) out[static_cast<size_t>(k - 50)] = got;
  }
  return out;
}

// Whatever the Octave engine held before it was swapped in, none of it is heard: the output matches a box
// whose engine held nothing, bit for bit. The same stale ring in a slot placed with no swap is heard,
// which shows the clear is what keeps it out.
void checkNoStaleAudio() {
  check(octaveFromOff(true, true) == octaveFromOff(true, false),
        "swap at once: a swapped-in card switched on plays nothing its engine held before the swap");
  check(octaveFromOff(false, true) != octaveFromOff(false, false),
        "swap at once: the same stale ring with no swap to clear it is heard");
}

}  // namespace

void checkSwapAtOnce() {
  checkChangeHeard();
  checkOffSlotSwapsUnheard();
  checkHeardSwapsFade();
  checkToggleAfterSwap();
  checkNoStaleAudio();
}
