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

int reverbSlot(const Layout& layout) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (layout.card[s] == Card::ReverbSpring || layout.card[s] == Card::ReverbChasm) return s;
  }
  return -1;
}

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
  bool tracker = false;  // Autotune or Crowd is placed, so the pitch tracker runs
  for (const Card c : layout.card) {
    sum.avg += cardDesc(c).cost.avg;
    sum.worst += cardDesc(c).cost.worst;
    tracker = tracker || c == Card::Autotune || c == Card::Crowd;
  }
  if (tracker) {
    sum.avg += costs::kTracker.avg;
    sum.worst += costs::kTracker.worst;
  }
  return sum;
}

LayoutCheck layoutValid(const Layout& layout) {
  int reverbs = 0;
  uint32_t placed = 0;  // bit per non-Empty card already seen
  for (int s = 0; s < kSlotCount; ++s) {
    const Card c = layout.card[s];
    if (static_cast<int>(c) >= kCardCount) return fail(LayoutError::BadCard, s, Card::Empty, 0);
    if (c != Card::Empty) {
      const uint32_t bit = 1u << static_cast<int>(c);
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
