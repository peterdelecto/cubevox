#include "core/layout.h"

#include <stdio.h>

#include "core/costs.h"
#include "core/registry.h"

Layout defaultLayout() {
  return {{Card::InputGate, Card::Autotune, Card::Octave, Card::Unison,
           Card::Slapback, Card::Distortion, Card::Gate, Card::ReverbSpring}};
}

Layout crowdTestLayout() {
  Layout layout = defaultLayout();
  layout.card[1] = Card::Crowd;
  return layout;
}

namespace {

// Indexed by SwapSlot.
constexpr std::array<SwapCards, static_cast<size_t>(SwapSlot::Count)> kSwapCards = {{
    {3, {Card::Octave, Card::Formant, Card::FormantFollower}, {"OCTAVE", "FORMANT", "APRAXIA"}},
    {2, {Card::Unison, Card::Crowd, Card::Empty}, {"UNISON", "MOB", ""}},
    {2, {Card::Distortion, Card::Growl, Card::Empty}, {"DISTORTION", "DEEPTHROAT", ""}},
    {3, {Card::Gate, Card::Lofi, Card::Bloom}, {"GATE", "LO-FI", "CHATTER"}},
    {2, {Card::ReverbSpring, Card::ReverbChasm, Card::Empty}, {"SPRING", "CHASM", ""}},
}};

// The place of `c` among `which`'s cards, -1 when it is not one of them.
int swapIndex(SwapSlot which, Card c) {
  const SwapCards& set = swapCards(which);
  for (size_t i = 0; i < set.count; ++i) {
    if (set.card[i] == c) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

const SwapCards& swapCards(SwapSlot which) { return kSwapCards[static_cast<size_t>(which)]; }

int swapSlotOf(const Layout& layout, SwapSlot which) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (swapIndex(which, layout.card[s]) >= 0) return s;
  }
  return -1;
}

Card nextSwapCard(SwapSlot which, Card current) {
  const SwapCards& set = swapCards(which);
  return set.card[static_cast<size_t>(swapIndex(which, current) + 1) % set.count];
}

const char* swapWord(const Layout& layout, SwapSlot which) {
  const int slot = swapSlotOf(layout, which);
  const int index = slot < 0 ? 0 : swapIndex(which, layout.card[static_cast<size_t>(slot)]);
  return swapCards(which).word[static_cast<size_t>(index)];
}

int reverbSlot(const Layout& layout) { return swapSlotOf(layout, SwapSlot::Reverb); }

int unisonCrowdSlot(const Layout& layout) { return swapSlotOf(layout, SwapSlot::UnisonMob); }

int octaveFormantSlot(const Layout& layout) { return swapSlotOf(layout, SwapSlot::OctaveFormant); }

int distortionGrowlSlot(const Layout& layout) { return swapSlotOf(layout, SwapSlot::DistortionDeepthroat); }

int slot7CardSlot(const Layout& layout) { return swapSlotOf(layout, SwapSlot::Slot7); }

const char* layoutDisplayName(const char* stored) { return stored != nullptr && stored[0] != '\0' ? stored : "Default"; }

namespace {

LayoutCheck fail(LayoutError error, int slot, Card card, int percent) {
  LayoutCheck c;
  c.error = error;
  c.slot = slot;
  c.card = card;
  c.percent = percent;
  return c;
}

}  // namespace

LayoutCost layoutCost(const Layout& layout) {
  LayoutCost sum;
  sum.avg = costs::kFixed.avg;
  sum.worst = costs::kFixed.worst;
  // Autotune pays one tracker. A Formant voice directly behind it (only Empty slots between) reads that
  // tracker, and so do Mob and Deepthroat anywhere behind it. A Formant voice anywhere else, and a Mob
  // or Deepthroat card with no Autotune ahead, run a tracker of their own.
  int trackers = 0;
  bool autotune = false;  // Autotune sits ahead of the slot being counted
  bool adjacent = false;  // and nothing but Empty slots sits between it and that slot
  for (const Card c : layout.card) {
    sum.avg += cardDesc(c).cost.avg;
    sum.worst += cardDesc(c).cost.worst;
    if (c == Card::Empty) continue;
    const bool formant = c == Card::Formant || c == Card::FormantFollower;
    if (c == Card::Autotune) {
      ++trackers;
      autotune = true;
      adjacent = true;
      continue;
    }
    if (formant) {
      if (!(autotune && adjacent)) ++trackers;
    } else if ((engineOf(c) == Engine::Crowd || c == Card::Growl) && !autotune) {
      ++trackers;
    }
    adjacent = false;
  }
  sum.avg += trackers * costs::kTracker.avg;
  sum.worst += trackers * costs::kTracker.worst;
  return sum;
}

LayoutCheck layoutValid(const Layout& layout) {
  int reverbs = 0;
  uint32_t placed = 0;  // bit per non-Empty card already seen
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = layout.card[s];
    if (static_cast<int>(c) >= kCardCount) return fail(LayoutError::BadCard, s, Card::Empty, 0);
    if (c != Card::Empty) {
      // Formant and the Formant follower play one voice, so a layout holds one of them at most.
      const Card voice = c == Card::FormantFollower ? Card::Formant : c;
      const uint32_t bit = 1u << static_cast<int>(voice);
      if (placed & bit) return fail(LayoutError::SameCardTwice, s, c, 0);
      placed |= bit;
    }
    if (cardDesc(c).group == Group::Reverb) ++reverbs;
  }
  if (reverbs > 1) return fail(LayoutError::TwoReverbs, 0, Card::Empty, 0);
  if (reverbs == 0) return fail(LayoutError::NoReverb, 0, Card::Empty, 0);
#ifndef CUBEVOX_CROWDTEST
  // The test image places Crowd past the budget lines on purpose.
  const LayoutCost cost = layoutCost(layout);
  if (cost.avg > costs::kBudgetAvgPercent) return fail(LayoutError::OverAvg, 0, Card::Empty, cost.avg);
  if (cost.worst > costs::kBudgetWorstPercent) return fail(LayoutError::OverWorst, 0, Card::Empty, cost.worst);
#endif
  return LayoutCheck();
}

Recovery recoverLayout(Layout& layout) {
  Recovery r;
  r.why = layoutValid(layout);
  if (r.why.ok()) return r;
  layout = defaultLayout();
  r.replaced = true;
  return r;
}

void layoutReason(const LayoutCheck& check, char* buf, size_t size) {
  if (size == 0) return;
  switch (check.error) {
    case LayoutError::None: buf[0] = '\0'; break;
    case LayoutError::BadCard: snprintf(buf, size, "card id out of range in slot %d", check.slot + 1); break;
    case LayoutError::SameCardTwice: snprintf(buf, size, "same card twice: %s", cardDesc(check.card).id); break;
    case LayoutError::TwoReverbs: snprintf(buf, size, "two reverb cards"); break;
    case LayoutError::NoReverb: snprintf(buf, size, "no reverb card"); break;
    case LayoutError::OverAvg: snprintf(buf, size, "over CPU budget (mean %d %%)", check.percent); break;
    case LayoutError::OverWorst: snprintf(buf, size, "over CPU budget (worst %d %%)", check.percent); break;
  }
}
