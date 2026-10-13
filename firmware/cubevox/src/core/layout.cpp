#include "core/layout.h"

#include <stdio.h>

Layout defaultLayout() {
  return {{Card::InputGate, Card::Autotune, Card::Octave, Card::Unison,
           Card::Slapback, Card::Distortion, Card::Gate, Card::ReverbSpring}};
}

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

LayoutCheck layoutValid(const Layout& layout) {
  int avg = costs::kFixed.avg;
  int worst = costs::kFixed.worst;
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
    const CardDesc& d = cardDesc(c);
    avg += d.cost.avg;
    worst += d.cost.worst;
    if (d.group == Group::Reverb) ++reverbs;
  }
  if (reverbs > 1) return fail(LayoutError::TwoReverbs, 0, Card::Empty, 0);
  if (avg > costs::kBudgetAvgPercent) return fail(LayoutError::OverAvg, 0, Card::Empty, avg);
  if (worst > costs::kBudgetWorstPercent) return fail(LayoutError::OverWorst, 0, Card::Empty, worst);
  return LayoutCheck();
}

void layoutReason(const LayoutCheck& check, char* buf, size_t size) {
  switch (check.error) {
    case LayoutError::None: snprintf(buf, size, "%s", ""); break;
    case LayoutError::BadCard: snprintf(buf, size, "card id out of range in slot %d", check.slot + 1); break;
    case LayoutError::SameCardTwice: snprintf(buf, size, "same card twice: %s", cardDesc(check.card).id); break;
    case LayoutError::TwoReverbs: snprintf(buf, size, "two reverb cards"); break;
    case LayoutError::OverAvg: snprintf(buf, size, "over CPU budget (mean %d %%)", check.percent); break;
    case LayoutError::OverWorst: snprintf(buf, size, "over CPU budget (worst %d %%)", check.percent); break;
  }
}
